#ifdef USE_SD
	#include "FS.h"
	#include "SD.h"
	#include "SPI.h"
	#define filesystem SD
#endif
#ifdef USE_SD_MMC
	#include "FS.h"
	#include "SD_MMC.h"
	#define filesystem SD_MMC
#endif
#ifdef USE_SPIFFS
	#include "SPIFFS.h"
	#define filesystem SPIFFS
#endif
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <NimBLEDevice.h>
#include "Gif.hpp"
#include "Lua.hpp"

#define SERVICE_UUID           "6E400001-B5A3-F393-E0A9-E50E24DCCA9E" // UART service UUID
#define CHARACTERISTIC_UUID_RX "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_TX "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

#define DEFAULT_HOSTNAME	HOSTNAME
#define AP_SSID				HOSTNAME

const int SD_CS = 13;
const int SD_MOSI = 15;
const int SD_SCK = 14;
const int SD_MISO = 2;

MatrixPanel_I2S_DMA *display = nullptr;

uint8_t next_anim = 1;

char	hostname[50] = DEFAULT_HOSTNAME;

uint8_t* leds;
uint8_t* buffer;
uint8_t brightness = BRIGHTNESS;

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

float gif_scale;
int gif_off_x;
int gif_off_y;

uint8_t button_isPress = 0;

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
	display->fillScreen(0);
	display->setCursor(4, 4);
	display->setTextSize(1);
	display->setTextColor(display->color565(255,255,255));
	display->printf(str);
	display->fillRect(4, 16, 64 - 4 * 2, 8, 0xFFFF);
	CRGB rgb;
	hsv2rgb_spectrum(CHSV(hue+10, 255, 255), rgb);
	display->fillRect(
		4+1,
		16+1,
		map(byte_to_store, 0, data_size, 0, (64 - 4 * 2 - 2)),
		8 - 2,
		display->color565(rgb.r, rgb.g, rgb.b)
	);
	flip_matrix();
}

void print_message(const char *str) {
	Serial.printf("print_message: %s", str);
	display->fillScreen(0);
	display->setCursor(0, 0);
	display->setTextSize(1);
	display->setTextColor(display->color565(255,255,255));
	display->setTextWrap(true);
	display->print(str);
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
							if (rxValue[3] == '1')
								next_anim = 1;
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
						for (int i = 7; i < rxValue.length(); i++) {
							leds[byte_to_store++] = rxValue[i];

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
				case 'P':
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
					leds[byte_to_store] = rxValue[i];
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
			timeout_var = millis() + timeout_time;
			if (byte_to_store >= data_size) {
				Serial.printf("Image complete\n");
				display->fillScreenRGB888(0, 0, 0);
				for (int i = 0; i < (img_receive_width * img_receive_height); i++) {
					if (img_receive_color_depth == 16)
						display->drawPixel((i) % img_receive_width, (i) / img_receive_width, leds[i * 2] + (leds[i * 2 + 1] << 8));
					else
						display->drawPixelRGB888((i) % img_receive_width, (i) / img_receive_width, leds[i*3], leds[i*3+1], leds[i*3+2]);
				}
				flip_matrix();
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
				print_progress("load lua:");
			}
		}
	}
};


void load_anim(File file) {
	Serial.printf("load_anim !\n");
	if (!file) {
		print_message("Can't find\nGif file!");
		return;
	}
	Serial.printf("Open animation: '%s'\n", file.path());
	display->clearScreen();
	
	SpectreGif::play(file.path());
	Serial.print("load anim: ");
	Serial.print(file.name());
	Serial.print("\tsize: ");
	Serial.print(file.size() / (1024.0 * 1024.0));
	Serial.println(" Mo");

	if (deviceConnected) {
		char str[100];
		memset(str, 0, 100);
		strcat(str, "!P");
		strcat(str, file.name());
		strcat(str, "\r\n");
		pTxCharacteristic->setValue((uint8_t*)str, strlen(str));
		pTxCharacteristic->notify();
	}
}

#define MIN(a,b) (((a)<(b))?(a):(b))
#define BUF_SIZE (256*1)

void playAnimeTask(void* parameter) {
	File root;
	root = filesystem.open("/GIF");

	for (;;) {
		//Serial.printf("loop %s \n", root.path());
		vTaskDelay(1 / portTICK_PERIOD_MS);
		if (!root) {
			print_message("Can't find\nGif folder!");
		}
		if (gif_receive_mode) {
			// Check for timeouts
			if (timeout_var != 0 && millis() > timeout_var) {
				image_receive_mode = false;
				gif_receive_mode = false;
				Serial.printf("timemout\n");
				timeout_var = 0;
				next_anim = 1;
			}
		}

		if (digitalRead(0) == LOW || next_anim) {
			if (!button_isPress || next_anim) {
				button_isPress = 1;
				next_anim = 0;
				File file = root.openNextFile();
				if (!file) {
					root.close();
					root = filesystem.open("/GIF");
					file = root.openNextFile();
				}
				load_anim(file);
			}
		} else
			button_isPress = 0;

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
	}

	Serial.println("Ending task playAnimeTask");
	vTaskDelete(NULL);
}

void setup() {
	psramInit();
	Serial.begin(115200);

	Serial.println("\n------------------------------");
	Serial.printf("  LEDs driver\n");
	Serial.printf("  Hostname: %s\n", hostname);
	int core = xPortGetCoreID();
	Serial.print("  Main code running on core ");
	Serial.println(core);
	Serial.println("------------------------------");

	leds = (uint8_t*)malloc(LED_TOTAL * LED_SIZE);
	if (!leds)
		Serial.println("Can't allocate leds");

	Serial.println("LEDs driver start");

	HUB75_I2S_CFG::i2s_pins _pins = {R1_PIN, G1_PIN, B1_PIN, R2_PIN, G2_PIN, B2_PIN, A_PIN, B_PIN, C_PIN, D_PIN, E_PIN, LAT_PIN, OE_PIN, CLK_PIN};
	
	HUB75_I2S_CFG mxconfig(
		MATRIX_W,     // Module width
		MATRIX_H,     // Module height
		MATRIX_CHAIN, // chain length
		_pins         // pin mapping
	);

	mxconfig.double_buff = true;                   // use DMA double buffer (twice as much RAM required)
	// #ifndef DMA_DOUBLE_BUFF
	// #endif
	mxconfig.driver          = HUB75_I2S_CFG::SHIFTREG; // Matrix driver chip type - default is a plain shift register
	mxconfig.i2sspeed        = HUB75_I2S_CFG::HZ_10M;   // I2S clock speed
	mxconfig.clkphase        = true;                    // I2S clock phase
	mxconfig.latch_blanking  = MATRIX_LATCH_BLANK;      // How many clock cycles to blank OE before/after LAT signal change, default is 1 clock

	display = new MatrixPanel_I2S_DMA(mxconfig);

	display->begin();  // setup display with pins as pre-defined in the library

	set_brightness(BRIGHTNESS);

	#ifdef USE_SD
		// Initialize SD card
		SPI.begin(SD_SCK, SD_MISO, SD_MOSI);
		for (int i=0; i<20; i++) {
			if (!filesystem.begin(SD_CS, SPI)) {
				Serial.println("Card Mount Failed");
				print_message("Can't mnt\nSD Card!");
				delay(10);
			} else {
				break;
			}
		}
	#endif

	#ifdef USE_SD_MMC
		pinMode(2, INPUT_PULLUP);
		pinMode(15, PULLUP);
		pinMode(14, PULLUP);
		pinMode(13, PULLUP);
		if (!filesystem.begin("/sdcard", true)) {
			Serial.println("Card Mount Failed");
			print_message("Can't mnt\nSD Card!");
		} else {
			// root = filesystem.open("/");
			// file = root.openNextFile();
		}
	#endif

	#ifdef USE_SPIFFS
		if (!filesystem.begin(true)) {
			Serial.println("An Error has occurred while mounting SPIFFS");
			print_message("Can't mnt\nSPIFFS!");
			// ESP.restart();
		} else {
			Serial.println("mounting SPIFFS OK");
		}
	#endif

	// xTaskCreatePinnedToCore(
	// 	playAnimeTask,   /* Task function. */
	// 	"playAnimeTask", /* String with name of task. */
	// 	4096,  /* Stack size in bytes. */
	// 	NULL,	   /* Parameter passed as input of the task */
	// 	5,		   /* Priority of the task. */
	// 	&animeTaskHandle,	   /* Task handle. */
	// 	1
	// );

	Serial.println("Start BLE");
	// Create the BLE Device
	NimBLEDevice::init(HOSTNAME);
	NimBLEDevice::setPower(ESP_PWR_LVL_P9);

	// NimBLEDevice::setSecurityAuth(/*BLE_SM_PAIR_AUTHREQ_BOND | BLE_SM_PAIR_AUTHREQ_MITM |*/ BLE_SM_PAIR_AUTHREQ_SC);

	// Create the BLE Server
	NimBLEServer* pServer = NimBLEDevice::createServer();
	pServer->setCallbacks(new MyServerCallbacks());

	// Create the BLE Service
	NimBLEService* pService = pServer->createService(SERVICE_UUID);

	// Create a BLE Characteristic
	pTxCharacteristic = pService->createCharacteristic(
		CHARACTERISTIC_UUID_TX,
		NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ
	);

	BLECharacteristic* pRxCharacteristic = pService->createCharacteristic(
		CHARACTERISTIC_UUID_RX,
		NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::READ// | NIMBLE_PROPERTY::WRITE_NR
	);

	pRxCharacteristic->setCallbacks(new MyCallbacks());

	// Start the service
	pService->start();

	BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
	// pAdvertising->setAppearance(0x7<<6); // glasses
	pAdvertising->setAppearance(0x01F << 6 | 0x06); // LEDs 
	
	SpectreGif::init();
	Lua::init();
	Serial.println("::init() OK");

	// Start advertising
	pServer->getAdvertising()->start();
	Serial.println("Waiting a client connection to notify...");
}

void loop(void) {
	vTaskDelay(20 / portTICK_PERIOD_MS);
	return playAnimeTask(NULL);
}

