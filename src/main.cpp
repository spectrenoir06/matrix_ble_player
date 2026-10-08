#include <Mapping.h>
#include <sys/stat.h>

#include <Preferences.h>
Preferences preferences;

#ifdef USE_SD
	#include "FS.h"
	#include "SD.h"
	#include "SPI.h"
	#define filesystem SD
#endif
#ifdef USE_SPIFFS
	#include "SPIFFS.h"
	#define filesystem SPIFFS
#endif
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <NimBLEDevice.h>
#include "Arena.hpp"
#include "Gif.hpp"
#include "Lua.hpp"
#include "Protocol.hpp"
#include "Log.hpp"

// Advertised so clients can spot Spectre devices (spectre_protocol PROTOCOL.md §2.1)
#define ADV_UUID_SPECTRE "4242"

#define DEFAULT_HOSTNAME	HOSTNAME
#define AP_SSID				HOSTNAME

#ifdef USE_SD
	const int SD_CS   = SD_CS_PIN;
	const int SD_SCK  = SD_SCK_PIN;
	const int SD_MOSI = SD_MOSI_PIN;
	const int SD_MISO = SD_MISO_PIN;
#endif

MatrixPanel_I2S_DMA *display = nullptr;

char	hostname[50] = DEFAULT_HOSTNAME;

uint8_t brightness = BRIGHTNESS;
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

void print_progress(const char *str, uint32_t offset, uint32_t total_size) {
	virtualDisp->clearScreen();
	virtualDisp->setCursor(4, V_MATRIX_HEIGHT / 2 - 14);
	virtualDisp->setTextSize(1);
	virtualDisp->setTextColor(display->color565(255,255,255));
	virtualDisp->printf(str);
	virtualDisp->fillRect(4, V_MATRIX_HEIGHT/2, V_MATRIX_WIDTH - 4 * 2, 8, 255, 255, 255);
	CRGB rgb;
	hsv2rgb_spectrum(CHSV(hue+=10, 255, 255), rgb);
	virtualDisp->fillRect(
		4+1,
		(V_MATRIX_HEIGHT/2)+1,
		map(offset, 0, total_size, 0, ((V_MATRIX_WIDTH) - 4 * 2 - 2)),
		8 - 2,
		rgb.r,
		rgb.g,
		rgb.b
	);
	flip_matrix();
}

void print_message(const char *str) {
	Log.printf("print_message: %s", str);
	virtualDisp->clearScreen();
	virtualDisp->setCursor(0, V_MATRIX_HEIGHT/2-16);
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

void setup() {
	Serial.setRxBufferSize(5120);  // Spectre Protocol over serial: holds an upload window
	Serial.begin(115200);
	HEAP_MARK("start (serial ready)");

	Log.println("\n------------------------------");
	Log.printf("  Hub75 LEDs driver\n");
	Log.printf("  Hostname: %s\n", hostname);
	int core = xPortGetCoreID();
	Log.print("  Main code running on core ");
	Log.println(core);
	Log.println("------------------------------");

	HUB75_I2S_CFG::i2s_pins _pins = {R1_PIN, G1_PIN, B1_PIN, R2_PIN, G2_PIN, B2_PIN, A_PIN, B_PIN, C_PIN, D_PIN, E_PIN, LAT_PIN, OE_PIN, CLK_PIN};
	
	HUB75_I2S_CFG mxconfig(
		MATRIX_WIDTH,     // Module width
		MATRIX_HEIGHT,    // Module height
		MATRIX_CHAIN,     // chain length
		_pins             // pin mapping
	);

	mxconfig.double_buff     = true;                    // use DMA double buffer (twice as much RAM required)
	mxconfig.driver          = HUB75_I2S_CFG::SHIFTREG; // Matrix driver chip type - default is a plain shift register
	mxconfig.i2sspeed        = HUB75_I2S_CFG::HZ_20M;   // I2S clock speed
	mxconfig.clkphase        = false;                   // I2S clock phase
	mxconfig.latch_blanking  = 1;                       // How many clock cycles to blank OE before/after LAT signal change, default is 1 clock

	#ifdef IS_RICARD
		mxconfig.driver          = HUB75_I2S_CFG::FM6124;
		mxconfig.latch_blanking  = 5;
	#endif

	display = new MatrixPanel_I2S_DMA(mxconfig);

	display->begin();  // setup display with pins as pre-defined in the library
	HEAP_MARK("display DMA buffers");

	// GIF / PNG / Lua memory, while the heap is still in one piece
	bool arena_ok = Arena::init();
	HEAP_MARK("player arena");

	#ifdef IS_CROSS
		int16_t map[3*3] = {
			-1, 4, -1,
			1,  2,  3,
			-1, 0, -1
		};
		virtualDisp = new VirtualMatrixPanel((*display), 3, 3, 32, 32, map);
	#elif IS_PRINTER
		int16_t map[1] = {0};
		virtualDisp = new VirtualMatrixPanel((*display), 1, 1, 128, 32, map);
	#elif IS_RICARD
			int16_t map[2*3] = {
			0, 1,
			2, 3,
			4, 5
		};
		// int16_t map[1] = {0};
		virtualDisp = new VirtualMatrixPanel((*display), 3, 2, 32, 32, map);
	#elif IS_64x64
		int16_t map[1] = {0};
		virtualDisp = new VirtualMatrixPanel((*display), 1, 1, 64, 64, map);
	#else
		int16_t map[1] = {0};
		virtualDisp = new VirtualMatrixPanel((*display), 1, 1, 64, 32, map);
	#endif

	set_brightness(BRIGHTNESS);
	if (!arena_ok) {
		print_message("No RAM\nfor player");
		delay(2000);
	}

	#ifdef USE_SD
		// Initialize SD card
		SPI.begin(SD_SCK, SD_MISO, SD_MOSI);
		// 20 MHz SPI (5x faster listings, uploads, GIF reads); the library's
		// default 4 MHz as a fallback for cards / wiring that can't do it
		for (int i=0; i<20; i++) {
			uint32_t freq = i < 3 ? 20000000 : 4000000;
			if (!filesystem.begin(SD_CS, SPI, freq)) {
				Log.println("Card Mount Failed");
				print_message("Can't mnt\nSD Card!\n");
				delay(10);
			} else {
				is_fs_mnt = 1;
				Log.printf("SD card mounted at %u MHz\n", freq / 1000000);
				break;
			}
		}
	#endif

	#ifdef USE_SPIFFS
		if (!filesystem.begin(true)) {
			Log.println("An Error has occurred while mounting SPIFFS");
			print_message("Can't mnt\nSPIFFS!");
			// ESP.restart();
		} else {
			Log.println("mounting SPIFFS OK");
			is_fs_mnt = 1;
		}
	#endif

	HEAP_MARK("SD card mounted");
	String boot_anim;  // played once GIF / Lua tasks exist
	preferences.begin("matrix", false);
	if (is_fs_mnt) {
		board_begin();  // /matrix/<board>/{gif,png,lua}
		root = filesystem.open(board_dir("gif"));
		String saved = preferences.getString("anim", "");
		if (saved.length() && filesystem.exists(saved)) {
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

	BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
	// pAdvertising->setAppearance(0x7<<6); // glasses
	pAdvertising->setAppearance(0x01F << 6 | 0x06); // LEDs 

	uint8_t* mac = (uint8_t*)NimBLEDevice::getAddress().getNative();
	char macStr[5];
	snprintf(macStr, sizeof(macStr), "%02X%02X", mac[4], mac[5]);
	Log.printf("BLE MAC Address: %s\n", macStr);
	strcat(hostname, macStr);
	pAdvertising->setName(hostname);
	NimBLEDevice::setDeviceName(hostname);
	
	HEAP_MARK("advertising config");
	SpectreGif::init();
	HEAP_MARK("GIF task (16 KB stack)");
	Lua::init();
	HEAP_MARK("Lua task (10 KB stack)");
	Log.println("::init() OK");
	if (boot_anim.length())
		play_file(boot_anim.c_str());

	// Start advertising
	pAdvertising->addServiceUUID(ADV_UUID_SPECTRE);

	pAdvertising->start();
	HEAP_MARK("advertising: end of setup");
	Log.println("Waiting a client connection to notify...");
}

void loop(void) {
	//Log.printf("loop %s \n", root.path());
	protocol_loop();
#ifdef HEAP_TRACE
	static uint32_t last_trace = 0;
	if (millis() - last_trace > 5000) {  // stack still free in each task (high-water mark)
		last_trace = millis();
		Log.printf("[stack] free: loop %u, GifTask %u, LuaTask %u; heap free %u\n",
		           uxTaskGetStackHighWaterMark(nullptr), uxTaskGetStackHighWaterMark(xTaskGetHandle("GifTask")),
		           uxTaskGetStackHighWaterMark(xTaskGetHandle("LuaTask")), esp_get_free_heap_size());
	}
#endif
	if (is_fs_mnt && !root) {
		print_message("Can't find\nGif folder!\n");
		vTaskDelay(1000 / portTICK_PERIOD_MS);
	}

	vTaskDelay(1 / portTICK_PERIOD_MS);
}

