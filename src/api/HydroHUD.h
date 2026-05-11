#pragma once
///
/// @module Hydro.HUD
/// @description Per-frame HUD drawing primitives (text, textures) backed by
///   UE's UCanvas. Plus minimal helpers to access the local player's HUD.
///
///   The drawing API is callback-based - you register a function via
///   `Hydro.HUD.onDraw(fn)` and the engine calls it inside its native
///   AHUD::DrawHUD pass with a live UCanvas. Calling drawText/drawTexture
///   outside the callback is a no-op because the canvas is only valid
///   during that pass.
///
/// @depends EngineAPI (getPlayerCharacter, findProperty, findFunction,
///   callFunction); HydroEvents (UFunction-hook infrastructure)
///

struct lua_State;

namespace Hydro::API {

/// Register the Hydro.HUD module into the Lua state.
/// Called once during LuaRuntime initialization.
void registerHUDModule(lua_State* L);

} // namespace Hydro::API
