#include <LuaWrapper.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <atomic>
#include <multi_heap.h>
#include <Mapping.h>

extern MatrixPanel_I2S_DMA *display;
#include "Audio.hpp"
#include "Clock.hpp"
#include "Layout.hpp"
#include "Framebuffer.hpp"
#include "Arena.hpp"
#include "Protocol.hpp"
#include "Log.hpp"
extern void flip_matrix();
extern VirtualMatrixPanel *virtualDisp;

extern int spectre_lua_plz_stop;

namespace {

  // The Lua task exists only while scripts run: created by run_script(), it
  // deletes itself when there is nothing left to run (its 10 KB stack is
  // RAM WiFi needs). The lock: a script queued while it is exiting is not lost.
  static TaskHandle_t runLuaTaskHandle = NULL;
  static portMUX_TYPE lua_task_lock = portMUX_INITIALIZER_UNLOCKED;
  constexpr uint32_t LUA_STACK = 1024 * 10;
  struct Script {
    String source;
    String name;  // for error messages
  };
  std::atomic<Script*> current_lua_script(nullptr);
  std::atomic<bool> lua_running(false);  // the task may be using the arena

  // Lua allocator on a private heap laid over the shared arena
  void *arena_alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
    multi_heap_handle_t heap = static_cast<multi_heap_handle_t>(ud);
    if (nsize == 0) {
      multi_heap_free(heap, ptr);
      return nullptr;
    }
    return ptr ? multi_heap_realloc(heap, ptr, nsize) : multi_heap_malloc(heap, nsize);
  }

  static int lua_wrapper_updateDisplay(lua_State *lua_state) {
    flip_matrix();
    return 0;
  }

  static int lua_wrapper_drawPixel(lua_State *lua_state) {
    int x = luaL_checkinteger(lua_state, 1);
    int y = luaL_checkinteger(lua_state, 2);
    int r = luaL_checkinteger(lua_state, 3);
    int g = luaL_checkinteger(lua_state, 4);
    int b = luaL_checkinteger(lua_state, 5);
    virtualDisp->drawPixelRGB888(x, y, r, g, b);
    return 0;
  }

  static int lua_wrapper_delay(lua_State *lua_state) {
    int a = luaL_checkinteger(lua_state, 1);
    delay(a);
    return 0;
  }

  static int lua_wrapper_millis(lua_State *lua_state) {
    // an integer: this Lua's numbers are single floats, exact only to 2^24 ms
    // (4.6 h of uptime); 32-bit integers are exact for 24 days, then wrap
    // (differences stay right)
    lua_pushinteger(lua_state, (lua_Integer) millis());
    return 1;
  }

  // getTime(): hour, min, sec, day, month (1-12), year, weekday (1 = Sunday,
  // like os.date); nil while the time is unknown (no client nor NTP gave it)
  static int lua_wrapper_getTime(lua_State *lua_state) {
    struct tm t;
    if (!clock_local(t)) {
      lua_pushnil(lua_state);
      return 1;
    }
    lua_pushinteger(lua_state, t.tm_hour);
    lua_pushinteger(lua_state, t.tm_min);
    lua_pushinteger(lua_state, t.tm_sec);
    lua_pushinteger(lua_state, t.tm_mday);
    lua_pushinteger(lua_state, t.tm_mon + 1);
    lua_pushinteger(lua_state, t.tm_year + 1900);
    lua_pushinteger(lua_state, t.tm_wday + 1);
    return 7;
  }

  static int lua_wrapper_printBLE(lua_State *lua_state) {
    // Lua print to the connected client (sys.log event)
    const char *lstr = luaL_checkstring(lua_state, 1);
    protocol_log(LogLevel::Info, lstr);
    return 0;
  }

  static int lua_wrapper_clearDisplay(lua_State *lua_state) {
    virtualDisp->clearScreen();
    return 0;
  }

  static int lua_wrapper_fillDisplay(lua_State *lua_state) {
    int r = luaL_checkinteger(lua_state, 1);
    int g = luaL_checkinteger(lua_state, 2);
    int b = luaL_checkinteger(lua_state, 3);
    virtualDisp->fillScreenRGB888(r, g, b);
    return 0;
  }

  static int lua_wrapper_setTextColor(lua_State *lua_state) {
    int r = luaL_checkinteger(lua_state, 1);
    int g = luaL_checkinteger(lua_state, 2);
    int b = luaL_checkinteger(lua_state, 3);
    virtualDisp->setTextColor(virtualDisp->color565(r,g,b));
    return 0;
  }

  static int lua_wrapper_printText(lua_State *lua_state) {
    size_t len = 0;
    const char *str = luaL_checklstring(lua_state, 1, &len);
    virtualDisp->print(str);
    return 0;
  }

  static int lua_wrapper_setCursor(lua_State *lua_state) {
    int x = luaL_checkinteger(lua_state, 1);
    int y = luaL_checkinteger(lua_state, 2);
    virtualDisp->setCursor(x,y);
    return 0;
  }

  static int lua_wrapper_setTextSize(lua_State *lua_state) {
    int size = luaL_checkinteger(lua_state, 1);
    virtualDisp->setTextSize(size);
    return 0;
  }

  static int lua_wrapper_fillRect(lua_State *lua_state) {
    int x = luaL_checkinteger(lua_state, 1);
    int y = luaL_checkinteger(lua_state, 2);

    int w = luaL_checkinteger(lua_state, 3);
    int h = luaL_checkinteger(lua_state, 4);

    int r = luaL_checkinteger(lua_state, 5);
    int g = luaL_checkinteger(lua_state, 6);
    int b = luaL_checkinteger(lua_state, 7);
    virtualDisp->fillRect(x, y, w, h, r, g, b);
    return 0;
  }

  static int lua_wrapper_colorWheel(lua_State *lua_state) {
    uint8_t pos = luaL_checkinteger(lua_state, 1);
    uint8_t r,g,b;
    if(pos < 85) {
      r = pos * 3;
      g = 255 - pos * 3;
      b = 0;
    } else if(pos < 170) {
      pos -= 85;
      r = 255 - pos * 3;
      g = 0;
      b = pos * 3;
    } else {
      pos -= 170;
      r = 0;
      g = pos * 3;
      b = 255 - pos * 3;
    }
    lua_pushinteger(lua_state, (lua_Integer)r);
    lua_pushinteger(lua_state, (lua_Integer)g);
    lua_pushinteger(lua_state, (lua_Integer)b);
    return 3;
  }


  static int lua_wrapper_setTextWrap(lua_State *lua_state) {
    if (lua_isboolean(lua_state, 1))
      virtualDisp->setTextWrap(lua_toboolean(lua_state, 1));
    return 0;
  }

  // Sound (Audio.hpp), like Shadertoy's: getFFT() a table of Audio::BINS
  // levels 0..255 (low to high frequencies), getFFT(i) one of them (1-based);
  // getWave() / getWave(i) the waveform, 128 = silence.
  static int push_audio(lua_State *lua_state, void (*read)(uint8_t *)) {
    uint8_t v[Audio::BINS];
    read(v);
    if (lua_gettop(lua_state) >= 1) {
      lua_Integer i = luaL_checkinteger(lua_state, 1);
      lua_pushinteger(lua_state, i >= 1 && i <= Audio::BINS ? v[i - 1] : 0);
      return 1;
    }
    lua_createtable(lua_state, Audio::BINS, 0);
    for (int i = 0; i < Audio::BINS; i++) {
      lua_pushinteger(lua_state, v[i]);
      lua_rawseti(lua_state, -2, i + 1);
    }
    return 1;
  }

  static int lua_wrapper_getFFT(lua_State *lua_state) {
    return push_audio(lua_state, Audio::fft);
  }

  static int lua_wrapper_getWave(lua_State *lua_state) {
    return push_audio(lua_state, Audio::wave);
  }

  // scroll(dx, dy [, wrap]): shift what is on screen by (dx, dy) pixels
  // (right / down positive) into the frame being drawn; draw the new edge,
  // then updateDisplay(). Without wrap, black scrolls in.
  static int lua_wrapper_scroll(lua_State *lua_state) {
    int dx = luaL_checkinteger(lua_state, 1);
    int dy = luaL_optinteger(lua_state, 2, 0);
    bool wrap = lua_toboolean(lua_state, 3);
    scroll_display(dx, dy, wrap);
    return 0;
  }

  // getPixel(x, y): r, g, b of the frame being drawn (as near as the color
  // depth keeps them); 0, 0, 0 outside the picture
  static int lua_wrapper_getPixel(lua_State *lua_state) {
    uint8_t r = 0, g = 0, b = 0;
    get_pixel(luaL_checkinteger(lua_state, 1), luaL_checkinteger(lua_state, 2), r, g, b);
    lua_pushinteger(lua_state, r);
    lua_pushinteger(lua_state, g);
    lua_pushinteger(lua_state, b);
    return 3;
  }

  static int lua_wrapper_getMatrix(lua_State *lua_state) {
    lua_pushinteger(lua_state, (lua_Integer)matrix_w);
    lua_pushinteger(lua_state, (lua_Integer)matrix_h);
    return 2;
  }

  void lua_exec() {

    display->clearScreen();
    display->flipDMABuffer();
    display->clearScreen();
    display->flipDMABuffer();

    Script* str = current_lua_script.exchange(nullptr, std::memory_order_acq_rel);
    if (str == nullptr)  // stopped before it started
      return;

    // A fresh heap over the arena for each script: lua_close frees it all
    multi_heap_handle_t heap = multi_heap_register(Arena::data(), Arena::SIZE);
    if (!heap) {
      delete str;
      Log.line(LogLevel::Error, "lua: no memory");
      return;
    }
    LuaWrapper lua(arena_alloc, heap);
    lua.Lua_register("clearDisplay",   (const lua_CFunction) &lua_wrapper_clearDisplay);
    lua.Lua_register("fillDisplay",    (const lua_CFunction) &lua_wrapper_fillDisplay);
    lua.Lua_register("updateDisplay",  (const lua_CFunction) &lua_wrapper_updateDisplay);
    lua.Lua_register("drawPixel",      (const lua_CFunction) &lua_wrapper_drawPixel);
    lua.Lua_register("fillRect",       (const lua_CFunction) &lua_wrapper_fillRect);
    lua.Lua_register("colorWheel",     (const lua_CFunction) &lua_wrapper_colorWheel);

    lua.Lua_register("delay",          (const lua_CFunction) &lua_wrapper_delay);
    lua.Lua_register("millis",         (const lua_CFunction) &lua_wrapper_millis);

    lua.Lua_register("setTextColor",   (const lua_CFunction) &lua_wrapper_setTextColor);
    lua.Lua_register("setTextWrap",    (const lua_CFunction) &lua_wrapper_setTextWrap);
    lua.Lua_register("printText",      (const lua_CFunction) &lua_wrapper_printText);
    lua.Lua_register("setCursor",      (const lua_CFunction) &lua_wrapper_setCursor);
    lua.Lua_register("setTextSize",    (const lua_CFunction) &lua_wrapper_setTextSize);
    
    lua.Lua_register("printBLE",       (const lua_CFunction) &lua_wrapper_printBLE);
    lua.Lua_register("getMatrix",      (const lua_CFunction) &lua_wrapper_getMatrix);
    lua.Lua_register("getFFT",         (const lua_CFunction) &lua_wrapper_getFFT);
    lua.Lua_register("getWave",        (const lua_CFunction) &lua_wrapper_getWave);
    lua.Lua_register("scroll",         (const lua_CFunction) &lua_wrapper_scroll);
    lua.Lua_register("getPixel",       (const lua_CFunction) &lua_wrapper_getPixel);
    lua.Lua_register("getTime",        (const lua_CFunction) &lua_wrapper_getTime);
    
    spectre_lua_plz_stop = 0;
    String ret = lua.Lua_dostring(&str->source, str->name.c_str());
    delete str;
    if (ret.indexOf("lua plz stop") > -1) {
      // this is not a real error but just a termination request.
      // ignore.
      return;
    }
    if (ret.length() > 0) {
      Log.line(LogLevel::Error, ret.c_str());
    }
  }

  void runLuaTask(void* parameter) {
    for(;;) {
      // Mark busy *before* looking for a script, so stop() can't miss us.
      lua_running = true;
      if (current_lua_script.load() != nullptr) {
        lua_exec();
      }
      lua_running = false;
      bool done;
      portENTER_CRITICAL(&lua_task_lock);
      done = current_lua_script.load() == nullptr;
      if (done)
        runLuaTaskHandle = NULL;
      portEXIT_CRITICAL(&lua_task_lock);
      if (done)
        vTaskDelete(NULL);  // frees the stack
      vTaskDelay(1 / portTICK_PERIOD_MS);
    };
  }

}

namespace Lua {

  BaseType_t init() {
    LuaWrapper::out = &Log;  // Lua print() and the wrapper's messages: serial text + sys.log
    return pdPASS;  // the task is created by run_script()
  }

  bool stop() {
    Script* pending = current_lua_script.exchange(nullptr, std::memory_order_acq_rel);
    delete pending;
    spectre_lua_plz_stop = 1;
    for (int i = 0; i < 2000 && lua_running; i++)
      vTaskDelay(1 / portTICK_PERIOD_MS);
    return !lua_running;
  }

  bool running() {
    return lua_running || current_lua_script.load() != nullptr;
  }

  void run_script(String script, String name) {
    // stop current script
    spectre_lua_plz_stop = 1;
    // copy script string
    Script* str = new Script{script, name};
    str = current_lua_script.exchange(str, std::memory_order_acq_rel);
    if (str != nullptr) {
      delete str;
    }
    // no task: start one (a running task picks the script up)
    portENTER_CRITICAL(&lua_task_lock);
    bool create = runLuaTaskHandle == NULL;
    if (create)
      runLuaTaskHandle = reinterpret_cast<TaskHandle_t>(1);  // being created
    portEXIT_CRITICAL(&lua_task_lock);
    if (create && xTaskCreatePinnedToCore(runLuaTask, "LuaTask", LUA_STACK, NULL, 1, &runLuaTaskHandle, 1) != pdPASS) {
      runLuaTaskHandle = NULL;
      delete current_lua_script.exchange(nullptr, std::memory_order_acq_rel);
      Log.line(LogLevel::Error, "lua: not enough memory to start");
    }
  }

}
