#include <stdint.h>

static inline __attribute__((always_inline)) void to(uint8_t m, uint8_t M, uint16_t h, uint8_t* r, uint8_t* g, uint8_t* b) {
    float k = (h % 60) / 60.0;
    if (h <= 180) {
      if (h <= 60) {
        *r = M;
        *g = m + (M - m) * k;
        *b = m;
      } else if (h <= 120) {
        *r = M - (M - m) * k;
        *g = M;
        *b = m;
      } else { // h <= 180
        *r = m;
        *g = M;
        *b = m + (M - m) * k;
      }
    } else {
      if (h <= 240) {
        *r = m;
        *g = M - (M - m) * k;
        *b = M;
      } else if (h <= 300) {
        *r = m + (M - m) * k;
        *g = m;
        *b = M;
      } else { // h <= 360
        *r = M;
        *g = m;
        *b = M - (M - m) * k;
      }
    }
  }

  static inline __attribute__((always_inline)) void from(uint8_t r, uint8_t g, uint8_t b, uint8_t* m, uint8_t* M, uint16_t* h) {
    float k = 60.0 / (M - m);
    if (r <= g) {
      if (r <= b) {
        if (g <= b) { // r <= g <= b
          *m = r;
          *M = b;
          if (r != b) { // gray
            *h = 180.0 + (b - g) * 60.0 / (b - r);
          }
        } else { // r <= b < g
          *m = r;
          *M = g;
          *h = 120.0 + (b - r) * 60.0 / (g - r);
        }
      } else { // b < r <= g
        *m = b;
        *M = g;
        *h = 60.0 + (g - r) * 60.0 / (g - b);
      }
    } else {
      if (r <= b) { // g < r <= b
        *m = g;
        *M = b;
        *h = 240.0 + (r - g) * 60.0 / (b - g);
      } else {
        if (g <= b) { // g <= b < r
          *m = g;
          *M = r;
          *h = 300.0 + (r - b) * 60.0 / (r - g);
        } else { // b < g < r
          *m = b;
          *M = r;
          *h = 0.0 + (g - b) * 60.0 / (r - b);
        }
      }
    }
  }

namespace HueShift {

  void hue_shift(uint8_t* r, uint8_t* g, uint8_t* b, uint16_t shift) {
    uint8_t m;
    uint8_t M;
    uint16_t h;
    from(*r, *g, *b, &m, &M, &h);
    if (m != M) {
      h = (h + shift) % 360;
      to(m, M, h, r, g, b);
    }
  }

}
