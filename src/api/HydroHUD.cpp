///
/// @module Hydro.HUD
/// @description Per-frame HUD drawing primitives + local player HUD
///   discovery. Modders register an `onDraw` callback that fires every
///   time the engine paints the HUD; inside, they call `drawText` /
///   `drawTexture` / etc. to render overlays. Engine-level - works on
///   any UE5 host without per-game adjustment.
///
///   Implementation: the underlying drawing is delegated to UE's own
///   reflected `AHUD::DrawText` and `AHUD::DrawTextureSimple` UFunctions
///   via the existing UObject metatable colon-method dispatch
///   (`uobject_call_function` in LuaUObject.cpp). The C side just stashes
///   the active AHUD pointer during the dispatch; the Lua-side wrappers
///   do the actual `activeHUD:DrawText(...)` call. This keeps marshaling
///   logic in one place and inherits whatever PropertyMarshal already
///   handles.
///
///   The hook on `AHUD::DrawHUD` reuses `Hydro.Events.hook` - same
///   ProcessEvent inline-detour infrastructure, no duplication.
///
/// @depends EngineAPI (reflection); HydroEvents (UFunction hooks);
///   LuaUObject (colon-method dispatch + UObject userdata)
///

#include "HydroHUD.h"
#include "ModuleRegistry.h"
#include "../HydroCore.h"
#include "../EngineAPI.h"
#include "../LuaUObject.h"

extern "C" {
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>
}

namespace Hydro::API {

// -- Active-HUD context tracking --------------------------------------
//
// While an onDraw callback is running, `s_activeHUD` holds the AHUD
// pointer the engine handed us. The Lua wrappers use this so modder
// code can call `Hydro.HUD.drawText(x, y, text)` without explicitly
// passing the HUD around. Outside the dispatch the pointer is null and
// drawing calls become silent no-ops.
static void* s_activeHUD = nullptr;

// -- Hydro.HUD.getPlayerHUD -------------------------------------------

/// Return the local player's AHUD instance, or nil if not yet available.
///
/// Walks: Character (PlayerIndex 0) → Controller property → MyHUD
/// property. Each link reflects via `Engine::findProperty`; we don't
/// hardcode field offsets so this works across UE versions and host
/// engine forks. Any null link returns nil cleanly.
///
/// @returns AHUD - The local player's HUD, or nil if unavailable
static int l_hud_getPlayerHUD(lua_State* L) {
    void* character = Engine::getPlayerCharacter(0);
    if (!character) { Hydro::Lua::pushUObject(L, nullptr); return 1; }

    void* charClass = Engine::getObjClass(character);
    if (!charClass) { Hydro::Lua::pushUObject(L, nullptr); return 1; }

    void* controllerProp = Engine::findProperty(charClass, L"Controller");
    if (!controllerProp) { Hydro::Lua::pushUObject(L, nullptr); return 1; }
    int32_t ctrlOffset = Engine::getPropertyOffset(controllerProp);
    void* controller = nullptr;
    if (!Engine::readPtr((uint8_t*)character + ctrlOffset, &controller) || !controller) {
        Hydro::Lua::pushUObject(L, nullptr);
        return 1;
    }

    void* ctrlClass = Engine::getObjClass(controller);
    if (!ctrlClass) { Hydro::Lua::pushUObject(L, nullptr); return 1; }

    void* hudProp = Engine::findProperty(ctrlClass, L"MyHUD");
    if (!hudProp) { Hydro::Lua::pushUObject(L, nullptr); return 1; }
    int32_t hudOffset = Engine::getPropertyOffset(hudProp);
    void* hud = nullptr;
    Engine::readPtr((uint8_t*)controller + hudOffset, &hud);
    Hydro::Lua::pushUObject(L, hud);
    return 1;
}

// -- Hydro.HUD._ensureHUD (fallback for stripped hosts) ---------------
//
// Some games - notably DummyModdableGame at UE 5.6, and any minimal
// stripped UE5 cook - have `GameMode.HUDClass = nullptr`. Result:
// `PlayerController.MyHUD` stays null, the engine never calls DrawHUD,
// and our `ReceiveDrawHUD` hook never fires no matter how correctly it
// was installed. This isn't a bug in the hook - there's nothing to
// dispatch.
//
// Fallback: if MyHUD is null at the time the modder calls onDraw,
// spawn a fresh AHUD via the existing Engine::spawnActor (which goes
// through the engine's BeginDeferredActorSpawnFromClass UFunction, the
// same path Hydro.Assets.spawn uses) and assign it to MyHUD via direct
// reflected property write. The engine then renders it every frame
// just like a host-spawned HUD would.
//
// Real games (Palworld, SN2) that already have a HUD wired up land in
// the early-return branch and we touch nothing. This is a fallback,
// not a replacement.
//
// Returns true if MyHUD is now non-null (either was already, or we
// spawned one). Returns false if we couldn't find a player or spawn
// failed - caller can retry on a later tick.
// Throttle: log only on state transitions (failure → success or
// failure-reason-change), not every BeginPlay-triggered retry. UE 5.6 cooks
// fire hundreds of BeginPlays during level stream-in before the player
// spawns; without throttling we'd dump hundreds of identical lines.
static int s_lastFailureCode = 0;
enum EnsureFail : int { EF_NONE = 0, EF_NO_PAWN_OR_PC = 1, EF_NO_PC = 2,
                       EF_NO_HUD_PROP = 3, EF_NO_AHUD_CLASS = 4, EF_SPAWN_FAILED = 5 };
static void logFailOnce(int code, const char* msg) {
    if (s_lastFailureCode == code) return;
    s_lastFailureCode = code;
    Hydro::logInfo("[Hydro.HUD] ensureHUD: %s", msg);
}

// CDO check via UObject flags. RF_ClassDefaultObject = 0x10, stable
// across UE versions. Flags live at offset 0x18 in UObject (post-vtable
// + ObjectFlags + InternalIndex). We reuse the same offset HydroCore
// uses elsewhere via UOBJ_FLAGS but inline here to avoid pulling in
// EngineAPI internals.
static bool isCDO(void* obj) {
    if (!obj) return true;
    // UObject ObjectFlags live at +0x18 (stable across UE 5.x). Objects
    // we get here come from GUObjectArray-backed reflection helpers, so
    // the read is safe - Engine's findAllOf only returns live objects.
    uint32_t flags = *(uint32_t*)((uint8_t*)obj + 0x18);
    return (flags & 0x10) != 0;  // RF_ClassDefaultObject
}

// findFirstOf-style helper that skips the class-default-object. The
// stock findFirstOf walks GUObjectArray in index order and CDOs sit at
// low indices, so a naive findFirstOf("HUD") returns Default__HUD -
// useless for assignment to a live property.
static void* findFirstNonCDOInstanceOf(const wchar_t* className) {
    void* matches[16] = {};
    int n = Engine::findAllOf(className, matches, 16);
    for (int i = 0; i < n; i++) {
        if (matches[i] && !isCDO(matches[i])) return matches[i];
    }
    return nullptr;
}

// Find the local PlayerController via every reflection path we have.
// DMG-style stripped games may not have a Character subclass (just a
// Pawn), so getPlayerCharacter returns null. Try Pawn next; finally
// walk GUObjectArray for a non-CDO PlayerController instance.
static void* findPlayerController() {
    void* pawn = Engine::getPlayerCharacter(0);
    if (!pawn) pawn = Engine::getPlayerPawn(0);
    if (pawn) {
        void* pawnClass = Engine::getObjClass(pawn);
        if (pawnClass) {
            void* controllerProp = Engine::findProperty(pawnClass, L"Controller");
            if (controllerProp) {
                int32_t off = Engine::getPropertyOffset(controllerProp);
                void* pc = nullptr;
                if (Engine::readPtr((uint8_t*)pawn + off, &pc) && pc && !isCDO(pc)) return pc;
            }
        }
    }
    return findFirstNonCDOInstanceOf(L"PlayerController");
}

static bool ensureHUDInternal() {
    void* pc = findPlayerController();
    if (!pc) {
        logFailOnce(EF_NO_PAWN_OR_PC, "no player pawn or controller yet, will retry");
        return false;
    }

    void* pcClass = Engine::getObjClass(pc);
    if (!pcClass) {
        logFailOnce(EF_NO_PC, "PlayerController has no UClass (corrupt?)");
        return false;
    }

    void* hudProp = Engine::findProperty(pcClass, L"MyHUD");
    if (!hudProp) {
        logFailOnce(EF_NO_HUD_PROP, "MyHUD property not found on PlayerController class");
        return false;
    }
    int32_t hudOffset = Engine::getPropertyOffset(hudProp);

    void* existingHUD = nullptr;
    Engine::readPtr((uint8_t*)pc + hudOffset, &existingHUD);
    if (existingHUD) {
        std::string name = Engine::getObjectName(existingHUD);
        Hydro::logInfo("[Hydro.HUD] ensureHUD: MyHUD already wired (%p name='%s') - no action",
            existingHUD, name.c_str());
        s_lastFailureCode = EF_NONE;
        return true;
    }

    // Prefer reusing an existing AHUD instance over spawning a new one.
    // Stock UE's BeginDeferredActorSpawnFromClass refuses AHUD on UE 5.6
    // (returns null - empirically observed on DMG). Engines that
    // populate `GameMode.HUDClass` typically already spawn an AHUD that
    // ends up unowned-but-live in GUObjectArray. Wiring it to MyHUD
    // skips the spawn-restriction problem entirely.
    void* hudInstance = findFirstNonCDOInstanceOf(L"HUD");
    if (hudInstance) {
        std::string name = Engine::getObjectName(hudInstance);
        Hydro::logInfo("[Hydro.HUD] ensureHUD: reusing existing AHUD %p name='%s' for MyHUD",
            hudInstance, name.c_str());
    } else {
        // Last resort: try to spawn one. Real games (Palworld, SN2) that
        // wire MyHUD natively don't reach this path. DMG falls into the
        // existing-instance branch above. This is here as a sanity
        // fallback for hosts where neither code path triggers - even if
        // SpawnActor refuses AHUD, the failure is logged and we retry.
        Hydro::logInfo("[Hydro.HUD] ensureHUD: no existing AHUD instance, attempting spawn...");
        void* ahudClass = Engine::findObject(L"/Script/Engine.HUD");
        if (!ahudClass) {
            logFailOnce(EF_NO_AHUD_CLASS, "AHUD UClass (/Script/Engine.HUD) not found");
            return false;
        }
        hudInstance = Engine::spawnActor(ahudClass, 0.0, 0.0, 0.0);
        if (!hudInstance) {
            logFailOnce(EF_SPAWN_FAILED, "Both find-existing and SpawnActor failed for AHUD");
            return false;
        }
    }

    // Direct property write - UE's FObjectProperty layout is just a void*
    // at the property's offset. The GC will see the new pointer because
    // PlayerController is itself live in GUObjectArray.
    *(void**)((uint8_t*)pc + hudOffset) = hudInstance;
    Hydro::logInfo("[Hydro.HUD] ensureHUD: AHUD %p wired to PlayerController.MyHUD",
        hudInstance);
    s_lastFailureCode = EF_NONE;
    return true;
}

/// Lua-callable wrapper. Returns boolean: true if HUD is now wired,
/// false if we couldn't (caller can retry on a tick).
static int l_hud_ensureHUD(lua_State* L) {
    lua_pushboolean(L, ensureHUDInternal() ? 1 : 0);
    return 1;
}

// -- Hydro.HUD._getActiveHUD (internal) -------------------------------

/// Return the active AHUD pointer (set during an onDraw dispatch) as a
/// UObject userdata. Lua wrappers in `Hydro.HUD.drawText` etc. use this
/// to find the HUD they should invoke `DrawText` on. Outside an onDraw
/// fire, returns nil.
static int l_hud_getActiveHUD(lua_State* L) {
    Hydro::Lua::pushUObject(L, s_activeHUD);
    return 1;
}

// -- Hydro.HUD._dispatchDraw (internal) -------------------------------

/// Dispatcher fired by Hydro.Events.hook on AHUD::DrawHUD. Sets the
/// active-HUD pointer, calls the user's stored Lua callback, then
/// clears the pointer so post-draw `drawText` calls become no-ops.
///
/// Bound from Lua bootstrap as a closure with the user's onDraw
/// callback as upvalue 1. Receives the AHUD instance as Lua arg 1
/// (HydroEvents pushes the calling actor as the first hook arg).
static int l_hud_dispatchDraw(lua_State* L) {
    void* prevHUD = s_activeHUD;
    s_activeHUD = Hydro::Lua::checkUObject(L, 1);

    // First-fire diagnostic: confirms the ReceiveDrawHUD hook actually
    // reaches us. If this never logs, the host game's GameMode has
    // HUDClass=nullptr (no AHUD spawned, ReceiveDrawHUD never fires)
    // and we need a different draw entry point.
    static bool s_firstFireLogged = false;
    if (!s_firstFireLogged) {
        s_firstFireLogged = true;
        Hydro::logInfo("[Hydro.HUD] _dispatchDraw FIRST FIRE - ahud=%p", s_activeHUD);
    }

    // Invoke the user's callback (upvalue 1). No args - the modder uses
    // the `Hydro.HUD.draw*` wrappers which read s_activeHUD implicitly.
    lua_pushvalue(L, lua_upvalueindex(1));
    if (lua_pcall(L, 0, 0, 0) != 0) {
        Hydro::logError("Hydro.HUD.onDraw callback error: %s",
            lua_tostring(L, -1));
        lua_pop(L, 1);
    }

    s_activeHUD = prevHUD;
    return 0;
}

// -- Hydro.HUD.onDraw -------------------------------------------------
//
// Implemented as a Lua wrapper over Hydro.Events.hook - keeps all hook
// state ownership inside Hydro.Events (one dispatch path, one
// ProcessEvent inline hook). The wrapper is installed during module
// registration via luaL_dostring.
//
//   Hydro.HUD.onDraw = function(fn)
//       return Hydro.Events.hook(
//           "/Script/Engine.HUD:ReceiveDrawHUD",
//           function(ahud)
//               -- _dispatchDraw is a C function with `fn` as upvalue
//           end)
//   end
//
// We use `ReceiveDrawHUD` (the BlueprintImplementableEvent) rather
// than the C++ `DrawHUD` because BlueprintImplementableEvent is the
// one that fires for BP-overridden HUDs in cooked games (the C++
// DrawHUD calls into K2_OnDrawHUD which is the BP entry point).
// Native HUDs that don't override BP still receive DrawHUD; for those
// we additionally hook the path the user's game actually uses. For
// v1, ReceiveDrawHUD covers Palworld + DMG + most UE5 games.

// HUD is passed in as varargs[1] because at bootstrap-run time, the HUD
// module hasn't been stored in package.loaded yet (registerModule does that
// AFTER the initializer returns). Events IS already loaded - registered in
// dllmain.cpp before HUD - so require("Hydro.Events") works here.
static const char* HUD_LUA_BOOTSTRAP = R"LUA(
local HUD = ...
local Events = require("Hydro.Events")

-- AHUD's per-frame BlueprintImplementableEvent that's invoked from the
-- native AHUD::DrawHUD pass. The native DrawHUD itself is a virtual C++
-- method, NOT a UFunction, so it can't be ProcessEvent-hooked - only the
-- BIE entry can. ReceiveDrawHUD fires once per frame for every cooked
-- UE5 game that has an HUD.
local DRAW_HOOK_PATH = "/Script/Engine.HUD:ReceiveDrawHUD"

function HUD.onDraw(fn)
    if type(fn) ~= "function" then
        error("Hydro.HUD.onDraw expects a function", 2)
    end

    -- Fallback for stripped-HUD hosts (DMG-style): if MyHUD isn't wired,
    -- spawn one ourselves so the engine actually dispatches DrawHUD →
    -- ReceiveDrawHUD every frame. Real games already have a HUD and
    -- _ensureHUD becomes a no-op for them.
    --
    -- If the player isn't loaded yet at modder-init time, retry on each
    -- ReceiveBeginPlay (which fires reliably as actors stream in) until
    -- we successfully wire one. After that, we stop retrying.
    if not HUD._ensureHUD() then
        local retried = false
        Events.hook("/Script/Engine.Actor:ReceiveBeginPlay", function()
            if retried then return end
            if HUD._ensureHUD() then retried = true end
        end)
    end

    -- _dispatchDraw is a C closure factory: passing `fn` as the only
    -- upvalue gives us a per-callback dispatcher that bridges
    -- HydroEvents' (ahud) → user's () shape.
    local ok, err = pcall(function()
        Events.hook(DRAW_HOOK_PATH, function(ahud) HUD._dispatchDraw(ahud, fn) end)
    end)
    if not ok then
        -- Don't let a missing UFunction abort the modder's init script.
        -- Surface the failure so they know their HUD callback won't fire.
        error("Hydro.HUD.onDraw: failed to hook " .. DRAW_HOOK_PATH .. ": " .. tostring(err), 2)
    end
    return true
end

function HUD.drawText(x, y, text, color, scale)
    local hud = HUD._getActiveHUD()
    if not hud then return end
    color = color or { r = 1, g = 1, b = 1, a = 1 }
    scale = scale or 1.0
    -- AHUD::DrawText(Text, TextColor, ScreenX, ScreenY, Font, Scale, bScalePosition)
    -- Routes through the UObject colon-method machinery, which uses
    -- PropertyMarshal to fill params from these Lua args.
    pcall(function()
        hud:DrawText(text, color, x, y, nil, scale, false)
    end)
end

function HUD.drawTexture(texture, x, y, w, h, color)
    local hud = HUD._getActiveHUD()
    if not hud or not texture then return end
    color = color or { r = 1, g = 1, b = 1, a = 1 }
    -- AHUD::DrawTextureSimple(Texture, ScreenX, ScreenY, Scale, bScalePosition)
    -- (DrawTextureSimple ignores w/h; pass-through preserves API symmetry.)
    pcall(function()
        hud:DrawTextureSimple(texture, x, y, 1.0, false)
    end)
end
)LUA";

// _dispatchDraw is exposed as a regular module function but the Lua
// bootstrap wraps it in a closure with the user's fn as upvalue. The
// bootstrap calls it as `HUD._dispatchDraw(ahud, fn)`; we forward to
// the closure-friendly `l_hud_dispatchDraw` shape above.
static int l_hud_dispatchDrawTrampoline(lua_State* L) {
    // Stack: [ahud, fn]
    if (lua_gettop(L) < 2) return 0;
    // Capture fn as the closure upvalue, push ahud, call dispatcher.
    lua_pushvalue(L, 2);
    lua_pushcclosure(L, l_hud_dispatchDraw, 1);
    lua_pushvalue(L, 1);                  // ahud
    if (lua_pcall(L, 1, 0, 0) != 0) {
        Hydro::logError("Hydro.HUD._dispatchDraw error: %s",
            lua_tostring(L, -1));
        lua_pop(L, 1);
    }
    return 0;
}

// -- Module registration ----------------------------------------------

static const luaL_Reg hud_functions[] = {
    {"getPlayerHUD",   l_hud_getPlayerHUD},
    {"_ensureHUD",     l_hud_ensureHUD},
    {"_getActiveHUD",  l_hud_getActiveHUD},
    {"_dispatchDraw",  l_hud_dispatchDrawTrampoline},
    {nullptr,          nullptr},
};

// Module-register diagnostic. Mostly useful for the case where the host has
// `GameMode.HUDClass = nullptr` and no AHUD ever spawns. findAllOf returns up
// to N matches; we walk them and log name + class so we can tell whether
// what we found is just the CDO (Default__HUD) vs an actual gameplay
// instance. CDO doesn't run ReceiveDrawHUD; only spawned instances do.
static void diagAHUDPresence() {
    void* matches[16] = {};
    int n = Engine::findAllOf(L"HUD", matches, 16);
    if (n == 0) {
        Hydro::logWarn("[Hydro.HUD] DIAG: NO AHUD instance in GUObjectArray (host has no HUD; need to spawn one)");
        return;
    }
    Hydro::logInfo("[Hydro.HUD] DIAG: %d AHUD-derived object(s) in GUObjectArray:", n);
    for (int i = 0; i < n; i++) {
        std::string name = Engine::getObjectName(matches[i]);
        void* cls = Engine::getClass(matches[i]);
        std::string clsName = cls ? Engine::getObjectName(cls) : std::string("?");
        Hydro::logInfo("[Hydro.HUD] DIAG:   [%d] %p name='%s' class='%s'",
            i, matches[i], name.c_str(), clsName.c_str());
    }
}

void registerHUDModule(lua_State* L) {
    buildModuleTable(L, hud_functions);
    diagAHUDPresence();
    // Stack: [moduleTable]

    // Compile bootstrap as a function and call it with the module table as
    // varargs[1]. We can't `require("Hydro.HUD")` from inside the bootstrap
    // because registerModule writes to package.loaded AFTER this initializer
    // returns - so the bootstrap receives the table directly instead.
    if (luaL_loadstring(L, HUD_LUA_BOOTSTRAP) != 0) {
        Hydro::logError("Hydro.HUD: bootstrap compile failed: %s",
            lua_tostring(L, -1));
        lua_pop(L, 1);
        return;
    }
    // Stack: [moduleTable, bootstrapFn]
    lua_pushvalue(L, -2);  // duplicate moduleTable as bootstrap arg
    // Stack: [moduleTable, bootstrapFn, moduleTable]
    if (lua_pcall(L, 1, 0, 0) != 0) {
        Hydro::logError("Hydro.HUD: bootstrap exec failed: %s",
            lua_tostring(L, -1));
        lua_pop(L, 1);
    }
    // Stack: [moduleTable] - registerModule expects this on top.
}

} // namespace Hydro::API
