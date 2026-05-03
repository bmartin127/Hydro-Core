---@meta

--- @module Hydro.Events
--- @description Hook into engine events and UFunction calls.
---   Register callbacks on any UFunction. When the engine calls that
---   function, your Lua callback fires with the target object.
---
---   Implementation: patches each UFunction's native Func pointer
---   individually. This catches ALL calls to the hooked function
---   regardless of dispatch path (Blueprint, C++, network replication,
---   timers).
---
---   NOTE: For game-specific events, prefer dedicated Tier 2 modules
---   when available.
--- @depends EngineAPI
--- @engine_systems UFunction::Func native pointer

local Events = {}

--- Register a hook on a UFunction by path.
---
--- Patches the UFunction's native Func pointer so your callback fires
--- whenever the engine calls that function - from any code path.
--- Multiple mods can hook the same function; callbacks fire in
--- registration order, then the original function executes.
---
--- @param funcPath string Full UFunction path (e.g., `"/Script/Engine.Actor:ReceiveBeginPlay"`)
--- @param callback fun(self: UObject) Called when the function fires. Receives the target object.
--- @return boolean success true if the hook was registered
--- @engine UFunction::Func native pointer replacement
---
--- ```lua
--- local Events = require("Hydro.Events")
--- Events.hook("/Script/Engine.Actor:ReceiveBeginPlay", function(self)
---     print("An actor began play: " .. tostring(self))
--- end)
--- ```
function Events.hook(funcPath, callback) end

--- Remove all hooks registered by the calling mod.
---
--- Cleans up all function hooks created by this mod. If a hooked function
--- has no remaining callbacks from any mod, its original Func pointer is
--- restored. Call this during mod unload or when you no longer need the hooks.
---
--- @return number removed Count of hooks removed
---
--- ```lua
--- local Events = require("Hydro.Events")
--- -- Clean up all our hooks
--- local count = Events.unhookAll()
--- print("Removed " .. count .. " hooks")
--- ```
function Events.unhookAll() end

return Events
