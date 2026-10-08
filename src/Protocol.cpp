#include <Arduino.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <Mapping.h>
#include <Preferences.h>
#include <SpectreProtocol.h>
#include <spectre/ble_nimble.h>

#include "Gif.hpp"
#include "Lua.hpp"
#include "Png.hpp"
#include "Protocol.hpp"

#ifdef USE_SD
	#include "SD.h"
	#define filesystem SD
#endif
#ifdef USE_SPIFFS
	#include "SPIFFS.h"
	#define filesystem SPIFFS
#endif

#define FIRMWARE_VERSION "1.0.0"

using namespace spectre;

extern Preferences preferences;
extern uint8_t brightness;
extern void set_brightness(int b);
extern void set_all_pixel(uint8_t r, uint8_t g, uint8_t b, uint8_t w);
extern void print_progress(const char *str, uint32_t offset, uint32_t total_size);

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
uint32_t reboot_at = 0;

constexpr size_t PATH_MAX_LEN = 128;

// Reads a str argument as an absolute path that stays inside the filesystem.
bool read_path(Request& req, char* path) {
	req.args.str(path, PATH_MAX_LEN);
	return req.args.ok() && path[0] == '/' && !strstr(path, "..");
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

// Upload target: written to <path>.part, renamed over <path> when complete.
class FileSink : public StreamSink {
public:
	FileSink(const char* path, uint32_t size) : size_(size), start_(millis()) {
		snprintf(path_, sizeof(path_), "%s", path);
		snprintf(part_, sizeof(part_), "%s.part", path);
		file_ = filesystem.open(part_, FILE_WRITE, true);  // creates missing dirs
	}

	bool ok() { return (bool)file_; }

	bool write(const uint8_t* data, size_t len) override {
		if (file_.write(data, len) != len)
			return false;
		written_ += len;
		uint32_t now = millis();
		if (now - shown_ >= 250 || written_ == size_) {  // progress bar on the matrix
			shown_ = now;
			print_progress("upload", written_, size_);
		}
		return true;
	}

	uint16_t close(bool complete) override {
		file_.close();
		uint32_t ms = millis() - start_;
		NimBLEConnInfo c = ble.connInfo();
		Serial.printf("upload %s: %u/%u B in %u ms (%u B/s), conn itvl %.2f ms, mtu %u, rx dropped %u B, frame errors %u\n",
		              complete ? "ok" : "FAILED", written_, size_, ms, ms ? written_ * 1000 / ms : 0,
		              c.getConnInterval() * 1.25f, c.getMTU(), ble.dropped(), ble.errors());
		if (!complete) {
			filesystem.remove(part_);
			return 0;
		}
		if (filesystem.exists(path_)) {
			if (SpectreGif::isPlaying(path_))
				SpectreGif::stop();
			filesystem.remove(path_);
		}
		return filesystem.rename(part_, path_) ? 0 : error::Internal;
	}

private:
	char path_[PATH_MAX_LEN];
	char part_[PATH_MAX_LEN + 5];
	File file_;
	uint32_t size_;
	uint32_t start_;
	uint32_t written_ = 0;
	uint32_t shown_ = 0;
};

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

// ── file ─────────────────────────────────────────────────────────────────────

void file_list(Request& req) {
	char dir[PATH_MAX_LEN];
	if (!read_path(req, dir))
		return req.fail(error::BadArgs);
	uint16_t offset = req.args.u16();
	if (!req.args.ok())
		return req.fail(error::BadArgs);

	File root = filesystem.open(dir);
	if (!root || !root.isDirectory())
		return req.fail(error::NotFound);

	// total, count, then as many entries from `offset` as fit
	uint8_t page_buf[512];
	Writer page(page_buf, sizeof(page_buf));
	uint16_t total = 0;
	uint8_t count = 0;
	bool full = false;
	size_t room = req.result.remaining() - 3;  // minus total u16 + count u8
	for (File f = root.openNextFile(); f; f = root.openNextFile()) {
		if (f.isDirectory())
			continue;
		if (total++ < offset || full)
			continue;
		const char* name = f.name();
		const char* slash = strrchr(name, '/');  // some cores return the full path
		if (slash)
			name = slash + 1;
		size_t entry = 1 + strlen(name) + 4;
		if (page.size() + entry > room || count == 255) {
			full = true;
			continue;
		}
		page.str(name).u32(f.size());
		count++;
	}
	req.result.u16(total).u8(count).raw(page.data(), page.size());
}

void file_delete(Request& req) {
	char path[PATH_MAX_LEN];
	if (!read_path(req, path))
		return req.fail(error::BadArgs);
	if (!filesystem.exists(path))
		return req.fail(error::NotFound);
	if (SpectreGif::isPlaying(path))
		SpectreGif::stop();
	if (!filesystem.remove(path))
		req.fail(error::Internal);
}

void file_write(Request& req) {
	char path[PATH_MAX_LEN];
	if (!read_path(req, path))
		return req.fail(error::BadArgs);
	uint32_t size = req.args.u32();
	if (!req.args.ok())
		return req.fail(error::BadArgs);
	stop_all();  // the display shows the upload progress
	std::unique_ptr<FileSink> sink(new FileSink(path, size));
	if (!sink->ok())
		return req.fail(error::Internal, "cannot create file");
	req.acceptStream(std::move(sink), size);
}

// ── matrix ───────────────────────────────────────────────────────────────────

void matrix_info(Request& req) {
	req.result.u16(MATRIX_WIDTH).u16(MATRIX_HEIGHT).u16(V_MATRIX_WIDTH).u16(V_MATRIX_HEIGHT);
}

void matrix_play(Request& req) {
	char path[PATH_MAX_LEN];
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
		Lua::run_script(script);
	}
	Serial.printf("play %s\n", path);
	return 0;
}

void protocol_begin(NimBLEServer* server) {
	node.streamWindow = 4;
	node.addModule("file");
	node.addModule("light");
	node.addModule("matrix");

	node.on(method::SysInfo, sys_info);
	node.on(method::SysReboot, sys_reboot);
	node.on(method::LightBrightnessSet, brightness_set);
	node.on(method::LightBrightnessStep, brightness_step);
	node.on(method::LightFill, fill);
	node.on(method::LightClear, clear);
	node.on(method::FileList, file_list);
	node.on(method::FileDelete, file_delete);
	node.on(method::FileWrite, file_write);
	node.on(method::MatrixInfo, matrix_info);
	node.on(method::MatrixPlay, matrix_play);
	node.on(method::MatrixStop, matrix_stop);

	log_queue = xQueueCreate(8, sizeof(LogEntry));
	ble.begin(server);
	node.attach(ble);
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
	node.tick(millis());
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
