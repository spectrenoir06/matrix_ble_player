#pragma once

#include <stdint.h>

namespace SpectrePng {
  // Decode a PNG file and show it centered (the caller stops GIF / Lua first).
  // Returns 0, or a spectre::error code (NoMemory, BadArgs for an invalid PNG).
  uint16_t show(const char* path);
}
