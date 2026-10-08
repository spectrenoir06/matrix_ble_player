#pragma once

#include <stddef.h>

// One RAM block shared by whatever is playing: the GIF decoder, the PNG
// decoder or the Lua heap. Only one uses it at a time: play_file() stops the
// others (synchronously) before handing it over. Allocated once at boot,
// because later the heap is too fragmented for 45 KB in one piece.
namespace Arena {
  constexpr size_t SIZE = 46 * 1024;  // PNG decoder 45604 B, GIF decoder 23988 B

  // Call once, early in setup(). Returns false if the block could not be allocated.
  bool init();
  // The block, nullptr if init() failed.
  void* data();
}
