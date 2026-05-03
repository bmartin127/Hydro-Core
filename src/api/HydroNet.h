#pragma once
///
/// @module Hydro.Net
/// @description Network role detection and modpack identity.
///   isHost() and mode() expose the current UWorld net mode.
///   getModpackHash() returns a deterministic identifier for the active
///   mod set. Custom RPC primitives are not implemented here.
/// @depends EngineAPI (UWorld::GetNetMode via reflection), Manifest
///

struct lua_State;

namespace Hydro::API {

/// Register the Hydro.Net module into the Lua state.
/// Called once during LuaRuntime initialization.
void registerNetModule(lua_State* L);

} // namespace Hydro::API
