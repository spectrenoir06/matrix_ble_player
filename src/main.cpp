#include <Mapping.h>

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
#include "Gif.hpp"
#include "Lua.hpp"

#define LED_SIZE 3
#define LED_TOTAL (MATRIX_WIDTH*V_MATRIX_HEIGHT)

#define SERVICE_UUID           "6E400001-B5A3-F393-E0A9-E50E24DCCA9E" // UART service UUID
#define CHARACTERISTIC_UUID_RX "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_TX "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

#define SERVICE_UUID_SPECTRE          "00004242-0000-1000-8000-00805F9B34FB" // Spectre service UUID
#define CHARACTERISTIC_UUID_FIRMWARE  "00004243-0000-1000-8000-00805F9B34FB"
#define CHARACTERISTIC_UUID_HARDWARE  "00004244-0000-1000-8000-00805F9B34FB"
#define CHARACTERISTIC_UUID_ENV       "00004245-0000-1000-8000-00805F9B34FB"
#define CHARACTERISTIC_UUID_GIT       "00004246-0000-1000-8000-00805F9B34FB"
#define CHARACTERISTIC_UUID_BRIGHT    "00004247-0000-1000-8000-00805F9B34FB"
#define CHARACTERISTIC_UUID_MATRIX_LX "00004248-0000-1000-8000-00805F9B34FB"
#define CHARACTERISTIC_UUID_MATRIX_LY "00004249-0000-1000-8000-00805F9B34FB"
#define CHARACTERISTIC_UUID_MATRIX_VX "00004250-0000-1000-8000-00805F9B34FB"
#define CHARACTERISTIC_UUID_MATRIX_VY "00004251-0000-1000-8000-00805F9B34FB"

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

uint8_t* img_buffer = NULL;
uint8_t brightness = BRIGHTNESS;
File root;

NimBLECharacteristic* pTxCharacteristic;
bool deviceConnected = false;
bool oldDeviceConnected = false;
uint8_t txValue = 0;
uint8_t image_receive_mode = false;
uint8_t gif_receive_mode = false;
uint8_t lua_receive_mode = false;
uint32_t byte_to_store = 0;
uint32_t data_size = 0;
uint32_t img_receive_width = 0;
uint32_t img_receive_height = 0;
uint32_t img_receive_color_depth = 0;
File f_tmp;
uint8_t change_anim = 0;
int timeout_var = 0;
#define timeout_time 3000; // ms
int time_reveice = 0;
String lua_script = "";
uint8_t list_send_mode = false;

uint8_t button_isPress = 0;
VirtualMatrixPanel  *virtualDisp = nullptr;
uint8_t is_fs_mnt = false;

int sendBLE(const char *cstr) {
	if (deviceConnected) {
		pTxCharacteristic->setValue((uint8_t*)cstr, strlen(cstr));
		pTxCharacteristic->notify();
	}
	return 0;
}

void flip_matrix() {
	display->flipDMABuffer();
}

void set_brightness(int b) {
	brightness = constrain(b, 0, 255);
	Serial.printf("Brightness set to %d\n", brightness);
	display->setBrightness8(brightness); //0-255
}

void set_all_pixel(uint8_t r, uint8_t g, uint8_t b, uint8_t w) {
	delay(100);
	display->fillScreenRGB888(r,g,b);
	flip_matrix();
}


uint16_t hue = 0;

void print_progress(const char *str) {
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
		map(byte_to_store, 0, data_size, 0, ((V_MATRIX_WIDTH) - 4 * 2 - 2)),
		8 - 2,
		rgb.r,
		rgb.g,
		rgb.b
	);
	flip_matrix();
}

void print_message(const char *str) {
	Serial.printf("print_message: %s", str);
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
		deviceConnected = true;
		pServer->updateConnParams(desc->conn_handle, 0x6, 0x6, 0, 100);
	};

	void onDisconnect(NimBLEServer* pServer) {
		deviceConnected = false;
	}

	void onMTUChange (uint16_t MTU, ble_gap_conn_desc *desc) {
		Serial.printf("MTU change: %d\n", MTU);
	}
};
	

class MyCallbacks : public NimBLECharacteristicCallbacks {
	void onWrite(NimBLECharacteristic* pCharacteristic) {
		std::string rxValue = pCharacteristic->getValue();
		Serial.printf("Received Value: %d:\n", rxValue.length());
		// for (int i = 0; i < rxValue.length(); i++)
			// Serial.print(rxValue[i]);
		// Serial.println();

		if (rxValue.length() > 0 && rxValue[0] == '!' && !image_receive_mode && !gif_receive_mode) {
			switch (rxValue[1]) {
				case 'B':
					switch (rxValue[2]) {
						case '1':
							// if (rxValue[3] == '1')
							// 	next_anim = 1;
							break;
						case '5':
								set_brightness(brightness+2);
							break;
						case '6':
							if (rxValue[3] == '1')
								set_brightness(brightness-2);
							break;
						case '2':
							if (rxValue[3] == '1') {
								SpectreGif::stop();
								set_all_pixel(0, 0, 0, 255);
							}
							break;
						default:
							break;
					}
					break;
				case 'C':
					Lua::stop();
					set_all_pixel(rxValue[2], rxValue[3], rxValue[4], 0);
					break;
				case 'I':
					{
						Lua::stop();
						SpectreGif::stop();
						img_receive_color_depth = rxValue[2];
						img_receive_width = rxValue[3] + (rxValue[4] << 8);
						img_receive_height = rxValue[5] + (rxValue[6] << 8);
						Serial.printf("Image: depth: %d, %dX%d\n", img_receive_color_depth, img_receive_width, img_receive_height);
						image_receive_mode = true;
						byte_to_store = 0;
						data_size = img_receive_width * img_receive_height * (img_receive_color_depth / 8);
						if (img_buffer)
							free(img_buffer);
						img_buffer = (uint8_t*)malloc(data_size+1);
						if (!img_buffer) {
							print_message("not enought ram\n");
							break;
						}

						for (int i = 7; i < rxValue.length(); i++) {
							img_buffer[byte_to_store++] = rxValue[i];

						}
						timeout_var = millis() + timeout_time;
					}
					break;
				case 'L': // List files
						list_send_mode = true;
					break;
				case 'D': // delete file
					{
						const char* data = rxValue.c_str();
						char str[255];
						memset(str, 0, 255);
						strcat(str, "/GIF/");
						strcat(str, data+2);
						Serial.printf("Remove %s\n", str);
						if (SpectreGif::isPlaying(str)) {
							Serial.printf("The gif is playing\n");
							SpectreGif::stop();
						}
						filesystem.remove(str);
					}
					break;
				case 'G': // add file
					{
						Lua::stop();
						SpectreGif::stop();
						gif_receive_mode = true;
						const char* data = rxValue.c_str();
						char str[255];
						memset(str, 0, 255);
						strcat(str, "/GIF/");
						strcat(str, data + 2 + 4);
						Serial.printf("add %s\n", data + 2 + 4);
						f_tmp = filesystem.open(str, "w", true);
						int len = strlen(data + 2 + 4);
						data_size = *(uint32_t*)(data + 2);
						byte_to_store = 0;
						Serial.printf("gif size = %d\n", data_size);
						for (int i = 2 + 4 + len + 1; i < rxValue.length(); i++) {
							f_tmp.write(rxValue[i]);
							byte_to_store++;
						}
						timeout_var = millis() + timeout_time;
						time_reveice = millis();
						flip_matrix();

					}
					break;
				case 'P': // Play a store gif
					{
						const char* data = rxValue.c_str();
						char str[255];
						memset(str, 0, 255);
						strcat(str, "/GIF/");
						strcat(str, data+2);
						char *ptr = strchr(str, '\n');
						if (ptr)
							*ptr = 0;
						Serial.printf("Open %s\n", str);
						Lua::stop();
						File file = filesystem.open(str);
						preferences.putString("anim", file.path());
						SpectreGif::play(file.path());
					}
					break;
				case 'S':
					{
						SpectreGif::stop();
						lua_receive_mode = true;
						const char* data = rxValue.c_str();
						data_size = *(uint32_t*)(data + 2);
						Serial.printf("load lua size:%d\n", data_size);
						byte_to_store = 0;
						Lua::stop();
						lua_script = "";
						for (int i = 2 + 4; i < rxValue.length(); i++) {
							lua_script += rxValue[i];
							byte_to_store++;
						}
						timeout_var = millis() + timeout_time;
						time_reveice = millis();
					}
					break;
				case 'E':
					{
						const char* data = rxValue.c_str();
						brightness = atoi(data+2);
						set_brightness(brightness);
					}
					break;
				default:
					{
						Serial.printf("default switch\n");
						for (int i = 0; i < rxValue.length(); i++)
							Serial.print(rxValue[i]);
						Serial.println();
					}
					break;
			}
		}
		else if (image_receive_mode) {
			for (int i = 0; i < rxValue.length(); i++) {
				if (byte_to_store < (LED_TOTAL * LED_SIZE))
					img_buffer[byte_to_store] = rxValue[i];
				byte_to_store++;
			}
		}
		else if (gif_receive_mode) {
			for (int i = 0; i < rxValue.length(); i++) {
				f_tmp.write(rxValue[i]);
				byte_to_store++;
			}
		}
		else if (lua_receive_mode) {
			for (int i = 0; i < rxValue.length(); i++) {
				lua_script += rxValue[i];
				byte_to_store++;
			}
		}

		if (image_receive_mode) {
			Serial.printf("Byte receive: %d, wait: %d\n", byte_to_store, data_size - byte_to_store);
			int off_x = (MATRIX_WIDTH  - img_receive_width )/2;
			int off_y = (MATRIX_HEIGHT - img_receive_height)/2;
			timeout_var = millis() + timeout_time;
			if (byte_to_store >= data_size) {
				Serial.printf("Image complete\n");
				display->fillScreenRGB888(0, 0, 0);
				for (int i = 0; i < (img_receive_width * img_receive_height); i++) {
					if (img_receive_color_depth == 16)
						display->drawPixel(off_x + (i) % img_receive_width, off_y + (i) / img_receive_width, img_buffer[i * 2] + (img_buffer[i * 2 + 1] << 8));
					else
						display->drawPixelRGB888(off_x + (i) % img_receive_width, off_y + (i) / img_receive_width, img_buffer[i * 3], img_buffer[i * 3 + 1], img_buffer[i * 3 + 2]);
				}
				flip_matrix();
				free(img_buffer);
				img_buffer = nullptr;
				image_receive_mode = false;
				timeout_var = 0;
			}
			else {
				print_progress("load img:");
			}
		}

		if (gif_receive_mode) {
			Serial.printf("Byte receive: %d, wait: %d\n", byte_to_store, data_size - byte_to_store);
			timeout_var = millis() + timeout_time;
			if (byte_to_store >= data_size) {
				gif_receive_mode = false;
				Serial.printf("receive GIF OK\n");
				preferences.putString("anim", f_tmp.path());
				SpectreGif::play(f_tmp.path());
				f_tmp.close();
				timeout_var = 0;
				Serial.printf("time to receive gif: %dms\n", millis() - time_reveice);
			}
			else {
				print_progress("load gif:");
			}
		}
		

		if (lua_receive_mode) {
			Serial.printf("Byte receive: %d, wait: %d\n", byte_to_store, data_size - byte_to_store);
			timeout_var = millis() + timeout_time;
			if (byte_to_store >= data_size) {
				lua_receive_mode = false;
				Serial.printf("receive Lua OK\n");
				Serial.printf("time to receive Lua: %dms\n", millis() - time_reveice);
				timeout_var = 0;
				Serial.println("[APP] Free memory: " + String(esp_get_free_heap_size()) + " bytes");
				Lua::run_script(lua_script);
			}
			else {
				// print_progress("load lua:");
			}
		}
	}
};


// void load_anim(File file) {
// 	Serial.printf("load_anim !\n");
// 	if (!file) {
// 		print_message("Can't find\nGif file!");
// 		delay(1000);
// 		return;
// 	}
// 	Serial.printf("Open animation: '%s'\n", file.path());
// 	display->clearScreen();
	
// 	SpectreGif::play(file.path());
// 	Serial.print("load anim: ");
// 	Serial.print(file.name());
// 	Serial.print("\tsize: ");
// 	Serial.print(file.size() / (1024.0 * 1024.0));
// 	Serial.println(" Mo");

// 	if (deviceConnected) {
// 		char str[100];
// 		memset(str, 0, 100);
// 		strcat(str, "!P");
// 		strcat(str, file.name());
// 		strcat(str, "\r\n");
// 		pTxCharacteristic->setValue((uint8_t*)str, strlen(str));
// 		pTxCharacteristic->notify();
// 	}
// }

#define MIN(a,b) (((a)<(b))?(a):(b))
#define BUF_SIZE (256*1)

void playAnimeTask(void* parameter) {


	for (;;) {
		
	}

	// Serial.println("Ending task playAnimeTask");
	// vTaskDelete(NULL);
}

void setup() {
	Serial.begin(115200);

	Serial.println("\n------------------------------");
	Serial.printf("  Hub75 LEDs driver\n");
	Serial.printf("  Hostname: %s\n", hostname);
	int core = xPortGetCoreID();
	Serial.print("  Main code running on core ");
	Serial.println(core);
	Serial.println("------------------------------");

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

	display = new MatrixPanel_I2S_DMA(mxconfig);

	display->begin();  // setup display with pins as pre-defined in the library

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
	#else
		int16_t map[1] = {0};
		virtualDisp = new VirtualMatrixPanel((*display), 1, 1, 64, 32, map);
	#endif

	set_brightness(BRIGHTNESS);

	#ifdef USE_SD
		// Initialize SD card
		SPI.begin(SD_SCK, SD_MISO, SD_MOSI);
		for (int i=0; i<20; i++) {
			if (!filesystem.begin(SD_CS, SPI)) {
				Serial.println("Card Mount Failed");
				print_message("Can't mnt\nSD Card!\n");
				delay(10);
			} else {
				is_fs_mnt = 1;
				break;
			}
		}
	#endif

	#ifdef USE_SPIFFS
		if (!filesystem.begin(true)) {
			Serial.println("An Error has occurred while mounting SPIFFS");
			print_message("Can't mnt\nSPIFFS!");
			// ESP.restart();
		} else {
			Serial.println("mounting SPIFFS OK");
			is_fs_mnt = 1;
		}
	#endif

	if (is_fs_mnt) {
		root = filesystem.open("/GIF");
		preferences.begin("matrix", false);
		char str[255];
		if (preferences.getString("anim", str, 255)) {
			File file = filesystem.open(str);
			if (file.size() > 0) {
				Serial.printf("Start previous anim %s, %s\n", str, file.path());
				SpectreGif::play(file.path());
			} else {
				print_message("Gif\nnot loaded");
			}
		} else {
			File file = root.openNextFile();
			if (!file) {
				root.close();
				root = filesystem.open("/GIF");
				file = root.openNextFile();
				if (file)
					SpectreGif::play(file.path());
				else
					print_message("No gif\nfound");
			}
		}
	}

	Serial.println("Start BLE");
	// Create the BLE Device
	NimBLEDevice::init(HOSTNAME);
	NimBLEDevice::setPower(ESP_PWR_LVL_P9);
	NimBLEDevice::setMTU(BLE_ATT_MTU_MAX);

	// NimBLEDevice::setSecurityAuth(/*BLE_SM_PAIR_AUTHREQ_BOND | BLE_SM_PAIR_AUTHREQ_MITM |*/ BLE_SM_PAIR_AUTHREQ_SC);

	// Create the BLE Server
	NimBLEServer* pServer = NimBLEDevice::createServer();
	pServer->setCallbacks(new MyServerCallbacks());

	// Create the BLE Service UART
	NimBLEService* pService = pServer->createService(SERVICE_UUID);

	// Create a BLE Characteristic UART TX
	pTxCharacteristic = pService->createCharacteristic(
		CHARACTERISTIC_UUID_TX,
		NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ
	);

	// Create a BLE Characteristic  UART RX
	BLECharacteristic* pRxCharacteristic = pService->createCharacteristic(
		CHARACTERISTIC_UUID_RX,
		NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::READ// | NIMBLE_PROPERTY::WRITE_NR
	);

	pRxCharacteristic->setCallbacks(new MyCallbacks());

	// Start the service
	pService->start();



	// Create the BLE Service UART
	NimBLEService* pService2 = pServer->createService(SERVICE_UUID_SPECTRE);

	// Create a BLE Characteristic
	pService2->createCharacteristic(CHARACTERISTIC_UUID_FIRMWARE,  NIMBLE_PROPERTY::READ)->setValue("1.0.0");
	// pService2->createCharacteristic(CHARACTERISTIC_UUID_HARDWARE,  NIMBLE_PROPERTY::READ);
	// pService2->createCharacteristic(CHARACTERISTIC_UUID_ENV,       NIMBLE_PROPERTY::READ);
	pService2->createCharacteristic(CHARACTERISTIC_UUID_GIT,       NIMBLE_PROPERTY::READ)->setValue(BUILD_GIT_COMMIT_HASH);
	pService2->createCharacteristic(CHARACTERISTIC_UUID_BRIGHT,    NIMBLE_PROPERTY::READ)->setValue(brightness);
	pService2->createCharacteristic(CHARACTERISTIC_UUID_MATRIX_LX, NIMBLE_PROPERTY::READ)->setValue(MATRIX_WIDTH);
	pService2->createCharacteristic(CHARACTERISTIC_UUID_MATRIX_LY, NIMBLE_PROPERTY::READ)->setValue(MATRIX_HEIGHT);
	pService2->createCharacteristic(CHARACTERISTIC_UUID_MATRIX_VX, NIMBLE_PROPERTY::READ)->setValue(V_MATRIX_WIDTH);
	pService2->createCharacteristic(CHARACTERISTIC_UUID_MATRIX_VY, NIMBLE_PROPERTY::READ)->setValue(V_MATRIX_HEIGHT);

	pService2->start();


	BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
	// pAdvertising->setAppearance(0x7<<6); // glasses
	pAdvertising->setAppearance(0x01F << 6 | 0x06); // LEDs 
	
	SpectreGif::init();
	Lua::init();
	Serial.println("::init() OK");

	// Start advertising
	pServer->getAdvertising()->addServiceUUID(SERVICE_UUID);
	pServer->getAdvertising()->start();
	Serial.println("Waiting a client connection to notify...");
}

void loop(void) {
	//Serial.printf("loop %s \n", root.path());
	if (is_fs_mnt && !root) {
		print_message("Can't find\nGif folder!\n");
		vTaskDelay(1000 / portTICK_PERIOD_MS);
	}

	// Check for timeouts
	if (timeout_var != 0 && millis() > timeout_var) {
		image_receive_mode = false;
		gif_receive_mode = false;
		lua_receive_mode = false;
		Serial.printf("timeout\n");
		timeout_var = 0;
		if (img_buffer) {
			free(img_buffer);
			img_buffer = nullptr;
		}
	}

	if (list_send_mode) {
		list_send_mode = false;
		Serial.printf("Print list files:\n");
		File tmp_root = filesystem.open("/GIF");
		File tmp_file = tmp_root.openNextFile();
		char str[255];
		while(tmp_file) {
			memset(str, 0, 255);
			strcat(str, "!L");
			strcat(str, tmp_file.name());
			pTxCharacteristic->setValue((uint8_t*)str, strlen(str));
			pTxCharacteristic->notify();
			// Serial.println(str);
			tmp_file = tmp_root.openNextFile();
		}
		memset(str, 0, 255);
		strcat(str, "!L!");
		pTxCharacteristic->setValue((uint8_t*)str, strlen(str));
		pTxCharacteristic->notify();
	}
	vTaskDelay(1 / portTICK_PERIOD_MS);
}

