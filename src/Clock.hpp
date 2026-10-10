#pragma once

#include <stdint.h>
#include <time.h>

// The time of day, for scripts (Lua getTime): given by the clients when they
// connect (sys.time.set: UTC + their offset), or by NTP on WiFi. The ESP32
// keeps it running; unknown after a power cycle until one of them gives it.
void clock_set(uint32_t unix_utc, int16_t utc_offset_min);
void clock_set_offset(int16_t utc_offset_min);
int16_t clock_offset();
// Local time; false while the time is unknown.
bool clock_local(struct tm& out);
// Ask NTP servers (WiFi up).
void clock_ntp();
