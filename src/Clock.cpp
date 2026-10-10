#include "Clock.hpp"

#include <Arduino.h>
#include <sys/time.h>

namespace {
int16_t offset_min = 0;
constexpr time_t VALID_AFTER = 1700000000;  // 2023: before that, never set
}  // namespace

void clock_set(uint32_t unix_utc, int16_t utc_offset_min) {
	struct timeval tv = {(time_t)unix_utc, 0};
	settimeofday(&tv, nullptr);
	offset_min = utc_offset_min;
}

void clock_set_offset(int16_t utc_offset_min) {
	offset_min = utc_offset_min;
}

int16_t clock_offset() {
	return offset_min;
}

bool clock_local(struct tm& out) {
	time_t now = time(nullptr);
	if (now < VALID_AFTER)
		return false;
	now += (time_t)offset_min * 60;  // the offset applied by hand: no TZ rules needed
	gmtime_r(&now, &out);
	return true;
}

void clock_ntp() {
	configTime(0, 0, "pool.ntp.org", "time.google.com");
}
