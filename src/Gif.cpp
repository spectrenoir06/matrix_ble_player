#include <AnimatedGIF.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <Mapping.h>
#include <atomic>
#include <new>

#include "Arena.hpp"
#include "Log.hpp"

#include "Layout.hpp"
#include "Storage.hpp"

uint16_t HHH = 0;

void rgb2hsv(const uint8_t src_r, const uint8_t src_g, const uint8_t src_b, uint16_t *dst_h, uint8_t *dst_s, uint8_t *dst_v){
    float r = src_r / 255.0f;
    float g = src_g / 255.0f;
    float b = src_b / 255.0f;

    float h, s, v; // h:0-360.0, s:0.0-1.0, v:0.0-1.0

    float M = max(r, max(g, b));
    float m = min(r, min(g, b));

    v = M;

    if (M == 0.0f) {
        s = 0;
        h = 0;
    }
    else if (M - m == 0.0f) {
        s = 0;
        h = 0;
    }
    else {
        s = (M - m) / M;

        if (M == r) {
            h = 60 * ((g - b) / (M - m)) + 0;
        }
        else if (M == g) {
            h = 60 * ((b - r) / (M - m)) + 120;
        }
        else {
            h = 60 * ((r - g) / (M - m)) + 240;
        }
    }

    if (h < 0) h += 360.0f;

    *dst_h = (uint16_t)(h);   // dst_h : 0-180
    *dst_s = (unsigned char)(s * 255); // dst_s : 0-255
    *dst_v = (unsigned char)(v * 255); // dst_v : 0-255
}

void hsv2rgb(const uint16_t src_h, const uint8_t src_s, const uint8_t src_v, uint8_t *dst_r, uint8_t *dst_g, uint8_t *dst_b) {
    // src_h = src_h % 360;
    float h = src_h%360; // 0-360
    float s = src_s / 255.0f; // 0.0-1.0
    float v = src_v / 255.0f; // 0.0-1.0

    float r, g, b; // 0.0-1.0

    int   hi = (int)(h / 60.0f) % 6;
    float f  = (h / 60.0f) - hi;
    float p  = v * (1.0f - s);
    float q  = v * (1.0f - s * f);
    float t  = v * (1.0f - s * (1.0f - f));

    switch(hi) {
        case 0: r = v, g = t, b = p; break;
        case 1: r = q, g = v, b = p; break;
        case 2: r = p, g = v, b = t; break;
        case 3: r = p, g = q, b = v; break;
        case 4: r = t, g = p, b = v; break;
        case 5: r = v, g = p, b = q; break;
    }

    *dst_r = (uint8_t)(r * 255); // dst_r : 0-255
    *dst_g = (uint8_t)(g * 255); // dst_r : 0-255
    *dst_b = (uint8_t)(b * 255); // dst_r : 0-255
}

// rgb: 3 bytes, r g b
void color_shift(uint8_t *rgb, int32_t shift) {
    uint8_t s,v;
    uint16_t h;
    if ((rgb[0] != rgb[1]) || (rgb[1] != rgb[2])) {
      rgb2hsv(rgb[0], rgb[1], rgb[2], &h, &s, &v);
      hsv2rgb(h + shift, s, v, &rgb[0], &rgb[1], &rgb[2]);
    }
}

extern VirtualMatrixPanel *virtualDisp;
extern void flip_matrix();
uint8_t disposalMethod = 0;

namespace {
  // Constructed in the shared arena when a GIF starts, nullptr when stopped.
  static AnimatedGIF *gif = nullptr;
  static_assert(sizeof(AnimatedGIF) <= Arena::SIZE, "GIF decoder does not fit in the arena");
  static TaskHandle_t task = NULL;
  static File current_gif_file;
  static uint32_t next_frame_millis = 0;
  static std::atomic<bool> stop_requested(true);
  static std::atomic<bool> active(false);  // the task may be using the arena
  static std::atomic<char*> next_gif_file(nullptr);

  // Draw a line of image directly on the LED Matrix
  void GIFDraw(GIFDRAW *pDraw) {
    disposalMethod = pDraw->ucDisposalMethod;
    uint8_t *s;
    uint8_t *palette;  // RGB888: 3 bytes per color
    int y, iWidth;

    iWidth = pDraw->iWidth;
    if (iWidth > matrix_w)
      iWidth = matrix_w;

    int off_x = (matrix_w  - gif->getCanvasWidth() )/2;
    int off_y = (matrix_h - gif->getCanvasHeight())/2;

    palette = pDraw->pPalette24;
    y = pDraw->iY + pDraw->y; // current line
    s = pDraw->pPixels;
    for (int x = 0; x < pDraw->iWidth; x++) {
      if (!pDraw->ucHasTransparency || *s != pDraw->ucTransparent) {
          const uint8_t *col = palette + *s * 3;
          // color_shift(col, HHH + x*5 + y*5);
          virtualDisp->drawPixelRGB888(off_x + x + pDraw->iX, off_y + y, col[0], col[1], col[2]);
      }
      s++;
    }
  }

  void *GIFOpenFile(const char *fname, int32_t *pSize) {
    current_gif_file = storage->open(fname);
    free((char *)fname);
    if (current_gif_file) {
      *pSize = current_gif_file.size();
      return (void *)&current_gif_file;
    }
    return NULL;
  }

  void GIFCloseFile(void *pHandle) {
    File *f = static_cast<File *>(pHandle);
    if (f != NULL)
      f->close();
  }

  int32_t GIFReadFile(GIFFILE *pFile, uint8_t *pBuf, int32_t iLen) {
    int32_t iBytesRead;
    iBytesRead = iLen;
    File *f = static_cast<File *>(pFile->fHandle);
    // Note: If you read a file all the way to the last byte, seek() stops working
    if ((pFile->iSize - pFile->iPos) < iLen)
      iBytesRead = pFile->iSize - pFile->iPos - 1; // <-- ugly work-around
    if (iBytesRead <= 0)
      return 0;
    iBytesRead = (int32_t)f->read(pBuf, iBytesRead);
    pFile->iPos = f->position();
    return iBytesRead;
  }

  int32_t GIFSeekFile(GIFFILE *pFile, int32_t iPosition) {
    int i = micros();
    File *f = static_cast<File *>(pFile->fHandle);
    f->seek(iPosition);
    pFile->iPos = (int32_t)f->position();
    i = micros() - i;
    return pFile->iPos;
  }


  std::atomic<uint32_t> loops_done(0);

  void gifTask(void *parameter) {
    int i, t;
    uint8_t next_frame_ready = 0;
    uint8_t more_frame = 0;
    for (;;) {
      vTaskDelay(1 / portTICK_PERIOD_MS);
      // Mark busy *before* checking for a stop, so stop() can't miss us.
      active = true;
      if (stop_requested) {
        if (gif) {  // release the arena
          gif->close();
          gif = nullptr;
        }
        active = false;
        continue;
      }
      if (next_gif_file.load() != nullptr) {
        char* fp = next_gif_file.exchange(nullptr, std::memory_order_acq_rel);
        if (fp != nullptr) {
          if (gif)
            gif->close();
          if (current_gif_file) { // close old gif file
            current_gif_file.close();
          }
          // the arena may have been used by PNG / Lua since: build a fresh decoder
          gif = new (Arena::data()) AnimatedGIF();
          gif->begin(GIF_PALETTE_RGB888);
          next_frame_ready = 0;
          loops_done = 0;
          // fp will be freed by GIFOpenFile
          if(gif->open(fp, GIFOpenFile, GIFCloseFile, GIFReadFile, GIFSeekFile, GIFDraw)) {
            next_frame_millis = 0;
            // clear both buffers
            virtualDisp->clearScreen();
            flip_matrix();
            virtualDisp->clearScreen();
          } else {
            // error
            gif = nullptr;
            stop_requested = true;
            continue;
          }
        }
      }
      if (!gif)
        continue;
  
      if (!next_frame_ready) {
        more_frame = gif->playFrame(false, &i);
        next_frame_ready = 1;
      }

      t = millis();
      if (t >= next_frame_millis) {
        next_frame_millis = millis() + i;
        if (!more_frame) {
          loops_done++;
          gif->reset();
          flip_matrix();
          virtualDisp->clearScreen();
        } else {
          flip_matrix();
          if (disposalMethod == 2) {
            virtualDisp->clearScreen();
          } else {
            virtualDisp->copyDMABuffer();
          }
        }
        next_frame_ready = 0;
        HHH += 1;
      }
          // Log.printf("New frame\n");

      // copy front buffer into back buffer
      // virtualDisp->copyDMABuffer();
    }
  }

}

namespace SpectreGif {

  void play(const char* filepath) {
    char* fp = strdup(filepath);
    char* old_fp = next_gif_file.exchange(fp, std::memory_order_acq_rel);
    if (old_fp != NULL) {
      free(old_fp);
    }
    loops_done = 0;  // not the previous GIF's
    stop_requested = false;
  }

  uint8_t init() {
    return xTaskCreate(
        gifTask,   /* Task function. */
        "GifTask", /* String with name of task. */
        6144,            /* Stack size in bytes: ~2 KB used while playing (HEAP_TRACE) */
        NULL,            /* Parameter passed as input of the task */
        5,               /* Priority of the task. */
        &task);           /* Task handle. */
  }

  bool stop() {
    stop_requested = true;
    for (int i = 0; i < 500 && active; i++)
      vTaskDelay(1 / portTICK_PERIOD_MS);
    return !active;
  }

  bool playing() {
    return !stop_requested && (gif != nullptr || next_gif_file.load() != nullptr);
  }

  uint32_t loops() {
    return loops_done;
  }

  bool isPlaying(const char* fp) {
    // path() is NULL when no GIF is open (e.g. a Lua script is playing)
    return gif && current_gif_file && strcmp(current_gif_file.path(), fp) == 0;
  }

}