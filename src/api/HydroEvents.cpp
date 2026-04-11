///
/// @module Hydro.Events
/// @description Hook into engine events and UFunction calls.
///   Register callbacks on any UFunction. When the engine calls that
///   function, your Lua callback fires with the target object.
///
///   Two x64 inline detours are installed into the engine's function
///   dispatch pipeline: one on UObject::ProcessEvent and one on
///   UObject::ProcessInternal. Some Blueprint event dispatch paths
///   (including generated C++ stubs for BlueprintImplementableEvent
///   events like ReceiveBeginPlay) enter via ProcessInternal rather
///   than the outer ProcessEvent, so we have to catch both to hook
///   actor lifecycle events reliably. No UFunction struct is mutated,
///   so the base Actor's BeginPlay dispatch stays intact and character
///   animation blueprints keep working.
///
///   For game-specific events, prefer dedicated Tier 2 modules.
///   See CONTRIBUTING_TIER2.md.
///
/// @depends EngineAPI (ProcessEvent address, findObject, UFUNC_FUNC)
/// @engine_systems UObject::ProcessEvent, UObject::ProcessInternal
///

#include "HydroEvents.h"
#include "ModuleRegistry.h"
#include "../HydroCore.h"
#include "../EngineAPI.h"
#include "../LuaUObject.h"

extern "C" {
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>
}

#ifdef _WIN32
#include <windows.h>
#endif

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Hydro::API {

// Hook registry
//
// Hooks are keyed by the FName index of the UFunction's name, not by its
// UFunction* pointer. This matters because Blueprint subclasses generate
// their own per-class override UFunctions - e.g., hooking
// "/Script/Engine.Actor:ReceiveBeginPlay" should also fire when a BP
// subclass's own ReceiveBeginPlay override runs. Matching by pointer
// would miss every subclass override. Matching by FName index catches
// all of them: every UFunction with the name "ReceiveBeginPlay", no
// matter which class owns it.

struct HookCallback {
    lua_State* L;
    int callbackRef;
    std::string modId;
    std::string funcPath; // original path for diagnostics
};

struct HookedName {
    std::string firstPath;
    std::vector<HookCallback> callbacks;
};

// FName index -> hook data. Accessed from the game thread during detour
// dispatch and from Lua during hook(). The mutex protects registration;
// the detour snapshots callbacks under the lock then fires them outside.
static std::unordered_map<uint32_t, HookedName> s_hooks;
static std::mutex s_hooksMutex;
static bool s_hookInstalled = false;

// FName index is stored at the UObjectBase::NamePrivate field. On UE5.5
// that's offset 0x18 from the UObject base for a ComparisonIndex (uint32).
constexpr int UOBJECT_NAME_OFFSET = 0x18;

static uint32_t readNameIdx(void* ufunc) {
    if (!ufunc) return 0;
    uint32_t idx = 0;
    Engine::readInt32((uint8_t*)ufunc + UOBJECT_NAME_OFFSET, (int32_t*)&idx);
    return idx;
}

// Inline hook state
//
// We install two separate inline hooks: one on ProcessEvent (outer dispatch
// entry point for script calls), and one on ProcessInternal (the inner VM
// entry point that BP events sometimes bypass straight to). Both detours
// share the same hook map - each one reads the UFunction from its argument
// frame and looks up matching hooks by FName index.

using ProcessEventFn = void(__fastcall*)(void*, void*, void*);
using ProcessInternalFn = void(__fastcall*)(void*, void*, void*);

static ProcessEventFn s_peTrampoline = nullptr;
static ProcessInternalFn s_piTrampoline = nullptr;
static bool s_peHooked = false;
static bool s_piHooked = false;

// FFrame::Node offset - the UFunction pointer embedded in the execution
// frame passed to ProcessInternal. UE5.5 layout.
constexpr int FFRAME_NODE_OFFSET = 0x10;

// Minimal x64 length decoder

// Decodes instructions starting at 'code' until the total byte count is
// at least 'minBytes'. Returns the total decoded length, or 0 on failure.
// Only handles the opcodes that appear in MSVC function prologues - if an
// unexpected opcode is encountered we refuse to hook rather than guess.
static int decodePrologueLength(const uint8_t* code, int minBytes) {
    int pos = 0;
    while (pos < minBytes) {
        const uint8_t* p = code + pos;
        int rexLen = 0;

        // REX prefix (0x40-0x4F)
        if ((p[0] & 0xF0) == 0x40) {
            rexLen = 1;
            p++;
        }

        uint8_t op = p[0];
        int len = 0;

        auto modrmLen = [](const uint8_t* pp) -> int {
            uint8_t modrm = pp[1];
            uint8_t mod = (modrm >> 6) & 3;
            uint8_t rm = modrm & 7;
            int sib = 0;
            int disp = 0;
            if (mod != 3 && rm == 4) sib = 1;
            if (mod == 0) {
                if (rm == 5) disp = 4;
            } else if (mod == 1) {
                disp = 1;
            } else if (mod == 2) {
                disp = 4;
            }
            return 2 + sib + disp;
        };

        if (op >= 0x50 && op <= 0x5F) {
            // push/pop reg
            len = rexLen + 1;
        } else if (op == 0x90) {
            // nop
            len = rexLen + 1;
        } else if (op == 0x8B || op == 0x89 || op == 0x8D) {
            // mov reg, r/m  |  mov r/m, reg  |  lea reg, mem
            len = rexLen + modrmLen(p);
        } else if (op == 0x83 && (p[1] & 0x38) == 0x28) {
            // sub r/m, imm8  (0x83 /5)
            len = rexLen + modrmLen(p) + 1;
        } else if (op == 0x81 && (p[1] & 0x38) == 0x28) {
            // sub r/m, imm32 (0x81 /5)
            len = rexLen + modrmLen(p) + 4;
        } else if (op == 0x83 && (p[1] & 0x38) == 0x20) {
            // and r/m, imm8  (0x83 /4)
            len = rexLen + modrmLen(p) + 1;
        } else if (op == 0x33 || op == 0x31) {
            // xor reg, r/m
            len = rexLen + modrmLen(p);
        } else {
            logError("[Hydro.Events] Unknown opcode 0x%02X in ProcessEvent prologue at +%d",
                     op, pos);
            return 0;
        }

        pos += len;
    }
    return pos;
}

// Install/uninstall the inline hooks

// Detours defined further down, forward declared so installers can take their address.
static void __fastcall hydroProcessEventDetour(void* obj, void* func, void* params);
static void __fastcall hydroProcessInternalDetour(void* obj, void* stack, void* result);

// Follow trampoline jmps to find the real function body. Handles the two
// forms we typically see from incremental linkers and PLT thunks:
//   E9 XX XX XX XX              jmp rel32
//   FF 25 XX XX XX XX  <ptr>    jmp qword [rip+disp32]
// Stops when the first instruction is anything else. Recurses through
// chained jmps (up to 8 levels - enough for any real trampoline chain).
static void* resolveJmpTrampoline(void* addr, int depth = 0) {
    if (!addr || depth >= 8) return addr;
    const uint8_t* p = (const uint8_t*)addr;
    if (p[0] == 0xE9) {
        int32_t rel = *(const int32_t*)(p + 1);
        void* target = (uint8_t*)addr + 5 + rel;
        return resolveJmpTrampoline(target, depth + 1);
    }
    if (p[0] == 0xFF && p[1] == 0x25) {
        int32_t disp = *(const int32_t*)(p + 2);
        void** slot = (void**)((uint8_t*)addr + 6 + disp);
        void* target = nullptr;
        if (Engine::readPtr(slot, &target) && target) {
            return resolveJmpTrampoline(target, depth + 1);
        }
    }
    return addr;
}

// Core inline-hook installer. Target gets a 14-byte absolute jmp to the
// detour; an executable trampoline is allocated that runs the original
// prologue bytes and jumps back to target+N. Returns the trampoline
// pointer on success, nullptr on failure.
static void* installInlineHookAt(void* target, void* detour, const char* label) {
    if (!target) {
        logError("[Hydro.Events] %s address not available", label);
        return nullptr;
    }

    int savedBytes = decodePrologueLength((const uint8_t*)target, 14);
    if (savedBytes == 0 || savedBytes > 32) {
        logError("[Hydro.Events] Could not decode %s prologue", label);
        return nullptr;
    }

    const size_t trampolineSize = savedBytes + 14;
    uint8_t* tramp = (uint8_t*)VirtualAlloc(
        nullptr, trampolineSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) {
        logError("[Hydro.Events] Failed to allocate %s trampoline", label);
        return nullptr;
    }

    memcpy(tramp, target, savedBytes);

    // Append absolute jmp back to target + savedBytes
    uint64_t returnAddr = (uint64_t)target + savedBytes;
    tramp[savedBytes + 0] = 0xFF;
    tramp[savedBytes + 1] = 0x25;
    tramp[savedBytes + 2] = 0x00;
    tramp[savedBytes + 3] = 0x00;
    tramp[savedBytes + 4] = 0x00;
    tramp[savedBytes + 5] = 0x00;
    memcpy(tramp + savedBytes + 6, &returnAddr, sizeof(uint64_t));

    DWORD oldProtect = 0;
    if (!VirtualProtect(target, 14, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        logError("[Hydro.Events] VirtualProtect on %s failed", label);
        VirtualFree(tramp, 0, MEM_RELEASE);
        return nullptr;
    }

    uint8_t* t = (uint8_t*)target;
    uint64_t detourAddr = (uint64_t)detour;
    t[0] = 0xFF;
    t[1] = 0x25;
    t[2] = 0x00;
    t[3] = 0x00;
    t[4] = 0x00;
    t[5] = 0x00;
    memcpy(t + 6, &detourAddr, sizeof(uint64_t));

    VirtualProtect(target, 14, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), target, 14);

    logInfo("[Hydro.Events] Inline hook installed on %s at %p (saved %d bytes, trampoline at %p)",
            label, target, savedBytes, tramp);
    return tramp;
}

// Discover ProcessInternal via UE4SS's ExecuteUbergraph trick.
//
// ProcessInternal isn't exposed via a vtable entry we can pattern-match
// easily. But every UFunction has a `Func` native pointer at offset 0xD8,
// and the `/Script/CoreUObject.Object:ExecuteUbergraph` UFunction's Func
// pointer IS ProcessInternal - that's how the engine wires BP script
// dispatch. Read it out, follow any jmp trampolines, done.
static void* discoverProcessInternal() {
    void* ubergraph = Engine::findObject(L"/Script/CoreUObject.Object:ExecuteUbergraph");
    if (!ubergraph) {
        logError("[Hydro.Events] ExecuteUbergraph UFunction not found");
        return nullptr;
    }
    void* funcPtr = nullptr;
    if (!Engine::readPtr((uint8_t*)ubergraph + Engine::UFUNC_FUNC, &funcPtr) || !funcPtr) {
        logError("[Hydro.Events] ExecuteUbergraph Func pointer unreadable");
        return nullptr;
    }
    void* resolved = resolveJmpTrampoline(funcPtr);
    logInfo("[Hydro.Events] ProcessInternal at %p (via ExecuteUbergraph.Func=%p)",
            resolved, funcPtr);
    return resolved;
}

static bool installInlineHook() {
    // Install ProcessEvent hook
    if (!s_peHooked) {
        void* peTarget = Engine::getProcessEventAddress();
        void* peTramp = installInlineHookAt(peTarget,
            (void*)&hydroProcessEventDetour, "ProcessEvent");
        if (peTramp) {
            s_peTrampoline = (ProcessEventFn)peTramp;
            s_peHooked = true;
        }
    }

    // Install ProcessInternal hook - catches BP dispatches that bypass PE
    if (!s_piHooked) {
        void* piTarget = discoverProcessInternal();
        void* piTramp = installInlineHookAt(piTarget,
            (void*)&hydroProcessInternalDetour, "ProcessInternal");
        if (piTramp) {
            s_piTrampoline = (ProcessInternalFn)piTramp;
            s_piHooked = true;
        }
    }

    // At least one must succeed. If PE succeeded we can still catch some events.
    s_hookInstalled = s_peHooked || s_piHooked;
    return s_hookInstalled;
}

// The detour

// Shared callback dispatch: fire every hook whose FName index matches.
// Called by both detours with the UFunction resolved from their own
// argument shape. The source tag lets us log which detour a matched
// hook came through - useful for verifying both dispatch paths work.
static void fireHooksForFunction(const char* source, void* obj, void* func) {
    uint32_t nameIdx = readNameIdx(func);
    if (nameIdx == 0) return;

    std::vector<HookCallback> callbacksCopy;
    {
        std::lock_guard<std::mutex> lock(s_hooksMutex);
        auto it = s_hooks.find(nameIdx);
        if (it != s_hooks.end()) {
            callbacksCopy = it->second.callbacks;
        }
    }

    if (callbacksCopy.empty()) return;

    // A matched hook is the interesting signal - log which detour it came
    // through so we can confirm PE vs PI dispatch end-to-end.
    logInfo("[Hydro.Events] %s detour matched hook (nameIdx=0x%X, %d callbacks)",
            source, nameIdx, (int)callbacksCopy.size());

    for (const auto& cb : callbacksCopy) {
        if (cb.callbackRef == LUA_NOREF || !cb.L) continue;
        lua_rawgeti(cb.L, LUA_REGISTRYINDEX, cb.callbackRef);
        if (lua_isfunction(cb.L, -1)) {
            Lua::pushUObject(cb.L, obj);
            int err = lua_pcall(cb.L, 1, 0, 0);
            if (err != 0) {
                const char* msg = lua_tostring(cb.L, -1);
                logError("[Hydro.Events] Hook error (%s): %s",
                         cb.modId.c_str(), msg ? msg : "unknown");
                lua_pop(cb.L, 1);
            }
        } else {
            lua_pop(cb.L, 1);
        }
    }
}

static void __fastcall hydroProcessEventDetour(void* obj, void* func, void* params) {
    // Fast path: no hooks registered at all.
    if (s_hooks.empty()) {
        s_peTrampoline(obj, func, params);
        return;
    }
    fireHooksForFunction("PE", obj, func);
    s_peTrampoline(obj, func, params);
}

// ProcessInternal detour - signature is (UObject* this, FFrame& Stack, void* result).
// The UFunction being executed lives in FFrame::Node somewhere near the
// start of the struct after the FOutputDevice base. Offset varies by UE
// version and compiler padding; we probe a few candidate slots on the
// first call to find whichever yields a non-null pointer with a valid
// FName index, then lock that offset in for all subsequent calls.
static void __fastcall hydroProcessInternalDetour(void* obj, void* stack, void* result) {
    if (s_hooks.empty()) {
        s_piTrampoline(obj, stack, result);
        return;
    }

    static int s_nodeOffset = -1;

    void* func = nullptr;
    if (stack) {
        if (s_nodeOffset >= 0) {
            Engine::readPtr((uint8_t*)stack + s_nodeOffset, &func);
        } else {
            // First call: probe candidate Node offsets until one reads a
            // plausible UFunction pointer. Locks in for the rest of the run.
            static const int kCandidates[] = { 0x10, 0x18, 0x20, 0x28, 0x08 };
            for (int off : kCandidates) {
                void* candidate = nullptr;
                if (!Engine::readPtr((uint8_t*)stack + off, &candidate)) continue;
                if (!candidate) continue;
                uint32_t nameIdx = readNameIdx(candidate);
                if (nameIdx != 0 && nameIdx < 0x200000) {
                    s_nodeOffset = off;
                    func = candidate;
                    logInfo("[Hydro.Events] PI FFrame::Node locked in at offset 0x%X",
                            off);
                    break;
                }
            }
        }
    }

    if (func) {
        fireHooksForFunction("PI", obj, func);
    }
    s_piTrampoline(obj, stack, result);
}

// Lookup: read _MOD_ID from the calling Lua chunk's environment

static std::string getCallerModId(lua_State* L) {
    lua_Debug ar;
    for (int level = 1; level < 16; level++) {
        if (lua_getstack(L, level, &ar) == 0) break;
        if (lua_getinfo(L, "f", &ar) == 0) continue;
        if (!lua_isfunction(L, -1)) { lua_pop(L, 1); continue; }
        lua_getfenv(L, -1);
        if (lua_istable(L, -1)) {
            lua_getfield(L, -1, "_MOD_ID");
            if (lua_isstring(L, -1)) {
                std::string modId = lua_tostring(L, -1);
                lua_pop(L, 3);
                return modId;
            }
            lua_pop(L, 1);
        }
        lua_pop(L, 2);
    }
    return "";
}

// Public install/uninstall (kept for API compatibility)

bool installProcessEventHook() {
    // No eager install - the inline hook is installed lazily on first hook().
    return true;
}

void uninstallProcessEventHook() {
    std::lock_guard<std::mutex> lock(s_hooksMutex);
    for (auto& [nameIdx, hook] : s_hooks) {
        for (auto& cb : hook.callbacks) {
            if (cb.callbackRef != LUA_NOREF && cb.L) {
                luaL_unref(cb.L, LUA_REGISTRYINDEX, cb.callbackRef);
            }
        }
    }
    s_hooks.clear();
    // We deliberately do not restore the ProcessEvent prologue. Leaving the
    // detour in place with an empty hook map is effectively free (the fast
    // path is a single branch), and reverting an inline hook while the game
    // may still be mid-ProcessEvent call is unsafe.
}

// Lua API

/// Register a hook on a UFunction by path.
///
/// The first time a mod calls hook(), HydroCore installs an inline detour
/// on the global ProcessEvent function. Subsequent hooks just add entries
/// to the lookup table - no UFunction struct is ever mutated, so the
/// engine's dispatch behavior for unrelated functions is untouched.
///
/// @param funcPath string - Full UFunction path (e.g., "/Script/Engine.Actor:ReceiveBeginPlay")
/// @param callback function - Called when the function fires. Receives (self).
/// @returns boolean - true if the hook was registered
/// @throws string - If funcPath is invalid or function not found
/// @example
/// local Events = require("Hydro.Events")
/// Events.hook("/Script/Engine.Actor:ReceiveBeginPlay", function(self)
///     print("An actor began play: " .. tostring(self))
/// end)
static int l_events_hook(lua_State* L) {
    const char* funcPath = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);

    std::wstring widePath(funcPath, funcPath + strlen(funcPath));
    void* func = Engine::findObject(widePath.c_str());
    if (!func) {
        return luaL_error(L, "UFunction not found: %s", funcPath);
    }

    if (!installInlineHook()) {
        return luaL_error(L, "Failed to install ProcessEvent hook");
    }

    uint32_t nameIdx = readNameIdx(func);
    if (nameIdx == 0) {
        return luaL_error(L, "Could not read UFunction name index for %s", funcPath);
    }

    HookCallback cb;
    cb.L = L;
    cb.modId = getCallerModId(L);
    if (cb.modId.empty()) cb.modId = "unknown";
    cb.funcPath = funcPath;
    lua_pushvalue(L, 2);
    cb.callbackRef = luaL_ref(L, LUA_REGISTRYINDEX);

    {
        std::lock_guard<std::mutex> lock(s_hooksMutex);
        auto& hook = s_hooks[nameIdx];
        if (hook.firstPath.empty()) hook.firstPath = funcPath;
        hook.callbacks.push_back(std::move(cb));
    }

    logInfo("[Hydro.Events] Hooked %s (mod: %s, nameIdx=0x%X)",
            funcPath, s_hooks[nameIdx].callbacks.back().modId.c_str(), nameIdx);
    lua_pushboolean(L, 1);
    return 1;
}

/// Remove all hooks registered by the calling mod.
///
/// @returns number - Count of hooks removed
static int l_events_unhookAll(lua_State* L) {
    std::string modId = getCallerModId(L);

    int removed = 0;
    std::lock_guard<std::mutex> lock(s_hooksMutex);
    for (auto it = s_hooks.begin(); it != s_hooks.end(); ) {
        auto& hook = it->second;
        auto before = hook.callbacks.size();
        hook.callbacks.erase(
            std::remove_if(hook.callbacks.begin(), hook.callbacks.end(),
                [&](const HookCallback& cb) {
                    if (cb.modId == modId) {
                        if (cb.callbackRef != LUA_NOREF) {
                            luaL_unref(L, LUA_REGISTRYINDEX, cb.callbackRef);
                        }
                        return true;
                    }
                    return false;
                }),
            hook.callbacks.end());
        removed += (int)(before - hook.callbacks.size());

        if (hook.callbacks.empty()) {
            it = s_hooks.erase(it);
        } else {
            ++it;
        }
    }

    lua_pushinteger(L, removed);
    return 1;
}

// Module registration

static const luaL_Reg events_functions[] = {
    {"hook",      l_events_hook},
    {"unhookAll", l_events_unhookAll},
    {nullptr,     nullptr}
};

void registerEventsModule(lua_State* L) {
    buildModuleTable(L, events_functions);
}

} // namespace Hydro::API
