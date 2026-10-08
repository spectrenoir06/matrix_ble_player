#pragma once

#include <stddef.h>

// One RAM block shared by whatever is playing: the GIF decoder, the PNG
// decoder or the Lua heap. Only one uses it at a time: play_file() stops the
// others (synchronously) before handing it over. Allocated once at boot,
// because later the heap is too fragmented for 45 KB in one piece.
namespace Arena {
  // GIF decoder 23988 B; PNG: inflate state (~11 KB) + the raw image (64×64
  // RGBA: 16.4 KB); Lua: its whole heap
  constexpr size_t SIZE = 30 * 1024;

  // Call once, early in setup(). Returns false if the block could not be allocated.
  bool init();
  // The block, nullptr if init() failed.
  void* data();
}
