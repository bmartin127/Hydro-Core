#pragma once
///
/// @module Hydro.Net
/// @description Network role detection and modpack identity for multiplayer mods.
///   Provides the foundational hooks every multiplayer-aware mod needs:
///   - isHost() / mode() - query the current network role at runtime
///   - getModpackHash() - deterministic identity for the active mod set
///
///   Does NOT yet provide a custom RPC primitive (send/broadcast/on) - that
///   ships once the per-game replicated router asset is available.
/// @depends EngineAPI (UWorld::GetNetMode via reflection), Manifest
///

struct lua_State;

namespace Hydro::API {

/// Register the Hydro.Net module into the Lua state.
/// Called once during LuaRuntime initialization.
void registerNetModule(lua_State* L);

} // namespace Hydro::API
