---@meta

--- @module Hydro.Assets
--- @description Load, find, spawn, and destroy game assets and actors.
---   This is the primary interface for getting content from pak files into
---   the game world. All returned objects are reflection-enabled UObjects.
---
---   NOTE: If a specific Tier 2 module exists for your use case, prefer it:
---   - Adding creatures? Use `Hydro.SN2.Creatures.register()` instead.
---   - Adding items? Use `Hydro.SN2.Items.register()` instead.
---   Specific APIs enable conflict detection, multiplayer sync, and
---   survive game updates.
--- @depends EngineAPI
--- @engine_systems AssetRegistry, GameplayStatics, UWorld

local Assets = {}

--- Load a Blueprint class or asset from a mounted pak file.
---
--- Uses the AssetRegistry to locate and load the asset. The pak containing
--- the asset must already be mounted (done automatically for mods listed
--- in `hydro_mods.json`).
---
--- @param assetPath string Full asset path (e.g., `/Game/Mods/MyMod/BP_Fish.BP_Fish_C`)
--- @return UObject|nil loaded The loaded asset, or nil if not found
--- @engine AssetRegistryHelpers::GetAsset
---
--- ```lua
--- local Assets = require("Hydro.Assets")
--- local fishClass = Assets.load("/Game/Mods/MyMod/BP_Fish.BP_Fish_C")
--- if fishClass then
---     print("Loaded: " .. tostring(fishClass))
--- end
--- ```
function Assets.load(assetPath) end

--- Find an already-loaded UObject by its full path.
---
--- Unlike `load()`, this only finds objects that are already in memory.
--- Use this for engine built-in classes (e.g., `/Script/Engine.PointLight`)
--- or objects loaded by other mods.
---
--- @param objectPath string Full object path (e.g., `/Script/Engine.Actor`)
--- @return UObject|nil found The found object, or nil if not in memory
--- @engine StaticFindObject
---
--- ```lua
--- local Assets = require("Hydro.Assets")
--- local actorClass = Assets.find("/Script/Engine.Actor")
--- ```
function Assets.find(objectPath) end

--- Load a Blueprint class and spawn it as an actor in the world.
---
--- Convenience function that combines `load()` + engine spawn. The actor
--- goes through the full UE lifecycle: construction script, component
--- registration, and BeginPlay. The returned UObject supports full
--- reflection - read/write properties, call functions.
---
--- @param classPath string Blueprint class path to spawn
--- @param x? number World X coordinate (default: 0)
--- @param y? number World Y coordinate (default: 0)
--- @param z? number World Z coordinate (default: 0)
--- @return UObject|nil actor The spawned actor with reflection, or nil on failure
--- @engine GameplayStatics::BeginDeferredActorSpawnFromClass
--- @engine GameplayStatics::FinishSpawningActor
---
--- ```lua
--- local Assets = require("Hydro.Assets")
--- local cube = Assets.spawn("/Game/Mods/MyMod/BP_Cube.BP_Cube_C", 100, 0, 50)
--- if cube then
---     print("Spawned at: " .. cube:GetAddress())
--- end
--- ```
function Assets.spawn(classPath, x, y, z) end

--- Destroy a spawned actor, removing it from the world.
---
--- Calls `K2_DestroyActor` via ProcessEvent on the actor. After this call,
--- the UObject handle becomes invalid - do not use it further.
---
--- @param actor UObject The actor to destroy
--- @engine AActor::K2_DestroyActor
---
--- ```lua
--- local Assets = require("Hydro.Assets")
--- local cube = Assets.spawn("/Game/Mods/MyMod/BP_Cube.BP_Cube_C")
--- -- later...
--- Assets.destroy(cube)
--- ```
function Assets.destroy(actor) end

return Assets
