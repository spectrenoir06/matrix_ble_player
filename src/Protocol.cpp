#include <Arduino.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <Mapping.h>
#include <Preferences.h>
#include <SpectreProtocol.h>
#include <spectre/ble_nimble.h>
#include <spectre/fs_arduino.h>
#include <spectre/ota_esp32.h>
#include <spectre/serial_link.h>
#include <esp_wifi.h>
#include <spectre/wifi_esp32.h>
#include <spectre/ws_async.h>

#include "Clock.hpp"
#include "Gif.hpp"
#include "Lua.hpp"
#include "Playlist.hpp"
#include "Png.hpp"
#include "Protocol.hpp"
#include "Log.hpp"

#include "Layout.hpp"
#include "Storage.hpp"
#include <SD.h>
#include <SPIFFS.h>

#ifndef FIRMWARE_VERSION
	#define FIRMWARE_VERSION "1.0.0"
#endif
#ifndef BOARD_NAME
	#define BOARD_NAME "default"  // until set with matrix.board.set
#endif

using namespace spectre;

extern Preferences preferences;
extern uint8_t brightness;
extern uint8_t color_depth, color_depth_max;
extern bool set_color_depth(uint8_t bits);
extern const uint32_t MIN_FREE_HEAP;
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

// the SD card, else SPIFFS (flat: no real directories); the one mounted at
// boot (Storage.cpp), FileModule calls go to it
ArduinoFileSystem sd_fs(SD, storage_total, storage_used, true, "/sd");
ArduinoFileSystem spiffs_fs(SPIFFS, storage_total, storage_used, false, "/spiffs");
struct MountedFileSystem : FileSystem {
	FileSystem& fs() { return storage_is_sd ? static_cast<FileSystem&>(sd_fs) : spiffs_fs; }
	bool stat(const char* path, FileStat& st) override { return fs().stat(path, st); }
	bool list(const char* dir, const ListFn& fn) override { return fs().list(dir, fn); }
	bool mkdir(const char* path) override { return fs().mkdir(path); }
	bool remove(const char* path) override { return fs().remove(path); }
	bool rmdir(const char* path) override { return fs().rmdir(path); }
	bool rename(const char* from, const char* to) override { return fs().rename(from, to); }
	bool usage(uint64_t& total, uint64_t& used) override { return fs().usage(total, used); }
	std::unique_ptr<FileReader> openRead(const char* path) override { return fs().openRead(path); }
	std::unique_ptr<FileWriter> openWrite(const char* path) override { return fs().openWrite(path); }
} mounted_fs;
FileModule files(node, mounted_fs);
OtaModule ota(node);  // firmware updates over BLE or serial (replaces BLEOTA)

bool ble_on = false;

}  // namespace

// WiFi buffers: Arduino lets WiFi take up to 32 RX + 32 TX buffers of 1.6 KB
// under heavy traffic (~100 KB): on this board the heap ran out and WiFi got
// stuck. Fewer of them (linked with -Wl,--wrap=esp_wifi_init): a bit slower,
// never more than ~30 KB.
extern "C" esp_err_t __real_esp_wifi_init(const wifi_init_config_t* config);
extern "C" esp_err_t __wrap_esp_wifi_init(const wifi_init_config_t* config) {
	wifi_init_config_t cfg = *config;
	cfg.dynamic_rx_buf_num = 10;
	cfg.dynamic_tx_buf_num = 8;
	cfg.rx_ba_win = 6 < cfg.dynamic_rx_buf_num ? 6 : cfg.dynamic_rx_buf_num;
	return __real_esp_wifi_init(&cfg);
}

namespace {

// WiFi (network set with wifi.set, saved): the app from the SD card on
// http://<board>.local/ and the protocol on ws://<board>.local/ws.
// RAM is too short for WiFi next to BLE (no PSRAM), and next to the bigger
// layouts' displays (layout_depth_max): the board runs either
// - BLE mode: no network saved; wifi.set saves one and reboots into WiFi;
// - WiFi mode: a network saved; BLE off. Not connected 20 s after boot:
//   reboots into BLE mode for that boot only (fix the network over BLE).
// wifi.set "" (forget the network) reboots into BLE mode.
WifiModule wifi(node, "spectre-matrix");
constexpr uint32_t WIFI_JOIN_MS = 20000;
constexpr uint32_t SKIP_WIFI = 0x57494649;  // "WIFI"
RTC_NOINIT_ATTR uint32_t skip_wifi;  // survives ESP.restart(), not a power cycle
AsyncWebServer http(80);
// 1 KB packets: ≈ 7 KB of buffers (allocated once WiFi is up). 2 KB ones go
// ~60% faster but cost 7 KB more, and RAM is what keeps WiFi stable here.
AsyncWsLink ws("/ws", 1024, 4 * 1028);

// ── board: /matrix/<board>/{gif,png,lua} ─────────────────────────────────────

char board[33] = BOARD_NAME;
const char* const BOARD_SUBDIRS[] = {"gif", "png", "lua", "playlist"};

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
	storage->mkdir("/matrix");
	storage->mkdir(board_dir());
	for (const char* sub : BOARD_SUBDIRS)
		storage->mkdir(board_dir(sub));
}

// Reads a str argument as an absolute path that stays inside the storage->
bool read_path(Request& req, char* path) {
	req.args.str(path, FileModule::PATH_MAX_LEN);
	return req.args.ok() && FileModule::validPath(path);
}

bool has_ext(const char* path, const char* ext) {
	size_t n = strlen(path), e = strlen(ext);
	return n >= e && strcasecmp(path + n - e, ext) == 0;
}

// What plays (matrix.playing), "" when nothing; every change is pushed to the
// clients (matrix.playing.changed): an app opened later still knows it.
// What played, kept across a crash (RTC memory survives a panic or a
// watchdog reset, not a power cycle): at boot, a file that crashed the board
// is not started again (main.cpp).
struct CrashMemo {
	uint32_t magic;
	char path[96];
};
RTC_NOINIT_ATTR CrashMemo crash_memo;
constexpr uint32_t CRASH_MEMO_MAGIC = 0x504C4159;  // "PLAY"

// A playlist: now_playing is its path, now_item the file it shows.
char now_playing[FileModule::PATH_MAX_LEN] = "";
char now_item[FileModule::PATH_MAX_LEN] = "";

void set_playing(const char* path, const char* item = "") {
	if (strcmp(path, now_playing) == 0 && strcmp(item, now_item) == 0)
		return;
	strlcpy(now_playing, path, sizeof(now_playing));
	strlcpy(now_item, item, sizeof(now_item));
	crash_memo.magic = CRASH_MEMO_MAGIC;
	strlcpy(crash_memo.path, path, sizeof(crash_memo.path));
	uint8_t buf[4 + sizeof(now_playing) + sizeof(now_item)];
	Writer w(buf, sizeof(buf));
	w.str(now_playing).str(now_item);
	node.emit(method::MatrixPlayingChanged, w);
}

// Stop GIF and Lua and wait until they have released the shared arena.
bool stop_media() {
	bool lua = Lua::stop();
	bool gif = SpectreGif::stop();
	return lua && gif;
}

// stop_media() and the playlist.
// forget: nothing plays afterwards (false: something else is about to).
bool stop_all(bool forget = true) {
	Playlist::stop();
	bool ok = stop_media();
	if (forget)
		set_playing("");
	return ok;
}

// ── sys ──────────────────────────────────────────────────────────────────────

void sys_info(Request& req) {
	req.result.str(FIRMWARE_VERSION).str(BUILD_GIT_COMMIT_HASH).u32(millis()).u32(esp_get_free_heap_size())
	    .u32(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

void sys_time_set(Request& req) {
	uint32_t unix = req.args.u32();
	int16_t offset = req.args.i16();
	if (!req.args.ok())
		return req.fail(error::BadArgs);
	clock_set(unix, offset);
	if (preferences.getShort("utc_off", INT16_MIN) != offset)
		preferences.putShort("utc_off", offset);  // for NTP after a reboot
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
	req.result.u16(layout->panel_w * layout->chain).u16(layout->panel_h).u16(matrix_w).u16(matrix_h).str(board)
	    .u8(color_depth).u8(color_depth_max).str(layout->id);
}

void matrix_layouts(Request& req) {
	req.result.u8(LAYOUT_COUNT);
	for (uint8_t i = 0; i < LAYOUT_COUNT; i++) {
		const Layout& l = LAYOUTS[i];
		req.result.str(l.id).str(l.name).u16(l.cols * l.tile_w).u16(l.rows * l.tile_h)
		    .u8(layout_depth_max(l, false)).u8(layout_depth_max(l, true) != 0);
	}
}

// saved, then a reboot: the display's buffers are sized at boot
void matrix_layout_set(Request& req) {
	char id[33];
	req.args.str(id, sizeof(id));
	if (!req.args.ok() || !layout_save(id))
		return req.fail(error::BadArgs, "layout: an id of matrix.layouts");
	if (strcmp(id, layout->id) != 0) {
		Log.printf("layout: %s, rebooting\n", id);
		reboot_at = millis() + 300;
	}
}

// saved, then applied after the reply (protocol_loop): the display restarts
uint8_t pending_depth = 0;

void matrix_depth_set(Request& req) {
	uint8_t bits = req.args.u8();
	if (!req.args.ok() || bits < 2 || bits > color_depth_max)
		return req.fail(error::BadArgs, "depth: 2 to depth_max bits");
	pending_depth = bits;  // saved once it proves to fit (apply_depth)
}

// the display rebuilt with a new depth, what played plays again. Its buffers
// don't fit in the RAM left in pieces: saved, tried at the next boot (the heap
// is whole then; still too much: boots one bit less). Too little RAM left
// with them: back to the previous depth, not saved.
void apply_depth() {
	uint8_t bits = pending_depth;
	pending_depth = 0;
	if (bits == color_depth)
		return;
	String was = now_playing;
	bool playlist = Playlist::active();  // goes on with the item it was on
	if (!(playlist ? stop_media() : stop_all(false))) {
		reboot_at = millis() + 300;
		return;
	}
	uint8_t previous = color_depth;
	uint32_t t0 = millis();
	if (!set_color_depth(bits)) {
		Log.line(LogLevel::Error, "color depth: RAM in pieces, restarting to try at boot");
		bool back = set_color_depth(previous);  // something on screen meanwhile
		preferences.putUChar("depth", bits);
		reboot_at = millis() + (back ? 300 : 0);
		return;
	}
	if (esp_get_free_heap_size() < MIN_FREE_HEAP) {
		char msg[96];
		snprintf(msg, sizeof(msg), "color depth: not enough RAM for %u bits, back to %u", bits, previous);
		Log.line(LogLevel::Error, msg);
		if (!set_color_depth(previous)) {
			reboot_at = millis() + 300;
			return;
		}
	} else {
		preferences.putUChar("depth", bits);
		Log.printf("color depth: %u bits (%u ms), free heap %u\n", bits, millis() - t0, esp_get_free_heap_size());
	}
	if (playlist)
		Playlist::replay();
	else if (was.length())
		play_file(was.c_str());
	else
		set_playing("");
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

void matrix_playing(Request& req) {
	req.result.str(now_playing).str(now_item);
}

void matrix_skip(Request& req) {
	int8_t delta = req.args.i8();
	if (!req.args.ok() || delta == 0)
		return req.fail(error::BadArgs);
	if (!Playlist::skip(delta))
		req.fail(error::NotFound, "no playlist plays");
}

}  // namespace

String playing_before_crash() {
	esp_reset_reason_t why = esp_reset_reason();
	bool crashed = why == ESP_RST_PANIC || why == ESP_RST_INT_WDT || why == ESP_RST_TASK_WDT || why == ESP_RST_WDT;
	String path = crashed && crash_memo.magic == CRASH_MEMO_MAGIC ? String(crash_memo.path) : String();
	crash_memo.magic = 0;
	return path;
}

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

uint16_t play_media(const char* path) {
	bool gif = has_ext(path, ".gif"), png = has_ext(path, ".png"), lua = has_ext(path, ".lua");
	if (!gif && !png && !lua)
		return error::BadArgs;
	if (!storage->exists(path))
		return error::NotFound;
	// GIF, PNG and Lua share one RAM arena: the current one must be done first
	if (!stop_media())
		return error::Busy;
	if (gif) {
		SpectreGif::play(path);
	} else if (png) {
		if (uint16_t err = SpectrePng::show(path))
			return err;
	} else {
		File f = storage->open(path);
		String script = f.readString();
		f.close();
		const char* slash = strrchr(path, '/');
		if (!Lua::run_script(script, slash ? slash + 1 : path))
			return error::NoMemory;
	}
	return 0;
}

void playing_changed(const char* path, const char* item) {
	set_playing(path, item);
}

uint16_t play_file(const char* path) {
	if (!stop_all(false))
		return error::Busy;
	if (has_ext(path, ".txt")) {
		if (uint16_t err = Playlist::start(path)) {  // it reports each item itself
			set_playing("");
			return err;
		}
		Log.printf("play playlist %s\n", path);
		return 0;
	}
	if (uint16_t err = play_media(path)) {
		set_playing("");  // the previous file was stopped
		return err;
	}
	set_playing(path);
	Log.printf("play %s\n", path);
	return 0;
}

void protocol_begin(NimBLEServer* server) {
	node.streamWindow = 4;
	node.addModule("light");
	node.addModule("matrix");

	node.on(method::SysInfo, sys_info);
	node.on(method::SysReboot, sys_reboot);
	node.on(method::SysTimeSet, sys_time_set);
	clock_set_offset(preferences.getShort("utc_off", 0));
	node.on(method::LightBrightnessSet, brightness_set);
	node.on(method::LightBrightnessStep, brightness_step);
	node.on(method::LightFill, fill);
	node.on(method::LightClear, clear);
	// file.* comes from spectre::FileModule; the matrix only reacts to it
	files.onModify = [](const char* path) {
		if (SpectreGif::isPlaying(path))  // its file is about to change
			SpectreGif::stop();
		if (strcmp(path, now_playing) == 0) {  // the file, or the playlist itself
			Playlist::stop();
			set_playing("");
		}
	};
	static uint32_t upload_start = 0, progress_shown = 0, upload_size = 0;
	files.onUploadStart = [](const char*, uint32_t size) {
		stop_all();  // the display shows the upload progress
		upload_size = size;
		upload_start = millis();
		progress_shown = 0;
	};
	files.onUploadError = [](const char* path, uint32_t offset) {
		Log.printf("upload: SD card write failed in %s at byte %u (bad card / filesystem?)\n", path, offset);
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
	node.on(method::MatrixPlaying, matrix_playing);
	node.on(method::MatrixDepthSet, matrix_depth_set);
	node.on(method::MatrixLayouts, matrix_layouts);
	node.on(method::MatrixLayoutSet, matrix_layout_set);
	node.on(method::MatrixSkip, matrix_skip);

	log_queue = xQueueCreate(8, sizeof(LogEntry));
	if (server) {
		ble.begin(server);
		node.attach(ble);
		ble_on = true;
	}
	node.attach(serial);
	String host = board;  // banana → banana.local
	host.replace('_', '-');
	wifi.setHostname(host);
	node.attach(ws);
	wifi.onState = [](WifiModule::State s) {
		const char* names[] = {"off", "connecting", "connected", "failed"};
		if (s == WifiModule::Connected)
			Log.printf("wifi: connected to %s, http://%s.local (%s)\n", wifi.ssid().c_str(), wifi.hostname().c_str(),
			           wifi.ip().c_str());
		else
			Log.printf("wifi: %s\n", names[s]);
	};
	wifi.onConnected = [] {  // servers and their buffers only once WiFi is up
		clock_ntp();
		ws.begin(http);
		// the web app, copied to /www on the SD card: one gzipped index.html
		// (spectre-bt-app scripts/push-to-matrix.sh). A request needs a few KB:
		// refused (503) rather than started when RAM is short.
		http.serveStatic("/", *storage, "/www/").setDefaultFile("index.html").setFilter([](AsyncWebServerRequest*) {
			return heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) > 10 * 1024;
		});
		http.onNotFound([](AsyncWebServerRequest* r) {
			if (heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) <= 10 * 1024)
				r->send(503, "text/plain", "busy, try again");
			else
				r->send(404, "text/plain", "not found");
		});
		http.begin();
	};
	wifi.onSaved = [] {  // the other mode, or the new network: from a clean boot
		Log.printf("wifi: %s, rebooting\n", wifi.ssid().length() ? "network saved" : "off");
		reboot_at = millis() + 500;
	};
	wifi.radio = protocol_wifi_mode();
	wifi.powerSave = false;  // WiFi mode = BLE off: full speed
	wifi.begin();
}

bool protocol_wifi_mode() {
	static int mode = -1;
	if (mode < 0) {
		bool skip = skip_wifi == SKIP_WIFI;
		skip_wifi = 0;  // the next reboot tries WiFi again
		bool fits = layout_depth_max(*layout, true) != 0;
		mode = WifiModule::saved() && !skip && fits;
		if (skip)
			Log.println("wifi: could not join the network, BLE mode for this boot");
		else if (WifiModule::saved() && !fits)
			Log.printf("wifi: no RAM for it next to the %s layout, BLE mode\n", layout->id);
	}
	return mode == 1;
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
	if (ble_on)
		ble.poll();
	serial.poll();
	ws.poll();
	wifi.loop();
	static bool joined = false;
	joined |= wifi.state() == WifiModule::Connected;
	// Starved of RAM for long (heavy traffic gone wrong), WiFi can get stuck
	// for good: reboot (back on WiFi) rather than stay unreachable.
	static uint32_t starved_since = 0;
	if (wifi.radio && heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < 6 * 1024) {
		if (!starved_since)
			starved_since = millis() | 1;
		else if (millis() - starved_since > 20000 && !reboot_at) {
			Log.println("wifi: out of memory for 20 s, rebooting");
			reboot_at = millis() + 200;
		}
	} else {
		starved_since = 0;
	}
	if (wifi.radio && !joined && millis() > WIFI_JOIN_MS && !reboot_at) {
		Log.println("wifi: not connected after 20 s, rebooting into BLE mode");
		skip_wifi = SKIP_WIFI;
		reboot_at = millis() + 200;
	}
	node.tick(millis());
	ota.loop();
	LogEntry e;
	while (xQueueReceive(log_queue, &e, 0) == pdTRUE) {
		uint8_t buf[2 + sizeof(e.text)];
		Writer w(buf, sizeof(buf));
		w.u8(e.level).str(e.text);
		node.emit(method::SysLog, w);
	}
	Playlist::loop();
	if (pending_depth)
		apply_depth();
	if (reboot_at && millis() > reboot_at)
		ESP.restart();
}

void protocol_disconnected() {
	ble.disconnected();
}
