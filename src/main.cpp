#include <Mapping.h>
#include <sys/stat.h>

#include <Preferences.h>
Preferences preferences;

#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <NimBLEDevice.h>
#include <esp_bt.h>
#include "Arena.hpp"
#include "Gif.hpp"
#include "Lua.hpp"
#include "Layout.hpp"
#include "Protocol.hpp"
#include "Storage.hpp"
#include "Log.hpp"

// Advertised so clients can spot Spectre devices (spectre_protocol PROTOCOL.md §2.1)
#define ADV_UUID_SPECTRE "4242"

MatrixPanel_I2S_DMA *display = nullptr;

char	hostname[50];  // the BLE name: "<board name> <end of the MAC>"

uint8_t brightness = 50;
// Color depth (bits per color): saved ("depth", set with matrix.depth.set),
// the layout's default otherwise; capped to what RAM allows (Layout.cpp).
uint8_t color_depth = 5;
uint8_t color_depth_max = 7;
// Below this much free heap, BLE, uploads and updates start failing (seen at
// 8.5 KB): too many bits for the display, the depth goes back down.
extern const uint32_t MIN_FREE_HEAP = 20 * 1024;  // extern: Protocol.cpp reads it
File root;

VirtualMatrixPanel  *virtualDisp = nullptr;
uint8_t is_fs_mnt = false;

NimBLEServer* pServer;

void flip_matrix() {
	display->flipDMABuffer();
}

void set_brightness(int b) {
	brightness = constrain(b, 0, 250);
	Log.printf("Brightness set to %d\n", brightness);
	display->setBrightness8(brightness); //0-255
}

void set_all_pixel(uint8_t r, uint8_t g, uint8_t b, uint8_t w) {
	delay(100);
	display->fillScreenRGB888(r,g,b);
	flip_matrix();
}


uint16_t hue = 0;

extern void hsv2rgb(uint16_t h, uint8_t s, uint8_t v, uint8_t *r, uint8_t *g, uint8_t *b);  // Gif.cpp, h 0-359

void print_progress(const char *str, uint32_t offset, uint32_t total_size) {
	virtualDisp->clearScreen();
	virtualDisp->setCursor(4, matrix_h / 2 - 14);
	virtualDisp->setTextSize(1);
	virtualDisp->setTextColor(display->color565(255,255,255));
	virtualDisp->printf(str);
	virtualDisp->fillRect(4, matrix_h/2, matrix_w - 4 * 2, 8, 255, 255, 255);
	uint8_t r, g, b;
	hsv2rgb(hue += 14, 255, 255, &r, &g, &b);  // a rainbow, one step per update
	virtualDisp->fillRect(
		4+1,
		(matrix_h/2)+1,
		map(offset, 0, total_size, 0, (matrix_w - 4 * 2 - 2)),
		8 - 2,
		r,
		g,
		b
	);
	flip_matrix();
}

void print_message(const char *str) {
	Log.printf("print_message: %s", str);
	virtualDisp->clearScreen();
	virtualDisp->setCursor(0, matrix_h/2-16);
	virtualDisp->setTextSize(1);
	virtualDisp->setTextColor(virtualDisp->color565(255,255,255));
	virtualDisp->setTextWrap(true);
	virtualDisp->print(str);
	flip_matrix();
}
class MyServerCallbacks : public NimBLEServerCallbacks {
	void onConnect(NimBLEServer* pServer, ble_gap_conn_desc *desc) {
		// 7.5 ms interval + 251-byte packets: fastest BLE uploads (spectre_protocol bench)
		pServer->updateConnParams(desc->conn_handle, 0x6, 0x6, 0, 100);
		pServer->setDataLen(desc->conn_handle, 251);
		Log.printf("BLE connected\n");
	};

	void onDisconnect(NimBLEServer* pServer) {
		protocol_disconnected();
	}

	void onMTUChange (uint16_t mtu, ble_gap_conn_desc *desc) {
		Log.printf("MTU change: %d\n", mtu);
	}
};

#define MIN(a,b) (((a)<(b))?(a):(b))
#define BUF_SIZE (256*1)

void playAnimeTask(void* parameter) {


	for (;;) {
		
	}

	// Log.println("Ending task playAnimeTask");
	// vTaskDelete(NULL);
}

// Build with -DHEAP_TRACE to log the heap after each init step of setup().
#ifdef HEAP_TRACE
	#define HEAP_MARK(step)                                                                     \
		Log.printf("[heap] %-28s free %6u  largest %6u\n", step, esp_get_free_heap_size(), \
		           heap_caps_get_largest_free_block(MALLOC_CAP_8BIT))
#else
	#define HEAP_MARK(step)
#endif

// The library doesn't survive a failed DMA allocation (it carries on with a
// null descriptor list and crashes): first, the same allocations for real, in
// its order (each row's data for both buffers, then the two descriptor lists,
// one block each), with MIN_FREE_HEAP left for the rest. Free heap in pieces
// won't do: the rows split the big blocks the descriptors need.
static bool display_fits(uint8_t bits) {
	const Layout& l = *layout;
	const int rows = l.panel_h;  // row pairs × 2 buffers
	size_t row = size_t(l.panel_w) * l.chain * bits * 2;
	size_t desc = (size_t(1) << (bits - 1)) * (l.panel_h / 2) * 12;  // lldesc_t per pass per row (most a depth needs)
	void* block[64 + 2] = {};
	int n = 0;
	bool ok = rows <= 64;
	for (int i = 0; ok && i < rows; i++)
		ok = (block[n++] = heap_caps_malloc(row, MALLOC_CAP_DMA)) != nullptr;
	for (int i = 0; ok && i < 2; i++)
		ok = (block[n++] = heap_caps_malloc(desc + 2048, MALLOC_CAP_DMA)) != nullptr;  // + its small allocations in between
	ok = ok && esp_get_free_heap_size() >= MIN_FREE_HEAP;
	while (n)
		free(block[--n]);
	if (!ok)
		Log.printf("display: not enough RAM for %u bits\n", bits);
	return ok;
}

// The display (DMA buffers sized for `bits` per color) and its virtual panel,
// as the layout says. At boot, and again when the color depth changes
// (matrix.depth.set).
bool create_display(uint8_t bits) {
	const Layout& l = *layout;
	// G and B lines: two wirings in use (Layout::gb_swapped)
	int8_t g1 = 25, b1 = 32, g2 = 23, b2 = 26;
	if (l.gb_swapped) {
		std::swap(g1, b1);
		std::swap(g2, b2);
	}
	HUB75_I2S_CFG::i2s_pins _pins = {33, g1, b1, 27, g2, b2, 22, 21, 19, 18, l.e_pin, 17, 16, 5};
	//                               R1          R2          A   B   C   D   E        LAT OE CLK

	HUB75_I2S_CFG mxconfig(l.panel_w, l.panel_h, l.chain, _pins);

	mxconfig.setPixelColorDepthBits(bits);
	mxconfig.double_buff     = true;                    // use DMA double buffer (twice as much RAM required)
	mxconfig.driver          = l.fm6124 ? HUB75_I2S_CFG::FM6124 : HUB75_I2S_CFG::SHIFTREG;
	mxconfig.i2sspeed        = HUB75_I2S_CFG::HZ_20M;   // I2S clock speed
	mxconfig.clkphase        = false;                   // I2S clock phase
	mxconfig.latch_blanking  = l.fm6124 ? 5 : 1;        // clock cycles OE stays off around LAT

	if (!display_fits(bits))
		return false;
	display = new MatrixPanel_I2S_DMA(mxconfig);

	bool ok = display->begin();

	int16_t map[9];
	for (int i = 0; i < 9; i++)
		map[i] = l.map[i];
	virtualDisp = new VirtualMatrixPanel((*display), l.rows, l.cols, l.tile_w, l.tile_h, map);

	return ok;
}

// A new color depth, live: the caller stopped GIF / Lua (nothing draws). The
// old display's DMA buffers are freed, new ones allocated. False: no RAM for
// them (the caller reboots: the saved depth applies then).
bool set_color_depth(uint8_t bits) {
	delete virtualDisp;
	virtualDisp = nullptr;
	delete display;
	display = nullptr;
	if (!create_display(bits))
		return false;
	color_depth = bits;
	set_brightness(brightness);
	return true;
}

void setup() {
	Serial.setRxBufferSize(5120);  // Spectre Protocol over serial: holds an upload window
	Serial.begin(115200);
	HEAP_MARK("start (serial ready)");

	Log.println("\n------------------------------");
	Log.printf("  Hub75 LEDs driver\n");
	int core = xPortGetCoreID();
	Log.print("  Main code running on core ");
	Log.println(core);
	Log.println("------------------------------");

	{
		layout_begin();
		color_depth_max = layout_depth_max(*layout, protocol_wifi_mode());
		Preferences p;
		p.begin("matrix", true);
		color_depth = p.getUChar("depth", layout->depth);
		p.end();
		color_depth = constrain(color_depth, 2, color_depth_max);
		brightness = layout->brightness;
		Log.printf("  Layout: %s, %ux%u\n", layout->name, matrix_w, matrix_h);
		Log.printf("  Color depth: %u bits (max %u)\n", color_depth, color_depth_max);
	}

	// the model in Layout.cpp is an estimate: fewer bits if RAM says no
	while (!create_display(color_depth) && color_depth > 2) {
		Log.printf("  no RAM for the display at %u bits\n", color_depth);
		delete virtualDisp;
		virtualDisp = nullptr;
		delete display;
		display = nullptr;
		color_depth_max = --color_depth;
	}
	HEAP_MARK("display DMA buffers");

	// GIF / PNG / Lua memory, while the heap is still in one piece
	bool arena_ok = Arena::init();
	HEAP_MARK("player arena");

	set_brightness(brightness);
	if (!arena_ok) {
		print_message("No RAM\nfor player");
		delay(2000);
	}

	if (storage_begin())
		is_fs_mnt = 1;
	else
		print_message("Can't mnt\nSD / SPIFFS");
	{  // a board that had a card but can't read it now: say so (its files
	   // are on the card, the flash holds none of them)
		Preferences p;
		p.begin("matrix", false);
		if (storage_is_sd && !p.getBool("had_sd", false))
			p.putBool("had_sd", true);
		bool sd_lost = !storage_is_sd && p.getBool("had_sd", false);
		p.end();
		if (sd_lost) {
			Log.line(LogLevel::Error, "SD card not readable: using the flash (none of the card's files)");
			print_message("SD CARD\nERROR");
			delay(2500);
		}
	}

	HEAP_MARK("SD card / SPIFFS mounted");
	String boot_anim;  // played once GIF / Lua tasks exist
	preferences.begin("matrix", false);
	if (is_fs_mnt) {
		board_begin();  // /matrix/<board>/{gif,png,lua}
		root = storage->open(board_dir("gif"));
		String saved = preferences.getString("anim", "");
		String crashed = playing_before_crash();
		if (crashed.length()) {  // that file crashed the board: not again
			Log.printf("restarted after a crash while playing %s: not playing it again\n", crashed.c_str());
			const char* name = strrchr(crashed.c_str(), '/');
			print_message((String("CRASHED:\n") + (name ? name + 1 : crashed.c_str())).c_str());
			delay(2500);
			if (saved == crashed || crashed.endsWith(".txt"))
				saved = "";
		}
		if (saved.length() && storage->exists(saved)) {
			Log.printf("Start previous anim %s\n", saved.c_str());
			boot_anim = saved;
		} else {  // nothing saved, or deleted since: the board's first GIF
			File file = root ? root.openNextFile() : File();
			while (file && file.isDirectory())
				file = root.openNextFile();
			if (file)
				boot_anim = file.path();
			else
				print_message("No gif\nfound");
		}
	}

	HEAP_MARK("board, preferences");
	{  // BLE name: the board name ("default": the app's name), then the MAC's end
		String name = preferences.getString("board", "");
		if (name.length() == 0 || name == "default")
			name = "Spectre Matrix";
		name.setCharAt(0, toupper(name[0]));  // "banana" → "Banana 1A2B"
		strlcpy(hostname, name.c_str(), sizeof(hostname) - 6);
		strcat(hostname, " ");
	}
	BLEAdvertising *pAdvertising = nullptr;
	if (protocol_wifi_mode()) {  // no RAM for both radios
		Log.println("WiFi mode: BLE off");
		// the Bluetooth controller's memory, reserved at boot: to the heap
		// (until the next reboot)
		esp_bt_controller_mem_release(ESP_BT_MODE_BTDM);
		HEAP_MARK("Bluetooth memory released");
		protocol_begin(nullptr);
		HEAP_MARK("protocol (serial + WiFi links)");
	} else {
	Log.println("Start BLE");
	// Create the BLE Device
	NimBLEDevice::init(hostname);
	NimBLEDevice::setPower(ESP_PWR_LVL_P9);
	NimBLEDevice::setMTU(BLE_ATT_MTU_MAX);

	// NimBLEDevice::setSecurityAuth(/*BLE_SM_PAIR_AUTHREQ_BOND | BLE_SM_PAIR_AUTHREQ_MITM |*/ BLE_SM_PAIR_AUTHREQ_SC);

	// Create the BLE Server
	HEAP_MARK("NimBLE stack");
	pServer = NimBLEDevice::createServer();
	pServer->setCallbacks(new MyServerCallbacks());


	HEAP_MARK("BLE server");
	protocol_begin(pServer);
	HEAP_MARK("protocol (BLE+serial links)");

	pAdvertising = BLEDevice::getAdvertising();
	// pAdvertising->setAppearance(0x7<<6); // glasses
	pAdvertising->setAppearance(0x01F << 6 | 0x06); // LEDs 

	uint8_t* mac = (uint8_t*)NimBLEDevice::getAddress().getNative();
	char macStr[5];
	snprintf(macStr, sizeof(macStr), "%02X%02X", mac[4], mac[5]);
	Log.printf("BLE MAC Address: %s\n", macStr);
	strcat(hostname, macStr);
	pAdvertising->setName(hostname);
	NimBLEDevice::setDeviceName(hostname);
	}
	
	HEAP_MARK("advertising config");
	SpectreGif::init();
	HEAP_MARK("GIF task (6 KB stack)");
	Lua::init();
	HEAP_MARK("Lua (task created on demand)");
	Log.println("::init() OK");
	if (boot_anim.length())
		play_file(boot_anim.c_str());

	// Start advertising
	if (pAdvertising) {
		pAdvertising->addServiceUUID(ADV_UUID_SPECTRE);
		pAdvertising->start();
	}
	HEAP_MARK("advertising: end of setup");
	// the RAM model (Layout.cpp) is an estimate: a depth that leaves too little
	// for the rest would make the board unreachable, one bit less next boot
	if (esp_get_free_heap_size() < MIN_FREE_HEAP && color_depth > 2) {
		Log.printf("only %u B free at %u bits: %u bits from now on, rebooting\n", esp_get_free_heap_size(), color_depth,
		           color_depth - 1);
		preferences.putUChar("depth", color_depth - 1);
		delay(100);
		ESP.restart();
	}
	Log.println("Waiting a client connection to notify...");
}

void loop(void) {
	//Log.printf("loop %s \n", root.path());
	protocol_loop();
#ifdef HEAP_TRACE
	static uint32_t last_trace = 0;
	if (millis() - last_trace > 5000) {  // stack still free in each task (high-water mark)
		last_trace = millis();
		Log.printf("[stack] free: loop %u, GifTask %u, LuaTask %u; heap free %u (lowest %u)\n",
		           uxTaskGetStackHighWaterMark(nullptr), uxTaskGetStackHighWaterMark(xTaskGetHandle("GifTask")),
		           xTaskGetHandle("LuaTask") ? uxTaskGetStackHighWaterMark(xTaskGetHandle("LuaTask")) : 0,
		           esp_get_free_heap_size(),
		           esp_get_minimum_free_heap_size());
	}
#endif
	if (is_fs_mnt && !root) {
		print_message("Can't find\nGif folder!\n");
		vTaskDelay(1000 / portTICK_PERIOD_MS);
	}

	vTaskDelay(1 / portTICK_PERIOD_MS);
}

