///
/// @module Hydro.World
/// @description Discover and query game objects in the world.
///   Find actors by class, get the player, access the world.
///   All returned objects are reflection-enabled UObjects.
///
///   NOTE: This is a generic API for object discovery. For game-specific
///   operations, prefer the dedicated Tier 2 modules:
///   - Finding creatures? Use Hydro.SN2.Creatures.getAll() instead.
///   - Querying game state? Use the appropriate game-specific module.
///   Generic API is for novel mechanics, debugging, and cases not yet
///   covered by specific registries. See CONTRIBUTING_TIER2.md.
///
/// @depends EngineAPI (GUObjectArray, StaticFindObject, UWorld)
/// @engine_systems GUObjectArray, UWorld
///

#include "HydroWorld.h"
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

static std::wstring toWide(const char* str) {
    return std::wstring(str, str + strlen(str));
}

// World.findFirstOf

/// Find the first UObject in the world whose class name matches.
///
/// Scans all loaded objects (GUObjectArray) and returns the first one
/// whose UClass name matches the given string. Useful for finding
/// singletons like the player character, game mode, or HUD.
///
/// @param className string - The UClass name to search for (e.g., "PlayerCharacter", "BP_Fish_C")
/// @returns UObject - The first matching object, or nil if none found
/// @throws string - If className is not a string
/// @engine GUObjectArray iteration
/// @example
/// local World = require("Hydro.World")
/// local player = World.findFirstOf("PlayerCharacter")
/// if player then
///     print("Found player: " .. tostring(player))
/// end
static int l_world_findFirstOf(lua_State* L) {
    const char* className = luaL_checkstring(L, 1);
    void* obj = Engine::findFirstOf(toWide(className).c_str());
    Lua::pushUObject(L, obj);
    return 1;
}

// World.findAllOf

/// Find all UObjects in the world whose class name matches.
///
/// Returns a Lua table (array) of all matching objects. Limited to 256
/// results by default to prevent memory issues. For classes with many
/// instances (e.g., "StaticMeshActor"), consider using findFirstOf or
/// a more specific class name.
///
/// @param className string - The UClass name to search for
/// @param maxResults number? - Maximum results to return (default: 256)
/// @returns table - Array of UObjects, may be empty
/// @throws string - If className is not a string
/// @engine GUObjectArray iteration
/// @example
/// local World = require("Hydro.World")
/// local enemies = World.findAllOf("BP_EnemyBase_C")
/// print("Found " .. #enemies .. " enemies")
/// for i, enemy in ipairs(enemies) do
///     print("  " .. tostring(enemy))
/// end
static int l_world_findAllOf(lua_State* L) {
    const char* className = luaL_checkstring(L, 1);
    int maxResults = (int)luaL_optinteger(L, 2, 256);

    if (maxResults > 4096) maxResults = 4096;
    if (maxResults < 1) maxResults = 1;

    // Stack-allocate for small counts, heap for large
    void* stackBuf[256];
    void** results = (maxResults <= 256) ? stackBuf : new void*[maxResults];

    int count = Engine::findAllOf(toWide(className).c_str(), results, maxResults);

    lua_createtable(L, count, 0);
    for (int i = 0; i < count; i++) {
        Lua::pushUObject(L, results[i]);
        lua_rawseti(L, -2, i + 1); // Lua arrays are 1-indexed
    }

    if (maxResults > 256) delete[] results;
    return 1;
}

// World.getWorld

/// Get the current UWorld object.
///
/// Returns the active game world. From the world, you can access
/// properties like game time, level name, and call world-level functions.
///
/// @returns UObject - The current UWorld, or nil if not available
/// @engine UWorld (GWorld global pointer)
/// @example
/// local World = require("Hydro.World")
/// local world = World.getWorld()
/// if world then
///     print("World: " .. tostring(world))
/// end
static int l_world_getWorld(lua_State* L) {
    Lua::pushUObject(L, Engine::getWorld());
    return 1;
}

// World.getGameplayStatics

/// Get the GameplayStatics default object (CDO).
///
/// GameplayStatics is a Blueprint function library with utility functions
/// like GetAllActorsOfClass, GetPlayerCharacter, etc. Call functions on
/// the returned object via reflection.
///
/// @returns UObject - The GameplayStatics CDO, or nil
/// @engine UGameplayStatics (Default__GameplayStatics)
/// @example
/// local World = require("Hydro.World")
/// local statics = World.getGameplayStatics()
/// if statics then
///     -- Call any GameplayStatics function via reflection
///     -- statics:GetPlayerCharacter(world, 0)
/// end
static int l_world_getGameplayStatics(lua_State* L) {
    void* cdo = Engine::findObject(L"Default__GameplayStatics");
    Lua::pushUObject(L, cdo);
    return 1;
}

// Module registration

static const luaL_Reg world_functions[] = {
    {"findFirstOf",         l_world_findFirstOf},
    {"findAllOf",           l_world_findAllOf},
    {"getWorld",            l_world_getWorld},
    {"getGameplayStatics",  l_world_getGameplayStatics},
    {nullptr,               nullptr}
};

void registerWorldModule(lua_State* L) {
    buildModuleTable(L, world_functions);
}

} // namespace Hydro::API
