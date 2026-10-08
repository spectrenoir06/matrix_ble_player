// PNG decoder on the ESP32 ROM's inflate (miniz tinfl): small enough for the
// shared arena next to everything else (PNGdec needed 45 KB: a 32 KB zlib
// window). Everything is decompressed straight into the arena, so the raw
// image (height × (1 + row bytes)) must fit there: 64×64 RGBA does.
// Non-interlaced PNGs, any color type, 1–16 bits; alpha is blended over black.

#include <Arduino.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <Mapping.h>
#include <esp32/rom/miniz.h>

#include <SpectreProtocol.h>

#include "Arena.hpp"
#include "Png.hpp"
#include "Log.hpp"

#ifdef USE_SD
	#include "SD.h"
	#define filesystem SD
#endif
#ifdef USE_SPIFFS
	#include "SPIFFS.h"
	#define filesystem SPIFFS
#endif

extern VirtualMatrixPanel *virtualDisp;
extern void flip_matrix();

namespace {

  enum ColorType : uint8_t { PngGray = 0, PngRGB = 2, PngPalette = 3, PngGrayAlpha = 4, PngRGBA = 6 };

  // Laid out at the start of the arena; the raw image follows.
  struct Work {
    tinfl_decompressor inflate;
    uint8_t in[1024];            // IDAT bytes being inflated
    uint8_t palette[256][4];     // RGBA (tRNS alpha, 255 if none)
  };
  static_assert(sizeof(Work) < Arena::SIZE, "PNG work area does not fit in the arena");

  uint32_t be32(const uint8_t* b) { return uint32_t(b[0]) << 24 | b[1] << 16 | b[2] << 8 | b[3]; }

  bool read_exact(File& f, void* buf, size_t n) { return f.read(static_cast<uint8_t*>(buf), n) == n; }

  uint8_t paeth(int a, int b, int c) {
    int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
  }

  // Undo the per-row filters in place. bpp: bytes per pixel (at least 1).
  bool unfilter(uint8_t* raw, uint32_t height, uint32_t stride, uint32_t bpp) {
    uint8_t* prev = nullptr;
    for (uint32_t y = 0; y < height; y++) {
      uint8_t* row = raw + y * (stride + 1);
      uint8_t filter = row[0];
      uint8_t* p = row + 1;
      for (uint32_t i = 0; i < stride; i++) {
        uint8_t a = i >= bpp ? p[i - bpp] : 0;
        uint8_t b = prev ? prev[i] : 0;
        uint8_t c = prev && i >= bpp ? prev[i - bpp] : 0;
        switch (filter) {
          case 0: break;
          case 1: p[i] += a; break;
          case 2: p[i] += b; break;
          case 3: p[i] += (a + b) / 2; break;
          case 4: p[i] += paeth(a, b, c); break;
          default: return false;
        }
      }
      prev = p;
    }
    return true;
  }

  // One sample (0..255) of a row: bit depths 1, 2, 4, 8 and 16 (high byte).
  uint8_t sample(const uint8_t* p, uint32_t index, uint8_t depth) {
    switch (depth) {
      case 8: return p[index];
      case 16: return p[index * 2];
      default: {
        uint32_t bit = index * depth;
        uint8_t v = (p[bit / 8] >> (8 - depth - bit % 8)) & ((1 << depth) - 1);
        return v * 255 / ((1 << depth) - 1);
      }
    }
  }
  // Palette index (not scaled).
  uint8_t index_at(const uint8_t* p, uint32_t x, uint8_t depth) {
    if (depth == 8)
      return p[x];
    uint32_t bit = x * depth;
    return (p[bit / 8] >> (8 - depth - bit % 8)) & ((1 << depth) - 1);
  }

}

namespace SpectrePng {

  uint16_t show(const char* path) {
    // decoder + raw image in the shared arena (GIF and Lua are stopped by the caller)
    if (!Arena::data())
      return spectre::error::NoMemory;
    File f = filesystem.open(path);
    if (!f)
      return spectre::error::NotFound;

    Work* w = static_cast<Work*>(Arena::data());
    uint8_t* raw = static_cast<uint8_t*>(Arena::data()) + sizeof(Work);
    const size_t raw_cap = Arena::SIZE - sizeof(Work);

    auto fail = [&](const char* why) -> uint16_t {
      Log.printf("PNG %s: %s\n", path, why);
      f.close();
      return spectre::error::BadArgs;
    };

    uint8_t sig[8];
    static const uint8_t SIGNATURE[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    if (!read_exact(f, sig, 8) || memcmp(sig, SIGNATURE, 8) != 0)
      return fail("not a PNG");

    uint32_t width = 0, height = 0, stride = 0, raw_size = 0, out = 0;
    uint8_t depth = 0, type = 0, channels = 0;
    bool started = false, inflated = false, done = false;
    for (int i = 0; i < 256; i++) {
      w->palette[i][0] = w->palette[i][1] = w->palette[i][2] = 0;
      w->palette[i][3] = 255;
    }

    while (!done) {
      uint8_t head[8];
      if (!read_exact(f, head, 8))
        return fail("truncated");
      uint32_t len = be32(head);
      const char* chunk = reinterpret_cast<const char*>(head + 4);

      if (!memcmp(chunk, "IHDR", 4)) {
        uint8_t h[13];
        if (len != 13 || !read_exact(f, h, 13))
          return fail("bad header");
        width = be32(h);
        height = be32(h + 4);
        depth = h[8];
        type = h[9];
        if (h[12] != 0)
          return fail("interlaced PNGs are not supported");
        channels = type == PngGray || type == PngPalette ? 1 : type == PngGrayAlpha ? 2 : type == PngRGB ? 3 : type == PngRGBA ? 4 : 0;
        if (!channels || !width || !height || width > 1024 || height > 1024)
          return fail("unsupported format");
        stride = (width * channels * depth + 7) / 8;
        raw_size = height * (stride + 1);
        if (raw_size > raw_cap) {
          Log.printf("PNG %s: %ux%u is too big to decode (%u B, %u B max)\n", path, width, height, raw_size, raw_cap);
          f.close();
          return spectre::error::NoMemory;
        }
        tinfl_init(&w->inflate);
      } else if (!memcmp(chunk, "PLTE", 4)) {
        for (uint32_t i = 0; i < len / 3 && i < 256; i++)
          if (!read_exact(f, w->palette[i], 3))
            return fail("truncated");
        f.seek(f.position() + len - (len / 3 < 256 ? len / 3 : 256) * 3);
      } else if (!memcmp(chunk, "tRNS", 4) && type == PngPalette) {
        for (uint32_t i = 0; i < len && i < 256; i++)
          if (!read_exact(f, &w->palette[i][3], 1))
            return fail("truncated");
        if (len > 256)
          f.seek(f.position() + len - 256);
      } else if (!memcmp(chunk, "IDAT", 4)) {
        if (!stride)
          return fail("no header");
        started = true;
        while (len > 0) {
          size_t n = len < sizeof(w->in) ? len : sizeof(w->in);
          if (!read_exact(f, w->in, n))
            return fail("truncated");
          len -= n;
          size_t pos = 0;
          while (pos < n && !inflated) {
            size_t in_size = n - pos, out_size = raw_size - out;
            tinfl_status st = tinfl_decompress(&w->inflate, w->in + pos, &in_size, raw, raw + out, &out_size,
                                               TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_HAS_MORE_INPUT |
                                                   TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
            pos += in_size;
            out += out_size;
            if (st < 0)
              return fail("corrupt data");
            if (st == TINFL_STATUS_DONE)
              inflated = true;
            if (st == TINFL_STATUS_HAS_MORE_OUTPUT)
              return fail("more data than the image size");
          }
        }
      } else if (!memcmp(chunk, "IEND", 4)) {
        done = true;
      } else if (!(chunk[0] & 0x20)) {
        return fail("unknown critical chunk");
      } else {
        f.seek(f.position() + len);  // ancillary: skip
      }
      if (!done)
        f.seek(f.position() + 4);  // CRC (zlib's adler-32 checks the image data)
    }
    f.close();
    if (!started || !inflated || out != raw_size)
      return fail("incomplete image data");

    uint32_t bpp = (channels * depth + 7) / 8;
    if (!unfilter(raw, height, stride, bpp ? bpp : 1))
      return fail("bad filter");

    int off_x = (V_MATRIX_WIDTH - (int)width) / 2;
    int off_y = (V_MATRIX_HEIGHT - (int)height) / 2;
    Log.printf("PNG %s: %ux%u\n", path, width, height);
    virtualDisp->clearScreen();
    for (uint32_t y = 0; y < height; y++) {
      int py = off_y + (int)y;
      if (py < 0 || py >= V_MATRIX_HEIGHT)
        continue;
      const uint8_t* p = raw + y * (stride + 1) + 1;
      for (uint32_t x = 0; x < width; x++) {
        int px = off_x + (int)x;
        if (px < 0 || px >= V_MATRIX_WIDTH)
          continue;
        uint8_t r, g, b, a = 255;
        switch (type) {
          case PngGray: r = g = b = sample(p, x, depth); break;
          case PngGrayAlpha: r = g = b = sample(p, x * 2, depth); a = sample(p, x * 2 + 1, depth); break;
          case PngRGB: r = sample(p, x * 3, depth); g = sample(p, x * 3 + 1, depth); b = sample(p, x * 3 + 2, depth); break;
          case PngRGBA:
            r = sample(p, x * 4, depth); g = sample(p, x * 4 + 1, depth); b = sample(p, x * 4 + 2, depth);
            a = sample(p, x * 4 + 3, depth);
            break;
          default: {  // Palette
            const uint8_t* c = w->palette[index_at(p, x, depth)];
            r = c[0]; g = c[1]; b = c[2]; a = c[3];
          }
        }
        if (a != 255) {  // over black
          r = r * a / 255; g = g * a / 255; b = b * a / 255;
        }
        virtualDisp->drawPixel(px, py, virtualDisp->color565(r, g, b));
      }
    }
    flip_matrix();
    return 0;
  }

}
