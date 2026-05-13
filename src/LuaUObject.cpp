#include "LuaUObject.h"
#include "HydroCore.h"
#include "EngineAPI.h"
#include "PropertyMarshal.h"

extern "C" {
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>
}

#include <cstring>
#include <string>

namespace Hydro::Lua {

static const char* UOBJECT_MT = "HydroUObject";

struct UObjectUD {
    void* ptr; // Raw UObject*
};

// Single-seam type dispatch lives in PropertyMarshal. This wrapper preserves
// the (L, obj, prop) public signature used by HydroReflect and __index.
void pushPropertyValue(lua_State* L, void* obj, void* prop) {
    int32_t offset = Engine::getPropertyOffset(prop);
    if (offset < 0) { lua_pushnil(L); return; }
    PropertyMarshal::readToLua(L, prop, (const uint8_t*)obj + offset);
}

// Lua closure body for invoking a UFunction discovered via obj:FunctionName().
// Called from the __index metamethod's function-lookup path.
// Upvalues: [1] = target UObject*, [2] = UFunction*.
static int uobject_call_function(lua_State* L) {
    void* obj = lua_touserdata(L, lua_upvalueindex(1));
    void* fn = lua_touserdata(L, lua_upvalueindex(2));

    // ParmsSize derived from the UFunction's parameter chain (Layer 3) -
    // robust against UE-version layout shifts. The return-slot offset comes
    // from the property chain walk below, so we no longer need a precomputed
    // retOffset; CPF_ReturnParm + getPropertyOffset gives us the same answer
    // and works for out-params too.
    uint16_t parmsSize = Engine::getUFunctionParmsSize(fn);

    // Allocate param buffer
    uint8_t* params = (uint8_t*)alloca(parmsSize > 0 ? parmsSize : 8);
    memset(params, 0, parmsSize > 0 ? parmsSize : 8);

    // Fill params from Lua arguments by walking the UFunction's property
    // chain. Routes through PropertyMarshal so every type the marshal learns
    // becomes a callable arg automatically. Unsupported types stay zeroed -
    // matches prior behaviour, which other paths rely on.
    void* paramProp = Engine::getChildProperties(fn);

    // `obj:Method(arg)` colon syntax pushes `self` as Lua arg 1 even though
    // we already have the receiver via upvalue. Detect that case and start
    // from arg 2 so the first real input lines up with the first param.
    int argIdx = 1;
    if (lua_gettop(L) >= 1 && lua_isuserdata(L, 1) && checkUObject(L, 1) == obj) {
        argIdx = 2;
    }
    while (paramProp) {
        uint64_t pflags = Engine::getPropertyFlags(paramProp);

        // Skip return value - identified by CPF_ReturnParm flag
        if (pflags & Engine::CPF_ReturnParm) {
            paramProp = Engine::getNextProperty(paramProp);
            continue;
        }

        if (argIdx <= lua_gettop(L)) {
            int32_t off = Engine::getPropertyOffset(paramProp);
            if (off >= 0) {
                PropertyMarshal::writeFromLua(L, paramProp, params + off, argIdx);
            }
            argIdx++;
        }

        paramProp = Engine::getNextProperty(paramProp);
    }

    // Call ProcessEvent
    if (!Engine::callFunction(obj, fn, params)) {
        return luaL_error(L, "ProcessEvent failed");
    }

    // Return value + out-params. Walk the property chain a second time. The
    // CPF_ReturnParm slot becomes the first Lua return; every CPF_OutParm
    // (that isn't also CPF_ReturnParm) becomes a subsequent return value in
    // declaration order. Old behaviour was a blind 8-byte memcpy of the
    // return slot with a UObject heuristic - that meant every scalar return
    // and every out-param came back as garbage or nil.
    int returns = 0;
    void* outProp = Engine::getChildProperties(fn);
    while (outProp) {
        uint64_t pflags = Engine::getPropertyFlags(outProp);
        bool isReturn = (pflags & Engine::CPF_ReturnParm) != 0;
        bool isOut = (pflags & Engine::CPF_OutParm) != 0 && !isReturn;
        if (isReturn || isOut) {
            int32_t off = Engine::getPropertyOffset(outProp);
            if (off >= 0 && (uint32_t)off < parmsSize) {
                PropertyMarshal::readToLua(L, outProp, params + off);
                returns++;
            }
        }
        outProp = Engine::getNextProperty(outProp);
    }
    return returns;
}

// __index: obj.Property or obj:Function

// Built-in methods

static int uobject_isvalid(lua_State* L) {
    UObjectUD* ud = (UObjectUD*)luaL_checkudata(L, 1, UOBJECT_MT);
    if (!ud->ptr) { lua_pushboolean(L, 0); return 1; }

    // Look up the object's slot in GUObjectArray. The pointer-only check
    // below was a stale-handle bomb: a GC'd actor's pointer is non-null but
    // dereferencing it crashes. Now we verify the slot still wraps *this*
    // pointer; GC freeing or recycling the slot makes the slot pointer
    // differ (or null), and we report invalid.
    int32_t idx = -1;
    if (!Engine::readInt32((uint8_t*)ud->ptr + Engine::UOBJ_INDEX, &idx) || idx < 0) {
        lua_pushboolean(L, 0); return 1;
    }
    void* item = Engine::getObjectItemAt(idx);
    if (!item) { lua_pushboolean(L, 0); return 1; }

    void* slotObj = nullptr;
    if (!Engine::readPtr((uint8_t*)item + Engine::FUOBJ_OBJECT, &slotObj) ||
        slotObj != ud->ptr) {
        lua_pushboolean(L, 0); return 1;
    }

    // No flag check: EInternalObjectFlags bit positions vary across UE
    // versions (5.5: Unreachable=1<<28, Garbage=1<<21; older docs have
    // Native=1<<25 which UClass objects always carry - that bit was the
    // reason an earlier draft falsely rejected /Script/Engine.* classes).
    // The slot-pointer match above is sufficient for the common case
    // (GC freeing or recycling the slot); precise flag-bit discovery is
    // a Layer-1-style probe deferred until a real GC-edge bug surfaces.

    lua_pushboolean(L, 1);
    return 1;
}

static int uobject_getaddress(lua_State* L) {
    UObjectUD* ud = (UObjectUD*)luaL_checkudata(L, 1, UOBJECT_MT);
    lua_pushnumber(L, (double)(uintptr_t)ud->ptr);
    return 1;
}

static int uobject_getclass(lua_State* L) {
    UObjectUD* ud = (UObjectUD*)luaL_checkudata(L, 1, UOBJECT_MT);
    if (!ud->ptr) { lua_pushnil(L); return 1; }
    pushUObject(L, Engine::getClass(ud->ptr));
    return 1;
}

static int uobject_getouter(lua_State* L) {
    UObjectUD* ud = (UObjectUD*)luaL_checkudata(L, 1, UOBJECT_MT);
    if (!ud->ptr) { lua_pushnil(L); return 1; }
    pushUObject(L, Engine::getOuter(ud->ptr));
    return 1;
}

static int uobject_getname(lua_State* L) {
    UObjectUD* ud = (UObjectUD*)luaL_checkudata(L, 1, UOBJECT_MT);
    if (!ud->ptr) { lua_pushnil(L); return 1; }
    std::string n = Engine::getObjectName(ud->ptr);
    lua_pushlstring(L, n.data(), n.size());
    return 1;
}

static int uobject_getpath(lua_State* L) {
    UObjectUD* ud = (UObjectUD*)luaL_checkudata(L, 1, UOBJECT_MT);
    if (!ud->ptr) { lua_pushnil(L); return 1; }
    std::string p = Engine::getObjectPath(ud->ptr);
    lua_pushlstring(L, p.data(), p.size());
    return 1;
}

static int uobject_getflags(lua_State* L) {
    UObjectUD* ud = (UObjectUD*)luaL_checkudata(L, 1, UOBJECT_MT);
    if (!ud->ptr) { lua_pushnil(L); return 1; }
    int32_t flags = 0;
    if (!Engine::readInt32((uint8_t*)ud->ptr + Engine::UOBJ_FLAGS, &flags)) {
        lua_pushnil(L); return 1;
    }
    lua_pushnumber(L, (double)(uint32_t)flags);
    return 1;
}

// obj:SetFlags(setMask, clearMask) - OR in setMask, AND-NOT clearMask.
// Returns the new flag value, or nil if read/write failed.
static int uobject_setflags(lua_State* L) {
    UObjectUD* ud = (UObjectUD*)luaL_checkudata(L, 1, UOBJECT_MT);
    uint32_t setMask   = (uint32_t)luaL_checknumber(L, 2);
    uint32_t clearMask = lua_gettop(L) >= 3 ? (uint32_t)luaL_checknumber(L, 3) : 0;
    if (!ud->ptr) { lua_pushnil(L); return 1; }

    int32_t cur = 0;
    if (!Engine::readInt32((uint8_t*)ud->ptr + Engine::UOBJ_FLAGS, &cur)) {
        lua_pushnil(L); return 1;
    }
    uint32_t next = ((uint32_t)cur | setMask) & ~clearMask;
    volatile uint32_t* slot = (volatile uint32_t*)((uint8_t*)ud->ptr + Engine::UOBJ_FLAGS);
    __try {
        *slot = next;
    } __except(1) {
        lua_pushnil(L); return 1;
    }
    lua_pushnumber(L, (double)next);
    return 1;
}

// __index: check builtins -> property -> function

static int uobject_index(lua_State* L) {
    UObjectUD* ud = (UObjectUD*)luaL_checkudata(L, 1, UOBJECT_MT);
    const char* key = luaL_checkstring(L, 2);

    if (!ud->ptr) {
        lua_pushnil(L);
        return 1;
    }

    // Check metatable for built-in methods first (IsValid, GetClass, etc.)
    luaL_getmetatable(L, UOBJECT_MT);
    lua_pushvalue(L, 2);
    lua_rawget(L, -2);
    if (!lua_isnil(L, -1)) {
        lua_remove(L, -2); // remove metatable
        return 1;
    }
    lua_pop(L, 2); // pop nil + metatable

    void* cls = Engine::getClass(ud->ptr);
    if (!cls) { lua_pushnil(L); return 1; }

    // Convert key to wide string for FName lookup
    wchar_t wkey[256];
    for (int i = 0; key[i] && i < 255; i++) { wkey[i] = key[i]; wkey[i+1] = 0; }

    // Try as property first
    void* prop = Engine::findProperty(cls, wkey);
    if (prop) {
        pushPropertyValue(L, ud->ptr, prop);
        return 1;
    }

    // Try as function - return a callable closure
    void* func = Engine::findFunction(cls, wkey);
    if (func) {
        // Push a closure that captures the object and function
        lua_pushlightuserdata(L, ud->ptr);
        lua_pushlightuserdata(L, func);
        lua_pushcclosure(L, uobject_call_function, 2);
        return 1;
    }

    // Not found
    lua_pushnil(L);
    return 1;
}

// __newindex: obj.Property = value

static int uobject_newindex(lua_State* L) {
    UObjectUD* ud = (UObjectUD*)luaL_checkudata(L, 1, UOBJECT_MT);
    const char* key = luaL_checkstring(L, 2);
    // value is at index 3

    if (!ud->ptr) return 0;

    void* cls = Engine::getClass(ud->ptr);
    if (!cls) return 0;

    wchar_t wkey[256];
    for (int i = 0; key[i] && i < 255; i++) { wkey[i] = key[i]; wkey[i+1] = 0; }

    void* prop = Engine::findProperty(cls, wkey);
    if (!prop) {
        return luaL_error(L, "Property '%s' not found", key);
    }

    int32_t offset = Engine::getPropertyOffset(prop);
    if (offset < 0) return 0;
    if (!PropertyMarshal::writeFromLua(L, prop, (uint8_t*)ud->ptr + offset, 3)) {
        return luaL_error(L, "Cannot set property '%s' (unsupported type)", key);
    }
    return 0;
}

// __tostring

static int uobject_tostring(lua_State* L) {
    UObjectUD* ud = (UObjectUD*)luaL_checkudata(L, 1, UOBJECT_MT);
    if (ud->ptr) {
        lua_pushfstring(L, "UObject(%p)", ud->ptr);
    } else {
        lua_pushstring(L, "UObject(null)");
    }
    return 1;
}

// __eq

static int uobject_eq(lua_State* L) {
    UObjectUD* a = (UObjectUD*)luaL_checkudata(L, 1, UOBJECT_MT);
    UObjectUD* b = (UObjectUD*)luaL_checkudata(L, 2, UOBJECT_MT);
    lua_pushboolean(L, a->ptr == b->ptr);
    return 1;
}

// Public API

void initUObjectMetatable(lua_State* L) {
    luaL_newmetatable(L, UOBJECT_MT);

    // Metamethods
    lua_pushcfunction(L, uobject_index);
    lua_setfield(L, -2, "__index");

    lua_pushcfunction(L, uobject_newindex);
    lua_setfield(L, -2, "__newindex");

    lua_pushcfunction(L, uobject_tostring);
    lua_setfield(L, -2, "__tostring");

    lua_pushcfunction(L, uobject_eq);
    lua_setfield(L, -2, "__eq");

    // Built-in methods (checked first by __index before property/function lookup)
    lua_pushcfunction(L, uobject_isvalid);
    lua_setfield(L, -2, "IsValid");

    lua_pushcfunction(L, uobject_getclass);
    lua_setfield(L, -2, "GetClass");

    lua_pushcfunction(L, uobject_getaddress);
    lua_setfield(L, -2, "GetAddress");

    lua_pushcfunction(L, uobject_getouter);
    lua_setfield(L, -2, "GetOuter");

    lua_pushcfunction(L, uobject_getname);
    lua_setfield(L, -2, "GetName");

    lua_pushcfunction(L, uobject_getpath);
    lua_setfield(L, -2, "GetPath");

    lua_pushcfunction(L, uobject_getflags);
    lua_setfield(L, -2, "GetFlags");

    lua_pushcfunction(L, uobject_setflags);
    lua_setfield(L, -2, "SetFlags");

    lua_pop(L, 1);
}

void pushUObject(lua_State* L, void* ptr) {
    if (!ptr) {
        lua_pushnil(L);
        return;
    }
    UObjectUD* ud = (UObjectUD*)lua_newuserdata(L, sizeof(UObjectUD));
    ud->ptr = ptr;
    luaL_getmetatable(L, UOBJECT_MT);
    lua_setmetatable(L, -2);
}

void* checkUObject(lua_State* L, int idx) {
    // Accept both UObject userdata and lightuserdata (backwards compat)
    if (lua_isuserdata(L, idx)) {
        UObjectUD* ud = (UObjectUD*)luaL_testudata(L, idx, UOBJECT_MT);
        if (ud) return ud->ptr;
        // Fallback: lightuserdata
        if (lua_islightuserdata(L, idx))
            return lua_touserdata(L, idx);
    }
    return nullptr;
}

} // namespace Hydro::Lua
