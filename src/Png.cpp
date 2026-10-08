#include <Arduino.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <Mapping.h>
#include <PNGdec.h>
#include <new>

#include <SpectreProtocol.h>

#include "Arena.hpp"
#include "Png.hpp"

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
  constexpr int MAX_WIDTH = 320;  // line buffer size; wider images are refused
  static_assert(sizeof(PNG) <= Arena::SIZE, "PNG decoder does not fit in the arena");
  File png_file;
  int off_x = 0;
  int off_y = 0;

  void *PNGOpenFile(const char *fname, int32_t *pSize) {
    png_file = filesystem.open(fname);
    if (!png_file)
      return nullptr;
    *pSize = png_file.size();
    return &png_file;
  }

  void PNGCloseFile(void *pHandle) {
    static_cast<File *>(pHandle)->close();
  }

  int32_t PNGReadFile(PNGFILE *pFile, uint8_t *pBuf, int32_t iLen) {
    File *f = static_cast<File *>(pFile->fHandle);
    int32_t n = f->read(pBuf, iLen);
    pFile->iPos = f->position();
    return n;
  }

  int32_t PNGSeekFile(PNGFILE *pFile, int32_t iPosition) {
    File *f = static_cast<File *>(pFile->fHandle);
    f->seek(iPosition);
    pFile->iPos = f->position();
    return pFile->iPos;
  }

  int PNGDraw(PNGDRAW *pDraw) {
    PNG *png = static_cast<PNG *>(pDraw->pUser);
    uint16_t line[MAX_WIDTH];
    png->getLineAsRGB565(pDraw, line, PNG_RGB565_LITTLE_ENDIAN, 0x00000000);  // alpha over black
    int y = off_y + pDraw->y;
    for (int x = 0; x < pDraw->iWidth; x++)
      virtualDisp->drawPixel(off_x + x, y, line[x]);
    return 1;
  }
}

namespace SpectrePng {

  uint16_t show(const char* path) {
    // ~45 KB decoder in the shared arena (GIF and Lua are stopped by the caller)
    if (!Arena::data())
      return spectre::error::NoMemory;
    PNG *png = new (Arena::data()) PNG();
    uint16_t err = spectre::error::BadArgs;  // not a PNG we can decode
    if (png->open(path, PNGOpenFile, PNGCloseFile, PNGReadFile, PNGSeekFile, PNGDraw) != PNG_SUCCESS) {
      Serial.printf("PNG: cannot open %s (error %d)\n", path, png->getLastError());
    } else if (png->getWidth() > MAX_WIDTH) {
      Serial.printf("PNG: %s is wider than %d px\n", path, MAX_WIDTH);
      png->close();
    } else {
      off_x = (V_MATRIX_WIDTH  - png->getWidth())  / 2;
      off_y = (V_MATRIX_HEIGHT - png->getHeight()) / 2;
      Serial.printf("PNG %s: %dx%d\n", path, png->getWidth(), png->getHeight());
      virtualDisp->clearScreen();
      if (png->decode(png, 0) == PNG_SUCCESS)
        err = 0;
      png->close();
      flip_matrix();
    }
    png->~PNG();
    return err;
  }

}
