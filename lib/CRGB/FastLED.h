// Stand-in for FastLED (not a dependency any more): GFX_Root and the HUB75
// library include <FastLED.h> only for the CRGB type, so that is all this
// provides.
#pragma once

#include <stdint.h>

struct CRGB {
	union {
		struct {
			union { uint8_t r; uint8_t red; };
			union { uint8_t g; uint8_t green; };
			union { uint8_t b; uint8_t blue; };
		};
		uint8_t raw[3];
	};

	CRGB() : r(0), g(0), b(0) {}
	CRGB(uint8_t ir, uint8_t ig, uint8_t ib) : r(ir), g(ig), b(ib) {}
	// 0xRRGGBB (implicit, as in FastLED: GFX_Root's setTextColor relies on it)
	CRGB(uint32_t colorcode) : r((colorcode >> 16) & 0xFF), g((colorcode >> 8) & 0xFF), b(colorcode & 0xFF) {}
};
