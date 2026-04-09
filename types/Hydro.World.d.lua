---@meta

--- @module Hydro.World
--- @description Discover and query game objects in the world.
---   Find actors by class, get the player, access the world and
---   GameplayStatics. All returned objects are reflection-enabled UObjects.
---
---   NOTE: For game-specific operations, prefer dedicated Tier 2 modules:
---   - Finding creatures? Use `Hydro.SN2.Creatures.getAll()` instead.
---   - Querying game state? Use the appropriate game-specific module.
--- @depends EngineAPI
--- @engine_systems GUObjectArray, UWorld

local World = {}

--- Find the first UObject in the world whose class name matches.
---
--- Scans all loaded objects (GUObjectArray) and returns the first one
--- whose UClass name matches the given string. Useful for finding
--- singletons like the player character, game mode, or HUD.
---
--- @param className string The UClass name to search for (e.g., `"PlayerCharacter"`, `"BP_Fish_C"`)
--- @return UObject|nil found The first matching object, or nil if none found
--- @engine GUObjectArray iteration
---
--- ```lua
--- local World = require("Hydro.World")
--- local player = World.findFirstOf("PlayerCharacter")
--- if player then
---     print("Found player: " .. tostring(player))
--- end
--- ```
function World.findFirstOf(className) end

--- Find all UObjects in the world whose class name matches.
---
--- Returns a table (array) of all matching objects. Limited to 256
--- results by default to prevent memory issues. For classes with many
--- instances, consider using `findFirstOf` or a more specific class name.
---
--- @param className string The UClass name to search for
--- @param maxResults? number Maximum results to return (default: 256, max: 4096)
--- @return UObject[] found Array of matching UObjects, may be empty
--- @engine GUObjectArray iteration
---
--- ```lua
--- local World = require("Hydro.World")
--- local enemies = World.findAllOf("BP_EnemyBase_C")
--- print("Found " .. #enemies .. " enemies")
--- for i, enemy in ipairs(enemies) do
---     print("  " .. tostring(enemy))
--- end
--- ```
function World.findAllOf(className, maxResults) end

--- Get the current UWorld object.
---
--- Returns the active game world. From the world, you can access
--- properties like game time, level name, and call world-level functions.
---
--- @return UObject|nil world The current UWorld, or nil if not available
--- @engine GWorld global pointer
---
--- ```lua
--- local World = require("Hydro.World")
--- local world = World.getWorld()
--- if world then
---     print("World: " .. tostring(world))
--- end
--- ```
function World.getWorld() end

--- Get the GameplayStatics default object (CDO).
---
--- GameplayStatics is a Blueprint function library with utility functions
--- like `GetAllActorsOfClass`, `GetPlayerCharacter`, etc. Call functions on
--- the returned object via reflection.
---
--- @return UObject|nil statics The GameplayStatics CDO, or nil
--- @engine Default__GameplayStatics
---
--- ```lua
--- local World = require("Hydro.World")
--- local statics = World.getGameplayStatics()
--- -- Call any GameplayStatics function via reflection:
--- -- statics:GetPlayerCharacter(world, 0)
--- ```
function World.getGameplayStatics() end

return World
