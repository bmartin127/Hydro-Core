#pragma once

struct lua_State;
struct luaL_Reg;

namespace Hydro::API {

/// Push a new Lua table on the stack populated from a null-terminated
/// luaL_Reg array. Used as the body of every Hydro.* module register
/// function so the newtable/setfield loop lives in one place.
///
/// Usage:
///
///     static const luaL_Reg my_functions[] = {
///         {"foo", l_my_foo},
///         {"bar", l_my_bar},
///         {nullptr, nullptr}
///     };
///
///     void registerMyModule(lua_State* L) {
///         buildModuleTable(L, my_functions);
///     }
void buildModuleTable(lua_State* L, const luaL_Reg* functions);

} // namespace Hydro::API
