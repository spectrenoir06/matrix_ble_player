#include "ESP32-HUB75-MatrixPanel-I2S-DMA.h"

struct VirtualCoords
{
    int16_t x;
    int16_t y;
    int16_t virt_row; // chain of panels row
    int16_t virt_col; // chain of panels col

    VirtualCoords() : x(0), y(0) {}
};

#ifdef USE_GFX_ROOT
class VirtualMatrixPanel : public GFX
#elif !defined NO_GFX
class VirtualMatrixPanel : public Adafruit_GFX
#else
class VirtualMatrixPanel
#endif
{

public:
    VirtualMatrixPanel(MatrixPanel_I2S_DMA &disp, int _vmodule_rows, int _vmodule_cols, int _panelResX, int _panelResY, int16_t*_map)
#ifdef USE_GFX_ROOT
        : GFX(_vmodule_cols * _panelResX, _vmodule_rows * _panelResY)
#elif !defined NO_GFX
        : Adafruit_GFX(_vmodule_cols * _panelResX, _vmodule_rows * _panelResY)
#endif
    {
        this->display = &disp;

        for (uint16_t x=0; x < _vmodule_cols; x++)
            for (uint16_t y=0; y < _vmodule_rows; y++)
                map[x][y] = _map[x+y*_vmodule_cols];

        panelResX = _panelResX;
        panelResY = _panelResY;

        vmodule_rows = _vmodule_rows;
        vmodule_cols = _vmodule_cols;

        virtualResX = vmodule_cols * _panelResX;
        virtualResY = vmodule_rows * _panelResY;

        if (_vmodule_rows > 1 || vmodule_cols > 1)
            need_map = 1;
        else
            need_map = 0;

        coords.x = coords.y = -1; // By default use an invalid co-ordinates that will be rejected by updateMatrixDMABuffer
    }

    // equivalent methods of the matrix library so it can be just swapped out.
    void drawPixel(int16_t x, int16_t y, uint16_t color);   // overwrite adafruit implementation
    void fillScreen(uint16_t color); 			// overwrite adafruit implementation
    void setRotation(int rotate); 				// overwrite adafruit implementation
    
	void fillScreenRGB888(uint8_t r, uint8_t g, uint8_t b);
    void clearScreen() { display->clearScreen(); }
    void drawPixelRGB888(int16_t x, int16_t y, uint8_t r, uint8_t g, uint8_t b);

#ifdef USE_GFX_ROOT
    // 24bpp FASTLED CRGB colour struct support
    void fillScreen(CRGB color);
    void drawPixel(int16_t x, int16_t y, CRGB color);
    void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint8_t r, uint8_t g, uint8_t b);

    void color565to888(const uint16_t color, uint8_t &r, uint8_t &g, uint8_t &b);
    void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);


#endif

    uint16_t color444(uint8_t r, uint8_t g, uint8_t b) { return display->color444(r, g, b); }
    uint16_t color565(uint8_t r, uint8_t g, uint8_t b) { return display->color565(r, g, b); }
    uint16_t color333(uint8_t r, uint8_t g, uint8_t b) { return display->color333(r, g, b); }

    void flipDMABuffer() { display->flipDMABuffer(); }

private:
    MatrixPanel_I2S_DMA *display;

    virtual VirtualCoords getCoords(int16_t x, int16_t y);
    VirtualCoords coords;

    int16_t virtualResX;
    int16_t virtualResY;

    int16_t vmodule_rows;
    int16_t vmodule_cols;

    int16_t panelResX;
    int16_t panelResY;

    int16_t map[16][16];

    uint8_t need_map;

};

/**
 * Calculate virtual->real co-ordinate mapping to underlying single chain of panels connected to ESP32.
 * Updates the private class member variable 'coords', so no need to use the return value.
 * Not thread safe, but not a concern for ESP32 sketch anyway... I think.
 */
inline VirtualCoords VirtualMatrixPanel::getCoords(int16_t virt_x, int16_t virt_y)
{
    if (virt_x < 0 || virt_x >= virtualResX || virt_y < 0 || virt_y >= virtualResX) // _width and _height are defined in the adafruit constructor
    {                             // Co-ordinates go from 0 to X-1 remember! otherwise they are out of range!
        coords.x = coords.y = -1; // By defalt use an invalid co-ordinates that will be rejected by updateMatrixDMABuffer
        return coords;
    }

    if (!need_map) {
        coords.x = virt_x;
        coords.y = virt_y;
        return coords;
    }

    uint16_t px =  virt_x / panelResX;
    uint16_t py =  virt_y / panelResY;

    int16_t panel = map[px][py];
    // Serial.printf("Panel[%d][%d] = %d\n", px, py, panel);

    if (panel != -1) {
        coords.x = (panel * panelResX) + (virt_x%panelResX);
        coords.y = virt_y%panelResX;
    } else {
        coords.x = coords.y = -1; // By defalt use an invalid co-ordinates that will be rejected by updateMatrixDMABuffer
    }
    return coords;
}

inline void VirtualMatrixPanel::drawPixel(int16_t x, int16_t y, uint16_t color)
{ // adafruit virtual void override
    this->getCoords(x, y);
    // Serial.printf("Requested virtual x,y coord (%d, %d), got phyical chain coord of (%d,%d)\n", x,y, coords.x, coords.y);
    this->display->drawPixel(coords.x, coords.y, color);	
}

inline void VirtualMatrixPanel::fillScreen(uint16_t color)
{ // adafruit virtual void override
    this->display->fillScreen(color);
}

inline void VirtualMatrixPanel::fillScreenRGB888(uint8_t r, uint8_t g, uint8_t b)
{
    this->display->fillScreenRGB888(r, g, b);
}

inline void VirtualMatrixPanel::drawPixelRGB888(int16_t x, int16_t y, uint8_t r, uint8_t g, uint8_t b)
{
    this->getCoords(x, y);
    // Serial.printf("Requested virtual x,y coord (%d, %d), got phyical chain coord of (%d,%d)\n", x,y, coords.x, coords.y);
    this->display->drawPixelRGB888(coords.x, coords.y, r, g, b);
}

#ifdef USE_GFX_ROOT
// Support for CRGB values provided via FastLED
inline void VirtualMatrixPanel::drawPixel(int16_t x, int16_t y, CRGB color)
{
    this->getCoords(x, y);
    this->display->drawPixel(coords.x, coords.y, color);
}

inline void VirtualMatrixPanel::fillScreen(CRGB color)
{
    this->display->fillScreen(color);
}

inline void VirtualMatrixPanel::fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint8_t r, uint8_t g, uint8_t b)
{
    for (int px = x; px < (x+w); px++) {
        for (int py = y; py < (y+h); py++) {
            this->getCoords(px, py);
            this->display->drawPixelRGB888(coords.x, coords.y, r, g, b);
        }
    }
}

inline void VirtualMatrixPanel::color565to888(const uint16_t color, uint8_t &r, uint8_t &g, uint8_t &b)
{
  r = ((((color >> 11) & 0x1F) * 527) + 23) >> 6;
  g = ((((color >> 5) & 0x3F) * 259) + 33) >> 6;
  b = (((color & 0x1F) * 527) + 23) >> 6;
}

inline void VirtualMatrixPanel::fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
    uint8_t r, g, b;
    color565to888(color, r, g, b);
    fillRect(x,y,w,h,r,g,b);
}


#endif




