#pragma once

#include <Arduino.h>

enum class LogLevel : uint8_t { Debug = 0, Info = 1, Warning = 2, Error = 3 };

// The firmware's log: use like Serial (Log.printf, Log.println…). Each line is
//  - printed as plain text on Serial, unless a Spectre Protocol client is
//    talking over Serial (its link then only carries frames);
//  - sent as a sys.log event to every connected client (BLE, serial).
class LogPrint : public Print {
public:
	size_t write(uint8_t c) override { return write(&c, 1); }
	size_t write(const uint8_t* buf, size_t size) override;
	// Log `text` (may have several lines) with a level: the print functions use Info.
	void line(LogLevel level, const char* text);

private:
	void emitLine(LogLevel level);

	char line_[160];
	size_t len_ = 0;
};

extern LogPrint Log;
