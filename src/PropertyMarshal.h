#pragma once

#include <cstdint>

struct lua_State;

namespace Hydro::PropertyMarshal {

/// UE memory -> Lua stack. Pushes exactly one value (nil on unsupported type).
/// `data` is the resolved address of the property value (caller adds the
/// property's offset). Returns true if a non-nil value was pushed.
bool readToLua(lua_State* L, void* prop, const uint8_t* data);

/// Lua stack -> UE memory. Reads from luaIdx, writes through `data` using
/// type-specific encoding. Returns true on success, false if the type is
/// unsupported (caller decides whether to luaL_error). Caller must ensure
/// luaIdx is a valid stack slot.
bool writeFromLua(lua_State* L, void* prop, uint8_t* data, int luaIdx);

} // namespace Hydro::PropertyMarshal
