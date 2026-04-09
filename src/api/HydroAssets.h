#pragma once
///
/// @module Hydro.Assets
/// @description Load, find, spawn, and destroy game assets and actors.
///   This is the primary interface for getting content from pak files into
///   the game world. All returned objects are reflection-enabled UObjects.
/// @depends EngineAPI (StaticLoadObject, AssetRegistry, ProcessEvent)
///

struct lua_State;

namespace Hydro::API {

/// Register the Hydro.Assets module into the Lua state.
/// Called once during LuaRuntime initialization.
void registerAssetsModule(lua_State* L);

} // namespace Hydro::API
