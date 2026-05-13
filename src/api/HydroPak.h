#pragma once
///
/// @module Hydro.Pak
/// @description Mount pak files at runtime. Calls FPakPlatformFile::Mount on
///   the engine's discovered instance, then merges the optional sibling
///   AssetRegistry.bin via IAssetRegistry::AppendState.
///
///   Discovery prerequisite: HydroCore's AppendState bridge must have run
///   at least once (it captures FFilePackageStoreBackend → reverse-derives
///   FPakPlatformFile). Until then, mount() returns false.
///
/// @depends EngineAPI (mountPakAtRuntime), PakLoader, AppendState bridge
///

struct lua_State;

namespace Hydro::API {

/// Register the Hydro.Pak module into the Lua state.
/// Called once during LuaRuntime initialization.
void registerPakModule(lua_State* L);

} // namespace Hydro::API
