#include "Layout.hpp"

#include <Preferences.h>
#include <string.h>

// The boards this firmware ran on as separate build envs (git history:
// platformio.ini before the single env).
const Layout LAYOUTS[] = {
    // id, name, panel_w × panel_h, chain, gb_swapped, E pin, fm6124, rows × cols, tile_w × tile_h, map, depth, brightness
    {"banane", "Banane 64×32", 64, 32, 1, false, -1, false, 1, 1, 64, 32, {0}, 5, 50},
    {"printer", "Printer 128×32 (2 panels)", 64, 32, 2, true, -1, false, 1, 1, 128, 32, {0}, 6, 100},
    {"cross", "Cross 96×96 (5 panels)", 160, 32, 1, true, -1, false, 3, 3, 32, 32, {-1, 4, -1, 1, 2, 3, -1, 0, -1}, 5, 50},
    {"ricard", "Ricard 64×96 (6 panels)", 192, 32, 1, true, -1, true, 3, 2, 32, 32, {0, 1, 2, 3, 4, 5}, 5, 50},
    {"mirror", "64×64, E pin 12", 64, 64, 1, true, 12, false, 1, 1, 64, 64, {0}, 5, 50},
    {"64x64", "64×64, no E pin", 64, 64, 1, false, -1, false, 1, 1, 64, 64, {0}, 5, 50},
    {"32x32", "32×32", 32, 32, 1, false, -1, false, 1, 1, 32, 32, {0}, 7, 50},
};
const uint8_t LAYOUT_COUNT = sizeof(LAYOUTS) / sizeof(LAYOUTS[0]);
const Layout* layout = &LAYOUTS[0];
uint16_t matrix_w = 64, matrix_h = 32;

const Layout* layout_find(const char* id) {
	for (const Layout& l : LAYOUTS)
		if (strcmp(l.id, id) == 0)
			return &l;
	return nullptr;
}

void layout_begin() {
	Preferences p;
	p.begin("matrix", true);
	String id = p.getString("layout", LAYOUTS[0].id);
	p.end();
	if (const Layout* l = layout_find(id.c_str()))
		layout = l;
	matrix_w = layout->cols * layout->tile_w;
	matrix_h = layout->rows * layout->tile_h;
}

bool layout_save(const char* id) {
	if (!layout_find(id))
		return false;
	Preferences p;
	p.begin("matrix", false);
	p.putString("layout", id);
	p.remove("depth");  // the new layout's default
	p.end();
	return true;
}

// DMA memory of the display (double buffered): pixel data, 2 bytes per
// pixel of a row pair per bit, plus the DMA descriptors, which grow with
// the bits (more passes for the high bits) and the rows. Descriptors measured
// on a 64×32 panel (KB); free heap there in BLE mode: 64 KB at 5 bits,
// 37 KB at 7, 9 KB at 8 (not enough).
uint32_t display_bytes(const Layout& l, uint8_t bits) {
	static const uint16_t DESC_32_ROWS[] = {0, 0, 5 * 1024, 6 * 1024, 8 * 1024, 11 * 1024, 17 * 1024, 30 * 1024, 54 * 1024};
	uint32_t data = uint32_t(l.panel_w) * l.chain * l.panel_h * bits * 2;
	uint32_t desc = bits < 9 ? DESC_32_ROWS[bits] * l.panel_h / 32 : UINT32_MAX / 2;
	return data + desc;
}

// BLE: what a 64×32 at 7 bits takes (8: 9 KB left, BLE fails), WiFi: at 5
// bits (6 was too tight)
static uint32_t budget(bool wifi) {
	return wifi ? 32 * 1024 : 71 * 1024;
}

uint8_t layout_depth_max(const Layout& l, bool wifi) {
	const uint8_t lowest = wifi ? 4 : 2;  // WiFi at fewer bits: not worth it
	for (uint8_t bits = 8; bits >= lowest; bits--)
		if (display_bytes(l, bits) <= budget(wifi))
			return bits;
	return wifi ? 0 : 2;
}
