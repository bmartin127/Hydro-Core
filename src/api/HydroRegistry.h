#pragma once

struct lua_State;

namespace Hydro::API {

// Push the Hydro.Registry module table on the Lua stack. Called by
// LuaRuntime::registerModule during mod loading.
void registerRegistryModule(lua_State* L);

} // namespace Hydro::API
