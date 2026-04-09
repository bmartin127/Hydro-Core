#pragma once
///
/// @module Hydro.Events
/// @description Hook into engine events and UFunction calls.
///   Register pre/post callbacks on any UFunction. When the engine calls
///   that function, your Lua callback fires with the parameters.
///   This is what makes mods interactive - react to damage, death,
///   pickup, crafting, or any other game event.
/// @depends EngineAPI (ProcessEvent hook, UFunction reflection)
/// @engine_systems UObject::ProcessEvent
///

struct lua_State;

namespace Hydro::API {

/// Register the Hydro.Events module into the Lua state.
void registerEventsModule(lua_State* L);

/// Install the ProcessEvent hook. Call once after EngineAPI is ready.
/// Returns true if the hook was installed successfully.
bool installProcessEventHook();

/// Uninstall the hook and restore original ProcessEvent.
void uninstallProcessEventHook();

} // namespace Hydro::API
