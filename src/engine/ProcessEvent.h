#pragma once

namespace Hydro::Engine {

using ProcessEventFn = void(__fastcall*)(void*, void*, void*);

extern ProcessEventFn s_processEvent;

// Runtime UFunction dispatcher. First call probes the vtable to find the
// real ProcessEvent slot for this UE version (5.5=79, 5.6=76, etc.), caches
// it, and dispatches via that slot. Returns false on SEH crash or probe
// failure.
bool callProcessEvent(void* obj, void* func, void* params);

// Discover s_processEvent from a known UObject's vtable. Called during init;
// the runtime probe in callProcessEvent supersedes it once a UFunction call
// happens, but s_processEvent remains a useful diagnostic anchor.
bool findProcessEvent();

} // namespace Hydro::Engine
