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

#include <string>

struct lua_State;

namespace Hydro::API {

/// Register the Hydro.Events module into the Lua state.
void registerEventsModule(lua_State* L);

/// Install the ProcessEvent hook. Call once after EngineAPI is ready.
/// Returns true if the hook was installed successfully.
bool installProcessEventHook();

/// Uninstall the hook and restore original ProcessEvent.
void uninstallProcessEventHook();

/// Install a trace listener that logs every ProcessEvent / ProcessInternal /
/// BeginPlay call on the given target UObject for `seconds` seconds.
/// Output is JSONL (one event per line) at `outPath`. Returns true if the
/// trace was installed. Trace auto-expires and closes its output file after
/// the duration elapses - callers do not need to uninstall.
///
/// Piggybacks on the existing inline hook detours - does NOT install a
/// second inline hook. Safe to register multiple concurrent traces for
/// different targets.
bool addTrace(void* target, int seconds, const std::string& outPath);

/// Called from the game thread tick loop. Walks the active trace list,
/// flushes buffered output, and closes/removes expired traces.
void tickTraces();

/// Called from RegisterEngineTickPostCallback to pump yielded coroutines
/// every frame. The PE detour also pumps, but PE is dormant on title
/// screens / load screens, so waiting scripts can stall. Engine-tick pump
/// drives them regardless of UFunction traffic.
void tickEvents();

/// Register a top-level script coroutine that has yielded via wait(). The
/// caller (LuaRuntime) has already created the thread on `mainL`, set up
/// its environment, resumed it until the yield, and stored `regRef` as a
/// LUA_REGISTRYINDEX ref keeping `co` alive. The scheduler will resume
/// `co` once `seconds` have elapsed, and release regRef when `co`
/// completes or errors.
void registerYieldedCoroutine(lua_State* mainL, lua_State* co,
                              double seconds, const std::string& modId,
                              int regRef);

/// Lua C function for the global `wait(seconds)`. Yields the current
/// coroutine until the scheduler resumes it.
int globalWaitBinding(lua_State* L);

} // namespace Hydro::API
