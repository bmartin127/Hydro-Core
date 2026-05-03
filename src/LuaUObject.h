#pragma once

struct lua_State;

namespace Hydro::Lua {

/// Initialize the UObject metatable in the Lua state.
/// Call once after LuaRuntime::initialize().
void initUObjectMetatable(lua_State* L);

/// Push a UObject* as a wrapped Lua userdata with metamethods.
/// Lua code can then do: obj.PropertyName, obj:FunctionName(), etc.
/// Pushes nil if ptr is nullptr.
void pushUObject(lua_State* L, void* ptr);

/// Check if the value at the given stack index is a UObject userdata.
/// Returns the raw void* pointer, or nullptr.
void* checkUObject(lua_State* L, int idx);

/// Push a property's value onto the Lua stack. Reads from the object
/// at the property's offset and converts to the appropriate Lua type.
void pushPropertyValue(lua_State* L, void* obj, void* prop);

} // namespace Hydro::Lua
