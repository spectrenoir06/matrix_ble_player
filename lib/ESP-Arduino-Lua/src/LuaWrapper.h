#ifndef LUA_WRAPPER_H
#define LUA_WRAPPER_H

#include "Arduino.h"

#define LUA_USE_C89
#include "lua/lua.hpp"

class LuaWrapper {
  public:
    // Where print() and the wrapper's messages go (default: Serial).
    static Print *out;
    LuaWrapper();
    // Lua state whose memory comes from `alloc` (see lua_newstate)
    LuaWrapper(lua_Alloc alloc, void *ud);
    ~LuaWrapper();
    // Run a script; `name` shows in errors ("name:3: boom"). Returns the
    // error ("lua error: …"), empty if none.
    String Lua_dostring(const String *script, const char *name = "script");
    void Lua_register(const String name, const lua_CFunction function);

  private:
    void openLibs();
    lua_State *_state;
};

#endif
