#pragma once

#include <stdint.h>

// Panel layouts: one firmware drives every board, the layout is a setting
// (Preferences "matrix"/"layout", matrix.layout.set) instead of a build env.
struct Layout {
	const char* id;    // saved, sent to clients
	const char* name;  // shown in the app
	// the HUB75 chain: `chain` panels of panel_w × panel_h
	uint16_t panel_w, panel_h;
	uint8_t chain;
	bool gb_swapped;  // G and B lines swapped (G1 32, B1 25, G2 26, B2 23)
	int8_t e_pin;     // 1/32 scan panels (64 rows): -1 when not wired
	bool fm6124;      // FM6124 driver chips (latch blanking 5)
	// the picture: rows × cols tiles of tile_w × tile_h, map[row * cols + col]
	// = the tile's index along the chain (-1: no panel). 1 × 1: as wired.
	uint8_t rows, cols;
	uint16_t tile_w, tile_h;
	int8_t map[9];
	uint8_t depth;       // default color depth (bits per color)
	uint8_t brightness;  // at boot
};

extern const Layout LAYOUTS[];
extern const uint8_t LAYOUT_COUNT;
extern const Layout* layout;  // the current one (layout_begin())
// the picture's size (what GIF / PNG / Lua draw on)
extern uint16_t matrix_w, matrix_h;

// Loads the saved layout (Preferences "matrix" must be free to open).
void layout_begin();
const Layout* layout_find(const char* id);
// Saves `id` for the next boot (and drops the saved color depth).
bool layout_save(const char* id);

// The largest color depth whose DMA buffers fit next to the rest: in BLE
// mode, or in WiFi mode (0: WiFi doesn't fit with this layout).
uint8_t layout_depth_max(const Layout& l, bool wifi);
