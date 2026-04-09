#pragma once
///
/// @module Hydro.World
/// @description Discover and query game objects in the world.
///   Find actors by class, get the player, access the world.
///   All returned objects are reflection-enabled UObjects.
/// @depends EngineAPI (GUObjectArray, StaticFindObject, UWorld)
///

struct lua_State;

namespace Hydro::API {

/// Register the Hydro.World module into the Lua state.
/// Called once during LuaRuntime initialization.
void registerWorldModule(lua_State* L);

} // namespace Hydro::API
