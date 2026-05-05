///
/// @module Hydro.Assets
/// @description Load, find, spawn, and destroy game assets and actors.
///   This is the primary interface for getting content from pak files into
///   the game world. All returned objects are reflection-enabled UObjects.
///
///   NOTE: This is a generic API. If a specific Tier 2 module exists for
///   your use case, prefer it:
///   - Adding creatures? Use Hydro.SN2.Creatures.register() instead.
///   - Adding items? Use Hydro.SN2.Items.register() instead.
///   Specific APIs enable conflict detection, multiplayer sync, and
///   survive game updates.
///
/// @depends EngineAPI (StaticLoadObject, AssetRegistry, ProcessEvent)
/// @engine_systems AssetRegistry, GameplayStatics, UWorld
///

#include "HydroAssets.h"
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

// Helpers

/// Convert a narrow C string to a wide string for EngineAPI calls.
static std::wstring toWide(const char* str) {
    return std::wstring(str, str + strlen(str));
}

// Assets.load

/// Load a Blueprint class or asset from a mounted pak file.
///
/// Uses the AssetRegistry to locate and load the asset. The pak containing
/// the asset must already be mounted (done automatically for mods listed
/// in hydro_mods.json).
///
/// @param assetPath string - Full asset path (e.g., "/Game/Mods/MyMod/BP_Fish.BP_Fish_C")
/// @returns UObject - The loaded asset, or nil if not found
/// @throws string - If assetPath is not a string
/// @engine AssetRegistryHelpers::GetAsset
/// @example
/// local Assets = require("Hydro.Assets")
/// local fishClass = Assets.load("/Game/Mods/MyMod/BP_Fish.BP_Fish_C")
/// if fishClass then
///     print("Loaded: " .. tostring(fishClass))
/// end
static int l_assets_load(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    void* asset = Engine::loadAsset(toWide(path).c_str());
    Lua::pushUObject(L, asset);
    return 1;
}

// Assets.find

/// Find an already-loaded UObject by its full path.
///
/// Unlike load(), this only finds objects that are already in memory.
/// Use this for engine built-in classes (e.g., "/Script/Engine.PointLight")
/// or objects loaded by other mods.
///
/// @param objectPath string - Full object path (e.g., "/Script/Engine.Actor")
/// @returns UObject - The found object, or nil if not in memory
/// @throws string - If objectPath is not a string
/// @engine StaticFindObject
/// @example
/// local Assets = require("Hydro.Assets")
/// local actorClass = Assets.find("/Script/Engine.Actor")
/// if actorClass then
///     print("Found engine Actor class")
/// end
static int l_assets_find(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    void* obj = Engine::findObject(toWide(path).c_str());
    Lua::pushUObject(L, obj);
    return 1;
}

// Assets.spawn

/// Load a Blueprint class and spawn it as an actor in the world.
///
/// This is a convenience function that combines load() + engine spawn.
/// The actor goes through the full UE lifecycle: construction script,
/// component registration, and BeginPlay. The returned UObject supports
/// full reflection - read/write properties, call functions.
///
/// @param classPath string - Blueprint class path to spawn
/// @param x number? - World X coordinate (default: 0)
/// @param y number? - World Y coordinate (default: 0)
/// @param z number? - World Z coordinate (default: 0)
/// @returns UObject - The spawned actor with reflection, or nil on failure
/// @throws string - If classPath is not a string
/// @engine GameplayStatics::BeginDeferredActorSpawnFromClass
/// @engine GameplayStatics::FinishSpawningActor
/// @example
/// local Assets = require("Hydro.Assets")
/// local cube = Assets.spawn("/Game/Mods/MyMod/BP_Cube.BP_Cube_C", 100, 0, 50)
/// if cube then
///     print("Spawned at address: " .. cube:GetAddress())
/// end
static int l_assets_spawn(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    double x = luaL_optnumber(L, 2, 0.0);
    double y = luaL_optnumber(L, 3, 0.0);
    double z = luaL_optnumber(L, 4, 0.0);

    std::wstring widePath = toWide(path);

    // Three-tier lookup to handle every asset origin:
    //   1. AssetRegistry::GetAsset - fast, works for assets the game was
    //      packaged with (asset registry knows about them at engine init).
    //   2. StaticFindObject - catches already-loaded packages the registry
    //      might not have cached but that are live in memory.
    //   3. StaticLoadObject - actively loads the package from disk,
    //      including from runtime-mounted mod paks whose contents were
    //      never in the original game's AssetRegistry. This is the tier
    //      that makes runtime mod content-loading actually work.
    // Validate each tier: on hosts where the UFunction layout discovery
    // misfires (e.g. Palworld's forked engine), `loadAsset` can return a
    // bogus value (an FName index packed as void*). Real x64 user-mode heap
    // allocations live above 0x100000000 (4 GB); anything below that is an
    // FName index or other non-pointer integer and must be discarded so
    // the cascade falls through to the next tier.
    auto isValid = [](void* p) { return p && (uintptr_t)p > 0x100000000ULL; };
    // Log each tier's result so we can pinpoint which one (if any) is
    // returning a usable UClass on the current host.
    void* actorClass = Engine::loadAsset(widePath.c_str());
    logInfo("[Hydro.Assets] tier1 loadAsset('%s') = %p (valid=%d)",
            path, actorClass, isValid(actorClass) ? 1 : 0);
    if (!isValid(actorClass)) {
        actorClass = Engine::findObject(widePath.c_str());
        logInfo("[Hydro.Assets] tier2 findObject('%s') = %p (valid=%d)",
                path, actorClass, isValid(actorClass) ? 1 : 0);
    }
    if (!isValid(actorClass)) {
        actorClass = Engine::loadObject(widePath.c_str());
        std::string tier3Name = isValid(actorClass) ? Engine::getObjectName(actorClass) : std::string("<invalid>");
        logInfo("[Hydro.Assets] tier3 loadObject('%s') = %p name='%s' (valid=%d)",
                path, actorClass, tier3Name.c_str(), isValid(actorClass) ? 1 : 0);
    }

    if (!isValid(actorClass)) {
        logWarn("[Hydro.Assets] Class not found: %s", path);
        lua_pushnil(L);
        return 1;
    }

    void* actor = Engine::spawnActor(actorClass, x, y, z);
    Lua::pushUObject(L, actor);

    if (!actor)
        logError("[Hydro.Assets] Failed to spawn: %s", path);

    return 1;
}

// Assets.destroy

/// Destroy a spawned actor, removing it from the world.
///
/// Calls K2_DestroyActor via ProcessEvent on the actor. After this call,
/// the UObject handle becomes invalid - do not use it further.
///
/// @param actor UObject - The actor to destroy
/// @throws string - If actor is not a valid UObject
/// @engine AActor::K2_DestroyActor
/// @example
/// local Assets = require("Hydro.Assets")
/// local cube = Assets.spawn("/Game/Mods/MyMod/BP_Cube.BP_Cube_C")
/// -- later...
/// Assets.destroy(cube)
static int l_assets_destroy(lua_State* L) {
    void* actor = Lua::checkUObject(L, 1);
    if (!actor) {
        return luaL_error(L, "destroy() requires a valid UObject");
    }

    void* cls = Engine::getClass(actor);
    if (!cls) return 0;

    void* destroyFunc = Engine::findFunction(cls, L"K2_DestroyActor");
    if (destroyFunc) {
        uint8_t params[8] = {};
        Engine::callFunction(actor, destroyFunc, params);
    }

    return 0;
}

// Module registration

static const luaL_Reg assets_functions[] = {
    {"load",    l_assets_load},
    {"find",    l_assets_find},
    {"spawn",   l_assets_spawn},
    {"destroy", l_assets_destroy},
    {nullptr,   nullptr}
};

void registerAssetsModule(lua_State* L) {
    buildModuleTable(L, assets_functions);
}

} // namespace Hydro::API
