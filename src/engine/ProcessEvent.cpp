#include "ProcessEvent.h"
#include "../EngineAPI.h"
#include "../HydroCore.h"
#include "Internal.h"
#include "GUObjectArray.h"
#include "Reflection.h"

#include <cstring>

#ifdef _WIN32
#include <windows.h>
#endif

namespace Hydro::Engine {

ProcessEventFn s_processEvent = nullptr;

// Cached ProcessEvent vtable slot. -1 = not yet probed. UObject's vtable
// layout shifted across UE versions; UE4SS data confirms slot positions:
//   5.0 -> 75 (0x258), 5.1/5.6/5.7 -> 76 (0x260), 5.2-5.4 -> 77 (0x268),
//   5.5 -> 79 (0x278). Probing on first call avoids hardcoded-per-version
// branches and discovers the right slot empirically.
static int s_peSlot = -1;
static bool s_peProbeFailed = false;

// SEH-wrapped call through a specific vtable slot. Used both by the
// runtime fast path (after s_peSlot is known) and by the discovery probe.
// Returns true if the call completed without an unhandled exception.
static bool callPESlot(void** vtable, int slot, void* obj, void* func, void* params) {
    void* pe = nullptr;
    if (!safeReadPtr(&vtable[slot], &pe) || !pe) return false;
    bool crashed = false;
#ifdef _WIN32
    __try {
        ((ProcessEventFn)pe)(obj, func, params);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        crashed = true;
    }
#else
    ((ProcessEventFn)pe)(obj, func, params);
#endif
    return !crashed;
}

bool callProcessEvent(void* obj, void* func, void* params) {
    if (!obj || !func) return false;
    void** vtable = *(void***)obj;
    if (!vtable) return false;

    // Steady-state fast path.
    if (s_peSlot >= 0) {
        void* pe = nullptr;
        if (!safeReadPtr(&vtable[s_peSlot], &pe) || !pe) return false;
        bool crashed = false;
        DWORD crashCode = 0;
        void* crashAddr = nullptr;
#ifdef _WIN32
        __try {
            ((ProcessEventFn)pe)(obj, func, params);
        } __except(crashCode = GetExceptionCode(),
                   crashAddr = GetExceptionInformation()->ExceptionRecord->ExceptionAddress,
                   EXCEPTION_EXECUTE_HANDLER) {
            crashed = true;
        }
#else
        ((ProcessEventFn)pe)(obj, func, params);
#endif
        if (crashed) {
            Hydro::logError("EngineAPI: ProcessEvent crashed (slot=%d code=0x%08X at %p obj=%p func=%p)",
                s_peSlot, (unsigned)crashCode, crashAddr, obj, func);
            return false;
        }
        return true;
    }

    if (s_peProbeFailed) return false;

    // First call - discover the slot. UE 5.5's hardcoded 79 silently calls
    // a wrong virtual on 5.6 (which has slot 76 after Epic removed three
    // virtuals from UObject). Probe slots in order of likelihood; the one
    // whose call mutates the param block is the real ProcessEvent.
    //
    // Strategy: cap probe size at the UFunction's ParmsSize so we don't
    // overrun the caller's buffer. Snapshot before each probe, restore
    // between probes. First slot whose call modifies memcmp wins.
    size_t parmsSize = (size_t)getUFunctionParmsSize(func);
    if (parmsSize == 0 || parmsSize > 512) parmsSize = 256;

    uint8_t backup[512];
    memcpy(backup, params, parmsSize);

    // Priority: 79 (5.5, current default, validates existing UE 5.5 path
    // first), then 76 (5.1/5.6/5.7), 77 (5.2-5.4), 78, 75 (5.0), wider
    // sweep last. Covers all UE 5.x.
    static const int probeOrder[] = { 79, 76, 77, 78, 75, 80, 74, 81, 73, 82, 72, 83 };
    for (int probeIdx = 0; probeIdx < (int)(sizeof(probeOrder)/sizeof(probeOrder[0])); probeIdx++) {
        int slot = probeOrder[probeIdx];
        memcpy(params, backup, parmsSize);

        if (!callPESlot(vtable, slot, obj, func, params)) {
            // Crashed - wrong slot, restore and continue
            memcpy(params, backup, parmsSize);
            continue;
        }

        if (memcmp(params, backup, parmsSize) != 0) {
            s_peSlot = slot;
            Hydro::logInfo("EngineAPI: ProcessEvent vtable slot discovered = %d (offset 0x%X) "
                           "by param-mutation probe; first dispatched call already ran",
                           slot, slot * 8);
            return true;  // the winning probe already executed the caller's intended call
        }
    }

    // No slot worked. Default to the 5.5 value so subsequent presence checks
    // (s_processEvent != nullptr) keep working, but flag the probe failure
    // so we don't burn cycles re-probing every call.
    s_peProbeFailed = true;
    Hydro::logError("EngineAPI: ProcessEvent slot discovery FAILED - no vtable slot mutated "
                    "the param block. Spawn / UFunction calls will silently no-op.");
    return false;
}

void* getProcessEventAddress() {
    return (void*)s_processEvent;
}

bool findProcessEvent() {
    // Get the first valid UObject from GUObjectArray
    int32_t count = getObjectCount();
    for (int32_t i = 0; i < count && i < 100; i++) {
        void* obj = getObjectAt(i);
        if (!obj) continue;

        // Read vtable pointer
        void** vtable = nullptr;
        if (!safeReadPtr(obj, (void**)&vtable) || !vtable) continue;

        // ProcessEvent is at vtable index 79 (offset 0x278)
        void* pe = nullptr;
        if (!safeReadPtr(&vtable[VTABLE_PROCESS_EVENT], &pe) || !pe) continue;

        // Verify it looks like code (in the game module range)
        if ((uint8_t*)pe >= s_gm.base && (uint8_t*)pe < s_gm.base + s_gm.size) {
            s_processEvent = (ProcessEventFn)pe;
            Hydro::logInfo("EngineAPI: ProcessEvent at %p (from obj[%d])", pe, i);
            return true;
        }
    }

    Hydro::logError("EngineAPI: ProcessEvent not found in vtables");
    return false;
}

} // namespace Hydro::Engine
