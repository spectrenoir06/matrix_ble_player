#include <Arduino.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <Mapping.h>
#include <Preferences.h>
#include <SpectreProtocol.h>
#include <spectre/ble_nimble.h>
#include <spectre/fs_arduino.h>
#include <spectre/ota_esp32.h>
#include <spectre/serial_link.h>

#include "Gif.hpp"
#include "Lua.hpp"
#include "Png.hpp"
#include "Protocol.hpp"
#include "Log.hpp"

#ifdef USE_SD
	#include "SD.h"
	#define filesystem SD
#endif
#ifdef USE_SPIFFS
	#include "SPIFFS.h"
	#define filesystem SPIFFS
#endif

#ifndef FIRMWARE_VERSION
	#define FIRMWARE_VERSION "1.0.0"
#endif
#ifndef BOARD_NAME
	#define BOARD_NAME "default"  // until set with matrix.board.set
#endif

using namespace spectre;

extern Preferences preferences;
extern uint8_t brightness;
extern void set_brightness(int b);
extern void set_all_pixel(uint8_t r, uint8_t g, uint8_t b, uint8_t w);
extern void print_progress(const char *str, uint32_t offset, uint32_t total_size);
extern void print_message(const char *str);

namespace {

Node node("matrix", FIRMWARE_VERSION);

struct LogEntry {
	uint8_t level;
	char text[120];
};
QueueHandle_t log_queue = nullptr;
// 2 KB frames: fewer packets and SD writes per upload. The rx buffer holds a
// full stream window (4 × 2 KB) plus one frame. More in flight does not help:
// the ESP32 BLE controller takes ~300 B per connection event (≈40 KB/s at
// the 7.5 ms interval set in main.cpp), measured with python/examples/bench.py.
NimBLELink ble(10240, 2048);
// Same protocol on the USB serial port, shared with the debug output (each
// frame is preceded by a 0x00, clients show the text as console). 1 KB frames:
// a stream window (4 × 1 KB) fits the 5 KB rx buffer set in main.cpp.
SerialLink serial(Serial, 1024, [](uint32_t baud) { Serial.updateBaudRate(baud); }, 115200);
uint32_t reboot_at = 0;

#ifdef USE_SD
ArduinoFileSystem fs(SD, [] { return SD.totalBytes(); }, [] { return SD.usedBytes(); }, true, "/sd");
#else  // SPIFFS: flat, no real directories
ArduinoFileSystem fs(SPIFFS, [] { return uint64_t(SPIFFS.totalBytes()); }, [] { return uint64_t(SPIFFS.usedBytes()); }, false,
                     "/spiffs");
#endif
FileModule files(node, fs);
OtaModule ota(node);  // firmware updates over BLE or serial (replaces BLEOTA)

// ── board: /matrix/<board>/{gif,png,lua} ─────────────────────────────────────

char board[33] = BOARD_NAME;
const char* const BOARD_SUBDIRS[] = {"gif", "png", "lua"};

bool valid_board(const char* name) {
	size_t n = strlen(name);
	if (n == 0 || n > 32)
		return false;
	for (const char* c = name; *c; c++)
		if (!islower(*c) && !isdigit(*c) && *c != '_' && *c != '-')
			return false;
	return true;
}

void board_make_dirs() {
	filesystem.mkdir("/matrix");
	filesystem.mkdir(board_dir());
	for (const char* sub : BOARD_SUBDIRS)
		filesystem.mkdir(board_dir(sub));
}

// Reads a str argument as an absolute path that stays inside the filesystem.
bool read_path(Request& req, char* path) {
	req.args.str(path, FileModule::PATH_MAX_LEN);
	return req.args.ok() && FileModule::validPath(path);
}

bool has_ext(const char* path, const char* ext) {
	size_t n = strlen(path), e = strlen(ext);
	return n >= e && strcasecmp(path + n - e, ext) == 0;
}

// Stop GIF and Lua and wait until they have released the shared arena.
bool stop_all() {
	bool lua = Lua::stop();
	bool gif = SpectreGif::stop();
	return lua && gif;
}

// ── sys ──────────────────────────────────────────────────────────────────────

void sys_info(Request& req) {
	req.result.str(FIRMWARE_VERSION).str(BUILD_GIT_COMMIT_HASH).u32(millis()).u32(esp_get_free_heap_size())
	    .u32(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

void sys_reboot(Request&) {
	reboot_at = millis() + 200;  // let the RES go out first
}

// ── light ────────────────────────────────────────────────────────────────────

void brightness_set(Request& req) {
	uint8_t v = req.args.u8();
	if (!req.args.ok())
		return req.fail(error::BadArgs);
	set_brightness(v);
	req.result.u8(brightness);
}

void brightness_step(Request& req) {
	int8_t d = req.args.i8();
	if (!req.args.ok())
		return req.fail(error::BadArgs);
	set_brightness(brightness + d);
	req.result.u8(brightness);
}

void fill(Request& req) {
	uint8_t r = req.args.u8(), g = req.args.u8(), b = req.args.u8();
	if (!req.args.ok())
		return req.fail(error::BadArgs);
	if (!stop_all())
		return req.fail(error::Busy);
	set_all_pixel(r, g, b, 0);
}

void clear(Request& req) {
	if (!stop_all())
		return req.fail(error::Busy);
	set_all_pixel(0, 0, 0, 0);
}

// ── matrix ───────────────────────────────────────────────────────────────────

void matrix_info(Request& req) {
	req.result.u16(MATRIX_WIDTH).u16(MATRIX_HEIGHT).u16(V_MATRIX_WIDTH).u16(V_MATRIX_HEIGHT).str(board);
}

void matrix_board_set(Request& req) {
	char name[33];
	req.args.str(name, sizeof(name));
	if (!req.args.ok() || !valid_board(name))
		return req.fail(error::BadArgs, "board: 1-32 of a-z 0-9 _ -");
	strcpy(board, name);
	preferences.putString("board", board);
	preferences.remove("anim");  // the saved file belongs to the previous board
	board_make_dirs();
	Log.printf("board: %s\n", board);
}

void matrix_play(Request& req) {
	char path[FileModule::PATH_MAX_LEN];
	if (!read_path(req, path))
		return req.fail(error::BadArgs);
	if (uint16_t err = play_file(path))
		return req.fail(err);
	preferences.putString("anim", path);
}

void matrix_stop(Request& req) {
	if (!stop_all())
		req.fail(error::Busy);
}

}  // namespace

void board_begin() {
	String saved = preferences.getString("board", BOARD_NAME);
	if (valid_board(saved.c_str()))
		strcpy(board, saved.c_str());
	board_make_dirs();
	Log.printf("board: %s (%s)\n", board, board_dir().c_str());
}

String board_dir(const char* sub) {
	String dir = String("/matrix/") + board;
	if (sub)
		dir += String("/") + sub;
	return dir;
}

uint16_t play_file(const char* path) {
	bool gif = has_ext(path, ".gif"), png = has_ext(path, ".png"), lua = has_ext(path, ".lua");
	if (!gif && !png && !lua)
		return error::BadArgs;
	if (!filesystem.exists(path))
		return error::NotFound;

	// GIF, PNG and Lua share one RAM arena: the current one must be done first
	if (!stop_all())
		return error::Busy;
	if (gif) {
		SpectreGif::play(path);
	} else if (png) {
		if (uint16_t err = SpectrePng::show(path))
			return err;
	} else {
		File f = filesystem.open(path);
		String script = f.readString();
		f.close();
		const char* slash = strrchr(path, '/');
		Lua::run_script(script, slash ? slash + 1 : path);
	}
	Log.printf("play %s\n", path);
	return 0;
}

void protocol_begin(NimBLEServer* server) {
	node.streamWindow = 4;
	node.addModule("light");
	node.addModule("matrix");

	node.on(method::SysInfo, sys_info);
	node.on(method::SysReboot, sys_reboot);
	node.on(method::LightBrightnessSet, brightness_set);
	node.on(method::LightBrightnessStep, brightness_step);
	node.on(method::LightFill, fill);
	node.on(method::LightClear, clear);
	// file.* comes from spectre::FileModule; the matrix only reacts to it
	files.onModify = [](const char* path) {
		if (SpectreGif::isPlaying(path))  // its file is about to change
			SpectreGif::stop();
	};
	static uint32_t upload_start = 0, progress_shown = 0, upload_size = 0;
	files.onUploadStart = [](const char*, uint32_t size) {
		stop_all();  // the display shows the upload progress
		upload_size = size;
		upload_start = millis();
		progress_shown = 0;
	};
	files.onUploadProgress = [](uint32_t done, uint32_t total) {
		uint32_t now = millis();
		if (now - progress_shown >= 250 || done == total) {
			progress_shown = now;
			print_progress("upload", done, total);
		}
	};
	// firmware update: same progress bar, then a reboot into the new firmware
	ota.onStart = [](uint32_t size) {
		stop_all();
		upload_start = millis();
		progress_shown = 0;
		Log.printf("firmware update: %u B\n", size);
	};
	ota.onProgress = [](uint32_t done, uint32_t total) {
		uint32_t now = millis();
		if (now - progress_shown >= 250 || done == total) {
			progress_shown = now;
			print_progress("update", done, total);
		}
	};
	ota.onEnd = [](bool ok, const char* err) {
		if (ok) {
			Log.printf("firmware update ok in %u ms, rebooting\n", millis() - upload_start);
			print_message("Update OK\nrebooting");
		} else {
			Log.line(LogLevel::Error, (String("firmware update failed: ") + err).c_str());
			print_message("Update\nfailed");
		}
	};

	files.onUploadEnd = [](const char* path, bool ok) {
		uint32_t ms = millis() - upload_start;
		Log.printf("upload %s %s: %u B in %u ms (%u B/s)\n", path, ok ? "ok" : "FAILED", upload_size, ms,
		              ms ? upload_size * 1000 / ms : 0);
	};
	node.on(method::MatrixInfo, matrix_info);
	node.on(method::MatrixBoardSet, matrix_board_set);
	node.on(method::MatrixPlay, matrix_play);
	node.on(method::MatrixStop, matrix_stop);

	log_queue = xQueueCreate(16, sizeof(LogEntry));
	ble.begin(server);
	node.attach(ble);
	node.attach(serial);
}

bool protocol_serial_active() {
	return serial.active();
}

void protocol_log(LogLevel level, const char* text) {
	if (!log_queue)
		return;
	LogEntry e;
	e.level = static_cast<uint8_t>(level);
	strlcpy(e.text, text, sizeof(e.text));
	xQueueSend(log_queue, &e, 0);
}

void protocol_loop() {
	ble.poll();
	serial.poll();
	node.tick(millis());
	ota.loop();
	LogEntry e;
	while (xQueueReceive(log_queue, &e, 0) == pdTRUE) {
		uint8_t buf[2 + sizeof(e.text)];
		Writer w(buf, sizeof(buf));
		w.u8(e.level).str(e.text);
		node.emit(method::SysLog, w);
	}
	if (reboot_at && millis() > reboot_at)
		ESP.restart();
}

void protocol_disconnected() {
	ble.disconnected();
}
