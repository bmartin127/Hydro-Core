---@meta

--- A reflection-enabled Unreal Engine object.
---
--- All UObjects returned by Hydro APIs support:
--- - **Property access**: `obj.PropertyName` reads a UProperty via reflection
--- - **Property write**: `obj.PropertyName = value` writes a UProperty
--- - **Function calls**: `obj:FunctionName(args)` calls a UFunction via ProcessEvent
--- - **Built-in methods**: `IsValid()`, `GetClass()`, `GetAddress()`
--- - **String conversion**: `tostring(obj)` returns `"ClassName:0xAddress"`
---
--- Properties and functions are resolved at runtime from the engine's
--- reflection data - no hardcoded offsets. Type conversion between Lua and
--- UE types happens automatically (25+ type mappings).
---@class UObject
local UObject = {}

--- Check if this UObject is still valid (not garbage collected or destroyed).
--- @return boolean valid true if the object pointer is non-null and accessible
function UObject:IsValid() end

--- Get the UClass of this object.
--- @return UObject|nil class The UClass, or nil if invalid
function UObject:GetClass() end

--- Get the raw memory address of this object (for debugging).
--- @return string address Hex address string (e.g., `"0x1A2B3C4D"`)
function UObject:GetAddress() end

return UObject
