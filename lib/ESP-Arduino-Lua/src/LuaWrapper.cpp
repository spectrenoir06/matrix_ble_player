#include "LuaWrapper.h"

Print *LuaWrapper::out = &Serial;

extern "C" {
  static int lua_wrapper_print (lua_State *L) {
    int n = lua_gettop(L);  /* number of arguments */
    int i;
    lua_getglobal(L, "tostring");
    for (i=1; i<=n; i++) {
      const char *s;
      size_t l;
      lua_pushvalue(L, -1);  /* function to be called */
      lua_pushvalue(L, i);   /* value to print */
      lua_call(L, 1, 1);
      s = lua_tolstring(L, -1, &l);  /* get result */
      if (s == NULL)
        return luaL_error(L, "'tostring' must return a string to 'print'");
      if (i>1) LuaWrapper::out->write("\t");
      LuaWrapper::out->write(s);
      lua_pop(L, 1);  /* pop result */
    }
    LuaWrapper::out->println();
    return 0;
  }
} 

LuaWrapper::LuaWrapper() {
  _state = luaL_newstate();
  openLibs();
}

LuaWrapper::LuaWrapper(lua_Alloc alloc, void *ud) {
  _state = lua_newstate(alloc, ud);
  openLibs();
}

void LuaWrapper::openLibs() {
  luaopen_base(_state);
  luaopen_table(_state);
  luaopen_string(_state);
  luaopen_math(_state);
  lua_register(_state, "print", lua_wrapper_print);
  // Arduino constants as globals (not prepended to the script: error line
  // numbers stay right)
  const struct { const char *name; int value; } constants[] = {
    {"INPUT", INPUT}, {"OUTPUT", OUTPUT}, {"LOW", LOW}, {"HIGH", HIGH},
  };
  for (const auto &c : constants) {
    lua_pushinteger(_state, c.value);
    lua_setglobal(_state, c.name);
  }
}

LuaWrapper::~LuaWrapper() {
  lua_close(_state);
}


String LuaWrapper::Lua_dostring(const String *script, const char *name) {
  String result;
  String chunk = String("=") + name;  // "=": the name as is in messages
  if (luaL_loadbuffer(_state, script->c_str(), script->length(), chunk.c_str()) ||
      lua_pcall(_state, 0, LUA_MULTRET, 0)) {
    result = "lua error: " + String(lua_tostring(_state, -1));
    lua_pop(_state, 1);
  }
  return result;
}

void LuaWrapper::Lua_register(const String name, const lua_CFunction function) {
  lua_register(_state, name.c_str(), function);
}
