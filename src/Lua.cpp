#include <LuaWrapper.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <atomic>
#include <multi_heap.h>
#include <Mapping.h>

extern MatrixPanel_I2S_DMA *display;
#include "Arena.hpp"
#include "Protocol.hpp"
extern void flip_matrix();
extern VirtualMatrixPanel *virtualDisp;

extern int spectre_lua_plz_stop;

namespace {

  static TaskHandle_t runLuaTaskHandle = NULL;
  std::atomic<String*> current_lua_script(nullptr);
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
    lua_pushnumber(lua_state, (lua_Number) millis());
    return 1;
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

  static int lua_wrapper_getMatrix(lua_State *lua_state) {
    lua_pushinteger(lua_state, (lua_Integer)V_MATRIX_WIDTH);
    lua_pushinteger(lua_state, (lua_Integer)V_MATRIX_HEIGHT);
    return 2;
  }

  void lua_exec() {

    display->clearScreen();
    display->flipDMABuffer();
    display->clearScreen();
    display->flipDMABuffer();

    String* str = current_lua_script.exchange(nullptr, std::memory_order_acq_rel);
    if (str == nullptr)  // stopped before it started
      return;

    // A fresh heap over the arena for each script: lua_close frees it all
    multi_heap_handle_t heap = multi_heap_register(Arena::data(), Arena::SIZE);
    if (!heap) {
      delete str;
      protocol_log(LogLevel::Error, "lua: no memory");
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
    
    Serial.println("Start task runLuaTask");
    spectre_lua_plz_stop = 0;
    String ret = lua.Lua_dostring(str);
    delete str;
    if (ret.indexOf("lua plz stop") > -1) {
      // this is not a real error but just a termination request.
      // ignore.
      return;
    }
    if (ret.length() > 0) {
      Serial.println(ret);
      protocol_log(LogLevel::Error, ret.c_str());
    }
  }

  void runLuaTask(void* parameter) {
    // Infinite loop :-)
    for(;;) {
      // Mark busy *before* looking for a script, so stop() can't miss us.
      lua_running = true;
      if (current_lua_script.load() != nullptr) {
        lua_exec();
      }
      lua_running = false;
      vTaskDelay(1 / portTICK_PERIOD_MS);
    };
  }

}

namespace Lua {

  BaseType_t init() {
    BaseType_t ret = xTaskCreatePinnedToCore(
      runLuaTask,   /* Task function. */
      "LuaTask", /* String with name of task. */
      1024 * 10,  /* Stack size in bytes. */
      NULL,	   /* Parameter passed as input of the task */
      1,		   /* Priority of the task. */
      &runLuaTaskHandle,	   /* Task handle. */
      1
    );
    Serial.printf("xTaskCreatePinnedToCore returned %d\n", ret);
    return ret;
  }

  bool stop() {
    String* pending = current_lua_script.exchange(nullptr, std::memory_order_acq_rel);
    delete pending;
    spectre_lua_plz_stop = 1;
    for (int i = 0; i < 2000 && lua_running; i++)
      vTaskDelay(1 / portTICK_PERIOD_MS);
    return !lua_running;
  }

  void run_script(String script) {
    if (runLuaTaskHandle == NULL) {
      // could not create task for lua scripts
      // aborting
      return;
    }
    // stop current script
    spectre_lua_plz_stop = 1;
    // copy script string
    String* str = new String(script.c_str());
    str = current_lua_script.exchange(str, std::memory_order_acq_rel);
    if (str != nullptr) {
      delete str;
    }
  }

}
