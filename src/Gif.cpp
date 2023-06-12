#include <AnimatedGIF.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <atomic>

#ifdef USE_SD
#include "FS.h"
#include "SD.h"
#include "SPI.h"
#define filesystem SD
#endif
#ifdef USE_SD_MMC
#include "FS.h"
#include "SD_MMC.h"
#define filesystem SD_MMC
#endif
#ifdef USE_SPIFFS
#include "SPIFFS.h"
#define filesystem SPIFFS
#endif

extern MatrixPanel_I2S_DMA *display;
extern void flip_matrix();

namespace {
  static AnimatedGIF gif;
  static TaskHandle_t task = NULL;
  static File current_gif_file;
  static uint32_t next_frame_millis = 0;
  static int spectre_gif_plz_stop = 1;
  static std::atomic<char*> next_gif_file(nullptr);

  // Draw a line of image directly on the LED Matrix
  void GIFDraw(GIFDRAW *pDraw) {
    uint8_t *s;
    uint16_t *d, *usPalette, usTemp[320];
    int y, iWidth;

    iWidth = pDraw->iWidth;
    if (iWidth > MATRIX_WIDTH)
      iWidth = MATRIX_WIDTH;

    usPalette = pDraw->pPalette;
    y = pDraw->iY + pDraw->y; // current line

    s = pDraw->pPixels;
    if (pDraw->ucDisposalMethod == 2) { // restore to background color
      for (int x = 0; x < iWidth; x++) {
        if (s[x] == pDraw->ucTransparent)
          s[x] = pDraw->ucBackground;
      }
      pDraw->ucHasTransparency = 0;
    }
    // Apply the new pixels to the main image
    if (pDraw->ucHasTransparency) { // if transparency used
      uint8_t *pEnd, c, ucTransparent = pDraw->ucTransparent;
      int x, iCount;
      pEnd = s + pDraw->iWidth;
      x = 0;
      iCount = 0; // count non-transparent pixels
      while (x < pDraw->iWidth) {
        c = ucTransparent - 1;
        d = usTemp;
        while (c != ucTransparent && s < pEnd) {
          c = *s++;
          if (c == ucTransparent) { // done, stop
            s--; // back up to treat it like transparent
          } else { // opaque
            *d++ = usPalette[c];
            iCount++;
          }
        }             // while looking for opaque pixels
        if (iCount) { // any opaque pixels?
          for (int xOffset = 0; xOffset < iCount; xOffset++) {
            display->drawPixel(x + xOffset, y, usTemp[xOffset]); // 565 Color Format
          }
          x += iCount;
          iCount = 0;
        }
        // no, look for a run of transparent pixels
        c = ucTransparent;
        while (c == ucTransparent && s < pEnd) {
          c = *s++;
          if (c == ucTransparent)
            iCount++;
          else
            s--;
        }
        if (iCount) {
          x += iCount; // skip these
          iCount = 0;
        }
      }
    }
    else { // does not have transparency
      s = pDraw->pPixels;
      // Translate the 8-bit pixels through the RGB565 palette (already byte reversed)
      for (int x = 0; x < pDraw->iWidth; x++) {
        display->drawPixel(x, y, usPalette[*s++]); // color 565
      }
    }
  }

  void *GIFOpenFile(const char *fname, int32_t *pSize) {
    current_gif_file = filesystem.open(fname);
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

  void gifTask(void *parameter) {
    int t, i;
    for (;;) {
      vTaskDelay(1 / portTICK_PERIOD_MS);
      if (spectre_gif_plz_stop) continue;
      if (next_gif_file.load() != nullptr) {
        char* fp = next_gif_file.exchange(nullptr, std::memory_order_acq_rel);
        if (fp != nullptr) {
          if (current_gif_file) { // close old gif file
            current_gif_file.close();
          }
          // fp will be freed by GIFOpenFile
          if(gif.open(fp, GIFOpenFile, GIFCloseFile, GIFReadFile, GIFSeekFile, GIFDraw)) {
            spectre_gif_plz_stop = 0;
            next_frame_millis = 0;
          } else {
            // error
            spectre_gif_plz_stop = 1;
            continue;
          }
        }
      }
      // next frame
      t = millis();
      if (t < next_frame_millis) continue;
      if (gif.playFrame(false, &i)) {
        next_frame_millis = t + i;
        flip_matrix();
      } else {
        gif.reset();
      }
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
    spectre_gif_plz_stop = 0;
  }

  uint8_t init() {
    gif.begin(LITTLE_ENDIAN_PIXELS);
    return xTaskCreate(
        gifTask,   /* Task function. */
        "GifTask", /* String with name of task. */
        8192 * 2,        /* Stack size in bytes. */
        NULL,            /* Parameter passed as input of the task */
        5,               /* Priority of the task. */
        &task);           /* Task handle. */
  }

  void stop() {
    spectre_gif_plz_stop = 1;
  }

}