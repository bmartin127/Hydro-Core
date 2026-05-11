#pragma once
///
/// @module Hydro.UI
/// @description Runtime UMG widget construction primitives. Wraps
///   StaticConstructObject_Internal so Lua mods can NewObject any UClass
///   without authoring a cooked Blueprint pak.
///
///   The base primitive is `Hydro.UI.newObject(class, outer?)`. On top of
///   it sit higher-level helpers (addToViewport, setText, etc.) that call
///   the matching reflected UFunctions on the constructed widget.
///
///   Why this lives here: UMG widgets aren't AActor subclasses, so
///   Hydro.Assets.spawn (which routes through BeginDeferredActorSpawnFromClass)
///   can't create them. NewObject is the correct primitive. SCO is
///   resolved by HydroCore itself - see project_sco_resolver_design memory
///   for why we don't rely on UE4SS's patternsleuth here.
///
/// @depends EngineAPI (staticConstructObject, findObject, callFunction)
///

struct lua_State;

namespace Hydro::API {

/// Register the Hydro.UI module into the Lua state.
/// Called once during LuaRuntime initialization.
void registerUIModule(lua_State* L);

} // namespace Hydro::API
