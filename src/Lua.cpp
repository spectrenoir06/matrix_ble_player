#include <LuaWrapper.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <NimBLEDevice.h>
#include <atomic>

extern NimBLECharacteristic* pTxCharacteristic;
extern bool deviceConnected;
extern MatrixPanel_I2S_DMA *display;
extern int spectre_lua_plz_stop;
extern void flip_matrix();

namespace {

  static TaskHandle_t runLuaTaskHandle = NULL;
  std::atomic<String*> current_lua_script(nullptr);

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
    display->drawPixelRGB888(x, y, r, g, b);
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
    if (deviceConnected) {
      size_t len = 0;
      const char *str = luaL_checklstring(lua_state, 1, &len);
      pTxCharacteristic->setValue((uint8_t*)str, len);
      pTxCharacteristic->notify();
    }
    return 0;
  }

  static int lua_wrapper_clearDisplay(lua_State *lua_state) {
    display->clearScreen();
    return 0;
  }

  static int lua_wrapper_setTextColor(lua_State *lua_state) {
    int r = luaL_checkinteger(lua_state, 1);
    int g = luaL_checkinteger(lua_state, 2);
    int b = luaL_checkinteger(lua_state, 3);
    display->setTextColor(display->color565(r,g,b));
    return 0;
  }

  static int lua_wrapper_printText(lua_State *lua_state) {
    size_t len = 0;
    const char *str = luaL_checklstring(lua_state, 1, &len);
    display->print(str);
    return 0;
  }

  static int lua_wrapper_setCursor(lua_State *lua_state) {
    int x = luaL_checkinteger(lua_state, 1);
    int y = luaL_checkinteger(lua_state, 2);
    display->setCursor(x,y);
    return 0;
  }

  static int lua_wrapper_setTextSize(lua_State *lua_state) {
    int size = luaL_checkinteger(lua_state, 1);
    display->setTextSize(size);
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
    display->fillRect(x, y, w, h, r, g, b);
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
      display->setTextWrap(lua_toboolean(lua_state, 1));
    return 0;
  }

  void lua_exec() {
    LuaWrapper lua;
    lua.Lua_register("clearDisplay", (const lua_CFunction) &lua_wrapper_clearDisplay);
    lua.Lua_register("updateDisplay", (const lua_CFunction) &lua_wrapper_updateDisplay);
    lua.Lua_register("drawPixel", (const lua_CFunction) &lua_wrapper_drawPixel);
    lua.Lua_register("fillRect", (const lua_CFunction) &lua_wrapper_fillRect);
    lua.Lua_register("colorWheel", (const lua_CFunction) &lua_wrapper_colorWheel);

    lua.Lua_register("delay", (const lua_CFunction) &lua_wrapper_delay);
    lua.Lua_register("millis", (const lua_CFunction) &lua_wrapper_millis);

    lua.Lua_register("setTextColor", (const lua_CFunction) &lua_wrapper_setTextColor);
    lua.Lua_register("setTextWrap", (const lua_CFunction) &lua_wrapper_setTextWrap);
    lua.Lua_register("printText", (const lua_CFunction) &lua_wrapper_printText);
    lua.Lua_register("setCursor", (const lua_CFunction) &lua_wrapper_setCursor);
    lua.Lua_register("setTextSize", (const lua_CFunction) &lua_wrapper_setTextSize);
    
    lua.Lua_register("printBLE", (const lua_CFunction) &lua_wrapper_printBLE);
    
    Serial.println("Start task runLuaTask");
    String* str = current_lua_script.exchange(nullptr, std::memory_order_acq_rel);
    String ret = lua.Lua_dostring(str);
    delete str;
    Serial.println(ret);
    if (deviceConnected) {
      char str[512];
      memset(str, 0, 512);
      strcat(str, "!S");
      strcat(str, ret.c_str());
      pTxCharacteristic->setValue((uint8_t*)str, strlen(str));
      pTxCharacteristic->notify();
    }
  }

  void runLuaTask(void* parameter) {
    // Infinite loop :-)
    for(;;) {
      if (current_lua_script != NULL) {
        lua_exec();
        spectre_lua_plz_stop = 0;
      }
      vTaskDelay(1 / portTICK_PERIOD_MS);
    };
  }

}

namespace Lua {

  /**
   * Crate task for running lua scripts.
   */
  BaseType_t init() {
    BaseType_t ret = xTaskCreatePinnedToCore(
      runLuaTask,   /* Task function. */
      "runLuaTask", /* String with name of task. */
      1024 * 20,  /* Stack size in bytes. */
      NULL,	   /* Parameter passed as input of the task */
      1,		   /* Priority of the task. */
      &runLuaTaskHandle,	   /* Task handle. */
      1
    );
    Serial.printf("xTaskCreatePinnedToCore returned %d\n", ret);
    return ret;
  }

  /**
   * Kindly ask the lua interpreter to stop execution of the current script.
   * It is safe to call this function even if no script is currently running.
   */
  void stop() {
    spectre_lua_plz_stop = 1;
  }

  /**
   * Run the supplied script.
   */
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
