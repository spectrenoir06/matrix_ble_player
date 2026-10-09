#include "Storage.hpp"

#include <SD.h>
#include <SPI.h>
#include <SPIFFS.h>

#include "Log.hpp"

// the SD card slot of the driver boards
constexpr int SD_CS = 2, SD_SCK = 15, SD_MOSI = 14, SD_MISO = 13;

fs::FS* storage = &SD;
bool storage_is_sd = true;

bool storage_begin() {
	SPI.begin(SD_SCK, SD_MISO, SD_MOSI);
	// 20 MHz SPI (5x faster listings, uploads, GIF reads); the library's
	// default 4 MHz as a fallback for cards / wiring that can't do it.
	// 3 files open at most (default 5): each one reserves a 4 KB buffer.
	for (uint32_t freq : {20000000, 20000000, 4000000}) {
		if (SD.begin(SD_CS, SPI, freq, "/sd", 3)) {
			Log.printf("SD card mounted at %u MHz\n", freq / 1000000);
			return true;
		}
		delay(10);
	}
	SD.end();
	SPI.end();
	Log.println("No SD card: files in SPIFFS");
	storage = &SPIFFS;
	storage_is_sd = false;
	if (SPIFFS.begin(true, "/spiffs", 3))  // formatted the first time
		return true;
	Log.println("SPIFFS mount failed");
	return false;
}

uint64_t storage_total() {
	return storage_is_sd ? SD.totalBytes() : SPIFFS.totalBytes();
}

uint64_t storage_used() {
	return storage_is_sd ? SD.usedBytes() : SPIFFS.usedBytes();
}
