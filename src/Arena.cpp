#include <Arduino.h>

#include "Arena.hpp"
#include "Log.hpp"

namespace {
  void* block = nullptr;
}

namespace Arena {

  bool init() {
    if (!block)
      block = heap_caps_malloc(SIZE, MALLOC_CAP_8BIT);
    Log.printf("Arena: %u B %s, largest free block left %u B\n", SIZE, block ? "ok" : "FAILED",
                  heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    return block != nullptr;
  }

  void* data() {
    return block;
  }

}
