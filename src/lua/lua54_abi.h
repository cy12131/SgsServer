#pragma once

// 构建机仅安装了 Lua 5.4 运行库。这里声明 Runtime 实际使用的稳定 C ABI
// 子集，并由 CMake 严格链接 Lua 5.4；不复制 Lua 实现，也不暴露给公共 API。
extern "C" {
struct lua_State;
using lua_Integer = long long;
using lua_Number = double;
using lua_KContext = long long;
using lua_CFunction = int (*)(lua_State*);
using lua_KFunction = int (*)(lua_State*, int, lua_KContext);

lua_State* luaL_newstate();
void lua_close(lua_State* state);
void luaL_requiref(lua_State* state, const char* name, lua_CFunction open_function,
                   int global);
int luaopen_base(lua_State* state);
int luaL_loadfilex(lua_State* state, const char* filename, const char* mode);
int lua_pcallk(lua_State* state, int arguments, int results, int error_function,
               lua_KContext context, lua_KFunction continuation);
void lua_settop(lua_State* state, int index);
int lua_type(lua_State* state, int index);
const char* lua_tolstring(lua_State* state, int index, unsigned long* length);
lua_Integer lua_tointegerx(lua_State* state, int index, int* is_number);
void* lua_touserdata(lua_State* state, int index);
void lua_pushnil(lua_State* state);
const char* lua_pushstring(lua_State* state, const char* value);
const char* lua_pushlstring(lua_State* state, const char* value, unsigned long length);
void lua_pushinteger(lua_State* state, lua_Integer value);
void lua_pushnumber(lua_State* state, lua_Number value);
void lua_pushboolean(lua_State* state, int value);
void lua_pushlightuserdata(lua_State* state, void* value);
void lua_pushcclosure(lua_State* state, lua_CFunction function, int upvalues);
void lua_pushvalue(lua_State* state, int index);
void lua_createtable(lua_State* state, int array_size, int record_size);
int lua_getfield(lua_State* state, int index, const char* key);
void lua_setfield(lua_State* state, int index, const char* key);
int lua_getglobal(lua_State* state, const char* name);
void lua_setglobal(lua_State* state, const char* name);
int luaL_ref(lua_State* state, int table_index);
void lua_rawgeti(lua_State* state, int table_index, lua_Integer reference);
void lua_rawseti(lua_State* state, int table_index, lua_Integer index);
void luaL_unref(lua_State* state, int table_index, int reference);
int lua_error(lua_State* state);
}

inline constexpr int lua_ok = 0;
inline constexpr int lua_type_nil = 0;
inline constexpr int lua_type_number = 3;
inline constexpr int lua_type_string = 4;
inline constexpr int lua_type_table = 5;
inline constexpr int lua_type_function = 6;
inline constexpr int lua_registry_index = -1001000;

inline void lua_pop(lua_State* state, int count) {
    lua_settop(state, -count - 1);
}
