#pragma once

#include <stdint.h>

// Direct work on the display's frame buffers (the DMA bit planes), for
// what pixel by pixel calls from Lua would make slow.

// Shift the picture on screen by (dx, dy) pixels (right / down positive)
// into the buffer being drawn: what scrolls in is black, or with `wrap` what
// scrolled out on the other side. Copies the display's own bits (no color
// loss), whatever the panel layout: a 64x32 frame in well under 1 ms.
// The caller then draws the new edge and flips (updateDisplay).
void scroll_display(int dx, int dy, bool wrap);

// The color of the picture's pixel (x, y) in the frame being drawn, as close
// as the color depth keeps it (drawn 200, read back maybe 197). False:
// outside the picture.
bool get_pixel(int x, int y, uint8_t& r, uint8_t& g, uint8_t& b);
