#include "ModuleRegistry.h"

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

namespace Hydro::API {

void buildModuleTable(lua_State* L, const luaL_Reg* functions) {
    lua_newtable(L);
    for (const luaL_Reg* fn = functions; fn->name; fn++) {
        lua_pushcfunction(L, fn->func);
        lua_setfield(L, -2, fn->name);
    }
}

} // namespace Hydro::API
