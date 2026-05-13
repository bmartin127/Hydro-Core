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
#include "../engine/Layout.h"
#include "../engine/Internal.h"

extern "C" {
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>
}

#include <string>
#include <cstdio>

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

    void* tmpl = nullptr;
    if (lua_gettop(L) >= 3 && !lua_isnil(L, 3)) {
        tmpl = Lua::checkUObject(L, 3);
    }

    void* result = tmpl
        ? Engine::staticConstructObjectWithTemplate(uclass, outer, tmpl)
        : Engine::staticConstructObject(uclass, outer);
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

// -- Hydro.UI.duplicateObject -----------------------------------------

/// Deep-copy a UObject (and the subobject graph it owns) via the
/// engine's `StaticDuplicateObject` - archive-based duplication.
///
/// Use case: cooked UE 5.6 WBPs whose `widget.WidgetTree.RootWidget`
/// comes out nil after `Create()` because the standard UMG init path
/// uses NewObject-with-template + an InstancingGraph filter that skips
/// `UWidgetTree::RootWidget` (no `CPF_InstancedReference`).
/// `StaticDuplicateObject` copies properties via memory-archive
/// serialization, which writes every UPROPERTY regardless of flags -
/// recovering the full tree.
///
/// Typical usage in Lua:
///   local widget = libCDO:Create(world, cls, nil)
///   widget.WidgetTree = UI.duplicateObject(cls.WidgetTree, widget)
///   widget:AddToViewport(0)
///
/// @param source UObject - The object to duplicate (e.g. the class's
///   WidgetTree)
/// @param outer UObject - The duplicate's Outer (e.g. the widget
///   instance receiving the new tree)
/// @returns UObject - The duplicated object, or nil on failure
static int l_ui_duplicateObject(lua_State* L) {
    void* source = Lua::checkUObject(L, 1);
    void* outer  = Lua::checkUObject(L, 2);
    void* dup    = Engine::staticDuplicateObject(source, outer);
    Lua::pushUObject(L, dup);
    return 1;
}

// -- Hydro.UI.duplicateAndInitializeWidgetTree ------------------------

/// Invoke UUserWidget::DuplicateAndInitializeFromWidgetTree directly to
/// bypass UE 5.6's broken cooked-WBP init path. UE 5.6's
/// `InitializeWidgetStatic` skips its D&IFWT call when `widget.WidgetTree`
/// is non-null on entry - which it always is in shipping monolithic
/// builds, leaving the instance tree empty.
///
/// Typical usage:
///   local widget = libCDO:Create(world, cls, nil)
///   widget.WidgetTree = nil  -- null the broken/stub tree
///   UI.duplicateAndInitializeWidgetTree(widget, cls.WidgetTree)
///   widget:AddToViewport(0)
///
/// @param widget UObject - The UUserWidget instance to fix.
/// @param srcWidgetTree UObject - The class archetype WidgetTree
///   (typically `cls.WidgetTree` where `cls` is the WBP generated class).
/// @returns boolean - true if the call returned normally.
static int l_ui_duplicateAndInitializeWidgetTree(lua_State* L) {
    void* widget = Lua::checkUObject(L, 1);
    void* src    = Lua::checkUObject(L, 2);
    bool ok = Engine::duplicateAndInitializeWidgetTree(widget, src);
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

// -- Hydro.UI.dumpProperties ------------------------------------------

/// Walk the class chain (object → its UClass → SuperClass → ...) and dump
/// every FProperty's name + flag bits + offset + size. Used as a runtime
/// ground-truth probe: source-code citations claim `UWidgetTree::RootWidget`
/// has `CPF_InstancedReference` (0x80000) set, but we've never verified
/// that on the actual binary. This dump tells us.
///
/// Output format (Lua-side):
///   {
///     class_name = "WidgetTree",
///     properties = {
///       { name = "RootWidget", flags = "0x...", offset = N, size = N },
///       ...
///     }
///   }
///
/// @param obj UObject - The object whose class chain to walk
/// @returns table with class hierarchy + property tables, or nil on failure
static int l_ui_dumpProperties(lua_State* L) {
    void* obj = Lua::checkUObject(L, 1);
    if (!obj) { lua_pushnil(L); return 1; }

    auto& layout = Engine::s_layout;
    if (!layout.succeeded) {
        Hydro::logWarn("Hydro.UI.dumpProperties: property layout not discovered");
        lua_pushnil(L);
        return 1;
    }

    // Read object's class (UObject::ClassPrivate at +0x10 on all UE 5.x).
    void* cls = nullptr;
    if (!Engine::safeReadPtr((uint8_t*)obj + 0x10, &cls) || !cls) {
        lua_pushnil(L);
        return 1;
    }

    // Build a Lua array - one entry per class in the chain, each with
    // its properties.
    lua_newtable(L);
    int classIdx = 1;

    while (cls) {
        // ChildProperties at +childPropsOffset
        void* fprop = nullptr;
        if (!Engine::safeReadPtr((uint8_t*)cls + layout.childPropsOffset, &fprop)) break;

        lua_newtable(L);
        std::string clsName = Engine::getObjectName(cls);
        lua_pushstring(L, "class");
        lua_pushstring(L, clsName.c_str());
        lua_settable(L, -3);

        lua_pushstring(L, "properties");
        lua_newtable(L);
        int propIdx = 1;

        while (fprop) {
            // Property name (FName at +fieldNameOffset, FName.ComparisonIndex is uint32)
            uint32_t nameIdx = 0;
            if (!Engine::safeReadInt32((uint8_t*)fprop + layout.fieldNameOffset, (int32_t*)&nameIdx)) break;
            std::string propName = Engine::getNameString(nameIdx);

            // PropertyFlags (uint64 at +flags)
            uint64_t propFlags = 0;
            Engine::safeReadU64((uint8_t*)fprop + layout.flags, &propFlags);

            // Offset_Internal (int32 at +offsetInternal)
            int32_t propOffset = 0;
            Engine::safeReadInt32((uint8_t*)fprop + layout.offsetInternal, &propOffset);

            // ElementSize (int32 at +elementSize)
            int32_t propSize = 0;
            Engine::safeReadInt32((uint8_t*)fprop + layout.elementSize, &propSize);

            // Build the per-property table
            lua_newtable(L);
            lua_pushstring(L, "name");
            lua_pushstring(L, propName.c_str());
            lua_settable(L, -3);

            // flags as hex string (Lua numbers can't hold full uint64)
            char flagsStr[32];
            std::snprintf(flagsStr, sizeof(flagsStr), "0x%016llX",
                (unsigned long long)propFlags);
            lua_pushstring(L, "flags");
            lua_pushstring(L, flagsStr);
            lua_settable(L, -3);

            lua_pushstring(L, "offset");
            lua_pushnumber(L, (double)propOffset);
            lua_settable(L, -3);

            lua_pushstring(L, "size");
            lua_pushnumber(L, (double)propSize);
            lua_settable(L, -3);

            lua_rawseti(L, -2, propIdx++);

            // Next FProperty in chain
            void* nextProp = nullptr;
            if (!Engine::safeReadPtr((uint8_t*)fprop + layout.fieldNextOffset, &nextProp)) break;
            fprop = nextProp;
        }
        lua_settable(L, -3);  // properties

        lua_rawseti(L, -2, classIdx++);

        // Walk to SuperStruct (next class in chain)
        cls = Engine::getSuper(cls);
    }

    return 1;
}

// -- Hydro.UI.readPointer ---------------------------------------------

/// Read 8 raw bytes at `obj + offset` as a UObject pointer. Used to
/// disambiguate "Lua reflection reads the right field offset" vs "C++
/// direct member access reads from a different baked offset."
static int l_ui_readPointer(lua_State* L) {
    void* obj = Lua::checkUObject(L, 1);
    int offset = (int)luaL_checkinteger(L, 2);
    if (!obj) { lua_pushnil(L); return 1; }
    void* val = nullptr;
    Engine::safeReadPtr((uint8_t*)obj + offset, &val);
    Lua::pushUObject(L, val);
    return 1;
}

// -- Hydro.UI.getSuperClass -------------------------------------------

/// Walk one step up a UClass's super chain via the runtime SuperStruct
/// offset. Returns nil if the class is at the root (UObject) or super
/// offset wasn't discovered.
static int l_ui_getSuperClass(lua_State* L) {
    void* cls = Lua::checkUObject(L, 1);
    if (!cls) { lua_pushnil(L); return 1; }
    void* super = Engine::getSuper(cls);
    Lua::pushUObject(L, super);
    return 1;
}

// -- Hydro.UI.getClassFlags -------------------------------------------

/// Read EClassFlags (CLASS_*) on a UClass. Returns the raw uint32.
static int l_ui_getClassFlags(lua_State* L) {
    void* cls = Lua::checkUObject(L, 1);
    if (!cls) { lua_pushnil(L); return 1; }
    uint32_t flags = Engine::getClassFlags(cls);
    char buf[20]; std::snprintf(buf, sizeof(buf), "0x%08X", flags);
    lua_pushstring(L, buf);
    return 1;
}

// -- Hydro.UI.getObjectFlags ------------------------------------------

/// Read EObjectFlags (RF_*) on a UObject. Returns the raw uint32.
static int l_ui_getObjectFlags(lua_State* L) {
    void* obj = Lua::checkUObject(L, 1);
    if (!obj) { lua_pushnil(L); return 1; }
    int32_t flags = 0;
    Engine::safeReadInt32((uint8_t*)obj + 0x18, &flags);
    char buf[20]; std::snprintf(buf, sizeof(buf), "0x%08X", flags);
    lua_pushstring(L, buf);
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
    {"newObject",       l_ui_newObject},
    {"findObject",      l_ui_findObject},
    {"setText",         l_ui_setText},
    {"duplicateObject", l_ui_duplicateObject},
    {"duplicateAndInitializeWidgetTree", l_ui_duplicateAndInitializeWidgetTree},
    {"dumpProperties",  l_ui_dumpProperties},
    {"readPointer",     l_ui_readPointer},
    {"getSuperClass",   l_ui_getSuperClass},
    {"getClassFlags",   l_ui_getClassFlags},
    {"getObjectFlags",  l_ui_getObjectFlags},
    {"stripAbstract",   l_ui_stripAbstract},
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
