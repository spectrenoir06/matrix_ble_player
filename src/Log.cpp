#include "Log.hpp"

#include <mutex>

#include "Protocol.hpp"

LogPrint Log;

namespace {
	std::mutex lock;  // logs come from several tasks (loop, GIF, Lua…)
}

size_t LogPrint::write(const uint8_t* buf, size_t size) {
	std::lock_guard<std::mutex> guard(lock);
	for (size_t i = 0; i < size; i++) {
		char c = buf[i];
		if (c == '\r')
			continue;
		if (c == '\n' || len_ == sizeof(line_) - 1) {
			emitLine(LogLevel::Info);
			if (c == '\n')
				continue;
		}
		line_[len_++] = c;
	}
	return size;
}

void LogPrint::line(LogLevel level, const char* text) {
	std::lock_guard<std::mutex> guard(lock);
	if (len_)  // finish a pending print() first
		emitLine(LogLevel::Info);
	for (const char* c = text; *c; c++) {
		if (*c == '\r')
			continue;
		if (*c == '\n' || len_ == sizeof(line_) - 1) {
			emitLine(level);
			if (*c == '\n')
				continue;
		}
		line_[len_++] = *c;
	}
	if (len_)
		emitLine(level);
}

void LogPrint::emitLine(LogLevel level) {
	line_[len_] = 0;
	if (!protocol_serial_active()) {
		Serial.write(reinterpret_cast<const uint8_t*>(line_), len_);
		Serial.write("\r\n");
	}
	if (len_)
		protocol_log(level, line_);
	len_ = 0;
}
