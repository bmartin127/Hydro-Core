///
/// @module Hydro.Events
/// @description Hook into engine events and UFunction calls.
///   Register callbacks on any UFunction. When the engine calls that
///   function, your Lua callback fires with the target object.
///
///   Implementation: patches each UFunction's native Func pointer
///   (offset 0xD8) individually, same technique as UE4SS. This catches
///   ALL calls to the hooked function regardless of dispatch path.
///
///   NOTE: For game-specific events, prefer dedicated Tier 2 modules.
///   See CONTRIBUTING_TIER2.md.
///
/// @depends EngineAPI (UFunction layout, memory access)
/// @engine_systems UFunction::Func native pointer
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

#include <string>
#include <vector>
#include <unordered_map>

namespace Hydro::API {

// Hook registry
// Per-function Func pointer patching. Each hooked UFunction has its
// native Func replaced with our dispatcher. The original is saved so
// we can call through after running Lua callbacks.

struct HookCallback {
    lua_State* L;
    int callbackRef;
    std::string modId;
};

struct HookedFunction {
    void* ufunc;           // The UFunction* we hooked
    void* originalFunc;    // Saved original Func pointer
    bool addedNativeFlag;  // True if we added FUNC_Native (need to remove on unhook)
    std::vector<HookCallback> callbacks;
};

// UFunction* -> hook data
static std::unordered_map<void*, HookedFunction> s_hooks;
static bool s_initialized = false;

// The universal hook dispatcher
// This replaces the Func pointer on hooked UFunctions.
// Signature matches UE's FNativeFuncPtr:
//   void (UObject* Context, FFrame& Stack, void* RESULT_DECL)
//
// FFrame layout (we only need the Node pointer):
//   +0x00: vtable
//   +0x08: ... (varies)
//   +0x10: UFunction* Node (the function being executed)
// Actually, we identify the function from our hook map since each
// UFunction points to this same dispatcher - we use a different approach.
//
// Since ALL hooked functions point to the SAME dispatcher, we need to
// know which UFunction was called. The FFrame::Node field tells us.
// FFrame is the second parameter (Stack). In UE5, Node is at a known offset.

// FFrame::Node offset - this is the UFunction* being executed.
// In UE5.5: FFrame inherits FOutputDevice. Layout varies but Node is
// typically at offset 0x10 or 0x18. We'll read it from the Stack param.
constexpr int FFRAME_NODE = 0x10;

static void __fastcall hydroFuncHook(void* context, void* stack, void* result) {
    // Read the UFunction being executed from FFrame::Node
    void* ufunc = nullptr;
    Engine::readPtr((uint8_t*)stack + FFRAME_NODE, &ufunc);

    // Look up hook data
    auto it = s_hooks.find(ufunc);
    if (it == s_hooks.end()) {
        // Shouldn't happen, but safety: try all hooks to find by original
        for (auto& [key, hook] : s_hooks) {
            if (hook.ufunc == ufunc) {
                it = s_hooks.find(key);
                break;
            }
        }
        if (it == s_hooks.end()) return; // No hook found - can't call original safely
    }

    auto& hook = it->second;

    // Fire Lua callbacks (pre-hook: before original executes)
    for (const auto& cb : hook.callbacks) {
        if (cb.callbackRef != LUA_NOREF && cb.L) {
            lua_rawgeti(cb.L, LUA_REGISTRYINDEX, cb.callbackRef);
            if (lua_isfunction(cb.L, -1)) {
                Lua::pushUObject(cb.L, context);
                int err = lua_pcall(cb.L, 1, 0, 0);
                if (err != 0) {
                    const char* msg = lua_tostring(cb.L, -1);
                    Hydro::logError("[Hydro.Events] Hook error (%s): %s",
                        cb.modId.c_str(), msg ? msg : "unknown");
                    lua_pop(cb.L, 1);
                }
            } else {
                lua_pop(cb.L, 1);
            }
        }
    }

    // Call original function
    if (hook.originalFunc) {
        auto original = (void(__fastcall*)(void*, void*, void*))hook.originalFunc;
        original(context, stack, result);
    }
}

// Patch a UFunction's Func pointer

// FUNC_Native flag - when set, ProcessEvent calls Func directly
// instead of going through the bytecode interpreter.
constexpr uint32_t FUNC_NATIVE = 0x00000400;

static bool patchFuncPointer(void* ufunc) {
    if (!ufunc) return false;

    // Read current Func pointer at UFUNC_FUNC (0xD8)
    void* currentFunc = nullptr;
    Engine::readPtr((uint8_t*)ufunc + Engine::UFUNC_FUNC, &currentFunc);

    if (!currentFunc) {
        Hydro::logWarn("[Hydro.Events] UFunction has null Func pointer");
        return false;
    }

    // Already hooked?
    if (currentFunc == (void*)&hydroFuncHook) {
        return true;
    }

    // Save original and patch
    auto& hook = s_hooks[ufunc];
    hook.ufunc = ufunc;
    hook.originalFunc = currentFunc;

    // Read current function flags
    uint32_t funcFlags = 0;
    Engine::readInt32((uint8_t*)ufunc + Engine::UFUNC_FLAGS, (int32_t*)&funcFlags);

#ifdef _WIN32
    // Make the UFunction writable (Func pointer + FunctionFlags)
    // Patch both Func and flags in one VirtualProtect call
    DWORD oldProtect;
    if (!VirtualProtect((uint8_t*)ufunc + Engine::UFUNC_FLAGS,
            Engine::UFUNC_FUNC + sizeof(void*) - Engine::UFUNC_FLAGS + 8,
            PAGE_READWRITE, &oldProtect)) {
        Hydro::logError("[Hydro.Events] VirtualProtect failed");
        return false;
    }

    // Replace Func pointer with our hook
    void* hookAddr = (void*)&hydroFuncHook;
    memcpy((uint8_t*)ufunc + Engine::UFUNC_FUNC, &hookAddr, sizeof(void*));

    // Set FUNC_Native flag so ProcessEvent dispatches through Func
    // (without this, non-native functions bypass Func entirely)
    if (!(funcFlags & FUNC_NATIVE)) {
        funcFlags |= FUNC_NATIVE;
        memcpy((uint8_t*)ufunc + Engine::UFUNC_FLAGS, &funcFlags, sizeof(uint32_t));
        hook.addedNativeFlag = true;
    }

    VirtualProtect((uint8_t*)ufunc + Engine::UFUNC_FLAGS,
        Engine::UFUNC_FUNC + sizeof(void*) - Engine::UFUNC_FLAGS + 8,
        oldProtect, &oldProtect);
#endif

    Hydro::logInfo("[Hydro.Events] Patched Func=%p->%p, flags=0x%X->0x%X",
        currentFunc, hookAddr, funcFlags & ~FUNC_NATIVE, funcFlags);
    return true;
}

// Restore a UFunction's original Func pointer

static void unpatchFuncPointer(void* ufunc) {
    auto it = s_hooks.find(ufunc);
    if (it == s_hooks.end()) return;

#ifdef _WIN32
    DWORD oldProtect;
    if (VirtualProtect((uint8_t*)ufunc + Engine::UFUNC_FLAGS,
            Engine::UFUNC_FUNC + sizeof(void*) - Engine::UFUNC_FLAGS + 8,
            PAGE_READWRITE, &oldProtect)) {

        // Restore original Func pointer
        memcpy((uint8_t*)ufunc + Engine::UFUNC_FUNC, &it->second.originalFunc, sizeof(void*));

        // Remove FUNC_Native flag if we added it
        if (it->second.addedNativeFlag) {
            uint32_t funcFlags = 0;
            memcpy(&funcFlags, (uint8_t*)ufunc + Engine::UFUNC_FLAGS, sizeof(uint32_t));
            funcFlags &= ~FUNC_NATIVE;
            memcpy((uint8_t*)ufunc + Engine::UFUNC_FLAGS, &funcFlags, sizeof(uint32_t));
        }

        VirtualProtect((uint8_t*)ufunc + Engine::UFUNC_FLAGS,
            Engine::UFUNC_FUNC + sizeof(void*) - Engine::UFUNC_FLAGS + 8,
            oldProtect, &oldProtect);
    }
#endif
}

// Public API

bool installProcessEventHook() {
    // No global hook needed - we patch per-function
    s_initialized = true;
    return true;
}

void uninstallProcessEventHook() {
    // Restore all patched functions
    for (auto& [ufunc, hook] : s_hooks) {
        unpatchFuncPointer(ufunc);
    }
    s_hooks.clear();
    s_initialized = false;
}

// Lua API

/// Register a hook on a UFunction by path.
///
/// Patches the UFunction's native Func pointer so your callback fires
/// whenever the engine calls that function - from any code path
/// (Blueprint, C++, network replication, timers).
///
/// @param funcPath string - Full UFunction path (e.g., "/Script/Engine.Actor:ReceiveBeginPlay")
/// @param callback function - Called when the function fires. Receives (self).
/// @returns boolean - true if the hook was registered
/// @throws string - If funcPath is invalid or function not found
/// @engine UFunction::Func native pointer replacement
/// @example
/// local Events = require("Hydro.Events")
/// Events.hook("/Script/Engine.Actor:ReceiveBeginPlay", function(self)
///     print("An actor began play: " .. tostring(self))
/// end)
static int l_events_hook(lua_State* L) {
    const char* funcPath = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);

    // Resolve the UFunction
    std::wstring widePath(funcPath, funcPath + strlen(funcPath));
    void* func = Engine::findObject(widePath.c_str());
    if (!func) {
        return luaL_error(L, "UFunction not found: %s", funcPath);
    }

    // Patch the UFunction's Func pointer if not already hooked
    if (!patchFuncPointer(func)) {
        return luaL_error(L, "Failed to hook: %s", funcPath);
    }

    // Register the Lua callback
    HookCallback cb;
    cb.L = L;

    lua_getfield(L, LUA_ENVIRONINDEX, "_MOD_ID");
    cb.modId = lua_isstring(L, -1) ? lua_tostring(L, -1) : "unknown";
    lua_pop(L, 1);

    lua_pushvalue(L, 2);
    cb.callbackRef = luaL_ref(L, LUA_REGISTRYINDEX);

    s_hooks[func].callbacks.push_back(cb);

    // Log original Func pointer and UFunction flags for debugging
    uint32_t funcFlags = 0;
    Engine::readInt32((uint8_t*)func + Engine::UFUNC_FLAGS, (int32_t*)&funcFlags);
    Hydro::logInfo("[Hydro.Events] Hooked %s (mod: %s, originalFunc=%p, funcFlags=0x%X)",
        funcPath, cb.modId.c_str(), s_hooks[func].originalFunc, funcFlags);
    lua_pushboolean(L, 1);
    return 1;
}

/// Remove all hooks registered by the calling mod.
///
/// @returns number - Count of hooks removed
static int l_events_unhookAll(lua_State* L) {
    lua_getfield(L, LUA_ENVIRONINDEX, "_MOD_ID");
    std::string modId = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
    lua_pop(L, 1);

    int removed = 0;
    for (auto& [func, hook] : s_hooks) {
        auto before = hook.callbacks.size();
        hook.callbacks.erase(
            std::remove_if(hook.callbacks.begin(), hook.callbacks.end(),
                [&](const HookCallback& cb) {
                    if (cb.modId == modId) {
                        if (cb.callbackRef != LUA_NOREF)
                            luaL_unref(L, LUA_REGISTRYINDEX, cb.callbackRef);
                        return true;
                    }
                    return false;
                }),
            hook.callbacks.end());
        removed += (int)(before - hook.callbacks.size());

        // If no callbacks left, restore original Func
        if (hook.callbacks.empty()) {
            unpatchFuncPointer(func);
        }
    }

    lua_pushinteger(L, removed);
    return 1;
}

// Module registration

static const luaL_Reg events_functions[] = {
    {"hook",       l_events_hook},
    {"unhookAll",  l_events_unhookAll},
    {nullptr,      nullptr}
};

void registerEventsModule(lua_State* L) {
    buildModuleTable(L, events_functions);
}

} // namespace Hydro::API
