///
/// @module Hydro.UI
/// @description Runtime UMG widget construction. The C bindings expose
///   primitives (newObject, setText) that need engine internals (SCO,
///   GMalloc, FText conversion). The high-level builder `simpleText`
///   lives in the Lua bootstrap below - modders who want richer trees
///   compose the same primitives directly from their own Lua.
///
///   API:
///     Hydro.UI.newObject(classOrPath, outer?)  -> UObject
///     Hydro.UI.setText(textBlock, str)         -> bool
///     Hydro.UI.stripAbstract(class)            -> uint32 (new flags)
///     Hydro.UI.simpleText(text, world?)        -> textBlock UObject
///                                              (assembled UMG tree
///                                              already AddToViewport'd;
///                                              call setText to update)
///
/// @depends EngineAPI (staticConstructObject, setWidgetText,
///                     modifyClassFlags, findObject)
///

#include "HydroUI.h"
#include "ModuleRegistry.h"
#include "../HydroCore.h"
#include "../EngineAPI.h"
#include "../LuaUObject.h"

extern "C" {
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>
}

#include <string>

namespace Hydro::API {

static std::wstring toWide(const char* str) {
    return std::wstring(str, str + strlen(str));
}

// -- Hydro.UI.newObject -----------------------------------------------

/// Construct a UObject of the given class via the engine's NewObject
/// primitive (StaticConstructObject_Internal).
///
/// @param classOrPath string|UObject - UClass userdata, or a /Script/...
///   path string that findObject will resolve.
/// @param outer? UObject - The new object's Outer. Defaults to nil
///   (engine uses GetTransientPackage()). For UMG widgets, prefer a
///   UWorld / UGameInstance / live actor so the widget chain can find
///   its rendering context.
/// @returns UObject - The constructed object, or nil on failure
static int l_ui_newObject(lua_State* L) {
    void* uclass = nullptr;
    if (lua_isstring(L, 1)) {
        const char* path = lua_tostring(L, 1);
        uclass = Engine::findObject(toWide(path).c_str());
        if (!uclass) {
            Hydro::logWarn("Hydro.UI.newObject: class not found: %s", path);
            Lua::pushUObject(L, nullptr);
            return 1;
        }
    } else {
        uclass = Lua::checkUObject(L, 1);
    }

    void* outer = nullptr;
    if (lua_gettop(L) >= 2 && !lua_isnil(L, 2)) {
        outer = Lua::checkUObject(L, 2);
    }

    void* result = Engine::staticConstructObject(uclass, outer);
    Lua::pushUObject(L, result);
    return 1;
}

// -- Hydro.UI.setText -------------------------------------------------

/// Set the text on a UMG text-bearing widget. Routes through
/// `UKismetTextLibrary::Conv_StringToText` to build the FText, then
/// invokes the widget's reflected `SetText(FText)` UFunction.
///
/// @param widget UObject - A UTextBlock / URichTextBlock / UEditableText etc.
/// @param text string - The string to set
/// @returns boolean - true on success, false if reflection fails
static int l_ui_setText(lua_State* L) {
    void* widget = Lua::checkUObject(L, 1);
    const char* s = luaL_checkstring(L, 2);
    if (!widget || !s) { lua_pushboolean(L, 0); return 1; }
    std::wstring wide = toWide(s);
    bool ok = Engine::setWidgetText(widget, wide.c_str());
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

// -- Hydro.UI.findObject ----------------------------------------------

/// Look up an in-memory UObject (UClass, CDO, or other live object) by
/// its full path. Wraps `Engine::findObject`. Useful for grabbing UClass
/// pointers (`/Script/UMG.UserWidget`) and Default Object instances
/// (`/Script/UMG.Default__WidgetBlueprintLibrary`) without doing a full
/// SCO-construct + discard dance.
///
/// @param path string - Object path (e.g., "/Script/UMG.UserWidget")
/// @returns UObject - The object, or nil if not loaded
static int l_ui_findObject(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    void* obj = Engine::findObject(toWide(path).c_str());
    Lua::pushUObject(L, obj);
    return 1;
}

// -- Hydro.UI.stripAbstract -------------------------------------------

/// Clear `CLASS_Abstract` (0x1) on the given UClass. Required ONLY if
/// the caller wants to use `UWidgetBlueprintLibrary::Create` to
/// construct an abstract base like UUserWidget - `Create` rejects
/// abstract classes. The raw `Hydro.UI.newObject` path doesn't need
/// this because `StaticConstructObject_Internal` has no Abstract check.
///
/// @param uclass UObject - The UClass to modify
/// @returns number - The new ClassFlags value (or 0 if write failed)
static int l_ui_stripAbstract(lua_State* L) {
    void* uclass = Lua::checkUObject(L, 1);
    constexpr uint32_t CLASS_Abstract = 0x00000001;
    uint32_t newFlags = Engine::modifyClassFlags(uclass, 0, CLASS_Abstract);
    lua_pushnumber(L, (double)newFlags);
    return 1;
}

// -- Module registration ----------------------------------------------

static const luaL_Reg ui_functions[] = {
    {"newObject",     l_ui_newObject},
    {"findObject",    l_ui_findObject},
    {"setText",       l_ui_setText},
    {"stripAbstract", l_ui_stripAbstract},
    {nullptr,         nullptr},
};

// High-level builder lives in Lua so modders can read the recipe and
// adapt it for richer trees. The four reflective property writes and
// two UFunction calls map 1:1 to UE's runtime UMG construction order.
//
// The receiver-argument trick (HUD pattern) - pass module table as
// varargs[1] because registerModule writes to package.loaded AFTER
// the bootstrap returns; the table isn't requirable yet from inside.
static const char* UI_LUA_BOOTSTRAP = R"LUA(
local UI = ...
local World = require("Hydro.World")

-- -- Hydro.UI.fromWBP ------------------------------------------------
--
-- Load a cooked Widget Blueprint by /Game/-style class path and instantiate
-- it via UWidgetBlueprintLibrary::Create. The cooked WBP is a
-- UWidgetBlueprintGeneratedClass (concrete, non-abstract, fully wired
-- WidgetTree CDO + Initialize machinery) so Create returns a renderable
-- widget. AddToViewport runs synchronously here so callers can immediately
-- :GetWidgetFromName() child widgets and :SetText() them.
--
-- Path shape: "/Game/Mods/<mod>/WBP_X.WBP_X_C". The trailing `_C` is the
-- generated class object name (UE convention - the .uasset stores both the
-- WBP itself and its generated class with `_C` suffix).
--
-- Returns the live UUserWidget on success. Throws (caller can pcall) on
-- any failure: Assets.load returned nil, WidgetBlueprintLibrary CDO not
-- loaded, Create returned nil, or AddToViewport faulted.
function UI.fromWBP(path, world)
    world = world or World.getWorld()
    if not world then
        error("Hydro.UI.fromWBP: no UWorld available", 2)
    end

    local cls = require("Hydro.Assets").load(path)
    if not cls then
        error("Hydro.UI.fromWBP: Assets.load returned nil for " .. tostring(path), 2)
    end

    local libCDO = UI.findObject("/Script/UMG.Default__WidgetBlueprintLibrary")
    if not libCDO then
        error("Hydro.UI.fromWBP: WidgetBlueprintLibrary CDO not loaded", 2)
    end

    local widget = libCDO:Create(world, cls, nil)
    if not widget then
        error("Hydro.UI.fromWBP: Create returned nil for " .. tostring(path), 2)
    end

    widget:AddToViewport(0)
    return widget
end

-- Construct a single-text UMG overlay anchored at top-left, AddToViewport
-- it, and return the created UTextBlock so the caller can :SetText() it
-- per frame. World defaults to the current UWorld when omitted.
--
-- Tree shape:
--   UUserWidget
--     +-- WidgetTree (UWidgetTree)
--         +-- RootWidget = UCanvasPanel
--                          +-- UTextBlock  (anchored top-left)
--
-- The CanvasPanel slot defaults to (0,0) origin and zero size; we set
-- a generous Size so the TextBlock has room to render. Anchors stay
-- (0,0) which means top-left in viewport space.
function UI.simpleText(text, world)
    world = world or World.getWorld()
    if not world then
        error("Hydro.UI.simpleText: no UWorld available", 2)
    end

    -- UUserWidget construction via UWidgetBlueprintLibrary::Create. Raw
    -- StaticConstructObject gives a UObject but Initialize() never runs,
    -- so AddToViewport silently returns SNullWidget and nothing renders.
    -- Create() internally does NewObject + Initialize + SetOwningPlayer
    -- in one BP-callable UFunction.
    --
    -- Wrinkle: Create() rejects classes with CLASS_Abstract, and
    -- UUserWidget::StaticClass() IS abstract. We clear the flag in place
    -- - one-time UClass mutation, persists for the rest of the process.
    -- SCO has no Abstract check, only Create does.
    local userWidgetCls = UI.findObject("/Script/UMG.UserWidget")
    if not userWidgetCls then
        error("Hydro.UI.simpleText: UUserWidget UClass not loaded", 2)
    end
    UI.stripAbstract(userWidgetCls)

    local libCDO = UI.findObject("/Script/UMG.Default__WidgetBlueprintLibrary")
    if not libCDO then
        error("Hydro.UI.simpleText: WidgetBlueprintLibrary CDO not loaded", 2)
    end

    local userWidget = libCDO:Create(world, userWidgetCls, nil)
    print("[Hydro.UI.simpleText] Create returned: " .. tostring(userWidget))
    if not userWidget then
        error("Hydro.UI.simpleText: Create returned nil", 2)
    end

    -- Create() initializes the widget. WidgetTree is auto-populated when
    -- the widget's class is a generated class; for the bare UUserWidget
    -- it might still be nil - fall back to NewObject + assign.
    local ok_t, tree = pcall(function() return userWidget.WidgetTree end)
    print("[Hydro.UI.simpleText] post-Create WidgetTree = " .. tostring(tree))
    if not ok_t or not tree then
        tree = UI.newObject("/Script/UMG.WidgetTree", userWidget)
        if not tree then error("Hydro.UI.simpleText: WidgetTree NewObject failed", 2) end
        userWidget.WidgetTree = tree
    end

    local tb = UI.newObject("/Script/UMG.TextBlock", tree)
    if not tb then error("Hydro.UI.simpleText: TextBlock NewObject failed", 2) end
    tree.RootWidget = tb

    -- Belt-and-suspenders visibility - should default to Visible (=0) but
    -- a runtime-NewObject'd widget without an Initialize() pass may have
    -- stale state. ESlateVisibility::Visible = 0.
    pcall(function() tb:SetVisibility(0) end)
    pcall(function() userWidget:SetVisibility(0) end)

    -- Initial text via the Conv_StringToText path (FText shim in C).
    local set_ok = UI.setText(tb, text or "")
    print("[Hydro.UI.simpleText] setText ok=" .. tostring(set_ok))

    -- AddToViewport. zorder=0 (under PIE/console UI but above game world).
    local ok2, err = pcall(function() userWidget:AddToViewport(0) end)
    if not ok2 then
        print("[Hydro.UI.simpleText] AddToViewport failed: " .. tostring(err))
    else
        print("[Hydro.UI.simpleText] AddToViewport ok")
    end

    -- Diag: read back the wired tree state
    local ok3, rw = pcall(function() return tree.RootWidget end)
    print("[Hydro.UI.simpleText] DIAG tree.RootWidget=" .. tostring(rw) .. " ok=" .. tostring(ok3))
    local ok4, wt = pcall(function() return userWidget.WidgetTree end)
    print("[Hydro.UI.simpleText] DIAG userWidget.WidgetTree=" .. tostring(wt) .. " ok=" .. tostring(ok4))

    return tb
end
)LUA";

void registerUIModule(lua_State* L) {
    buildModuleTable(L, ui_functions);
    // Stack: [moduleTable]

    if (luaL_loadstring(L, UI_LUA_BOOTSTRAP) != 0) {
        Hydro::logError("Hydro.UI: bootstrap compile failed: %s",
            lua_tostring(L, -1));
        lua_pop(L, 1);
        return;
    }
    // Stack: [moduleTable, bootstrapFn]
    lua_pushvalue(L, -2);
    // Stack: [moduleTable, bootstrapFn, moduleTable]
    if (lua_pcall(L, 1, 0, 0) != 0) {
        Hydro::logError("Hydro.UI: bootstrap exec failed: %s",
            lua_tostring(L, -1));
        lua_pop(L, 1);
    }
    // Stack: [moduleTable]
}

} // namespace Hydro::API
