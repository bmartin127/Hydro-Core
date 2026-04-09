#include "LuaUObject.h"
#include "HydroCore.h"
#include "EngineAPI.h"

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

// Type dispatch: read a property value and push to Lua

static void pushPropertyValue(lua_State* L, void* obj, void* prop) {
    using namespace Engine;
    const auto& types = getPropertyTypeNames();
    uint32_t typeId = getPropertyTypeNameIndex(prop);
    int32_t offset = getPropertyOffset(prop);
    int32_t size = getPropertyElementSize(prop);

    if (offset < 0) { lua_pushnil(L); return; }

    uint8_t* data = (uint8_t*)obj + offset;

    // Numeric types
    if (typeId == types.intProperty) {
        int32_t val = 0;
        readInt32(data, &val);
        lua_pushinteger(L, val);
    }
    else if (typeId == types.int64Property) {
        int64_t val = 0;
        memcpy(&val, data, 8);
        lua_pushnumber(L, (double)val);
    }
    else if (typeId == types.floatProperty) {
        float val = 0;
        memcpy(&val, data, 4);
        lua_pushnumber(L, val);
    }
    else if (typeId == types.doubleProperty) {
        double val = 0;
        memcpy(&val, data, 8);
        lua_pushnumber(L, val);
    }
    else if (typeId == types.boolProperty) {
        // UE BoolProperty can be bitfield - size=1 for simple bool
        uint8_t val = 0;
        memcpy(&val, data, 1);
        lua_pushboolean(L, val != 0);
    }
    else if (typeId == types.byteProperty || typeId == types.int8Property) {
        lua_pushinteger(L, *(int8_t*)data);
    }
    else if (typeId == types.uint16Property || typeId == types.int16Property) {
        int16_t val = 0;
        memcpy(&val, data, 2);
        lua_pushinteger(L, val);
    }
    else if (typeId == types.uint32Property) {
        uint32_t val = 0;
        memcpy(&val, data, 4);
        lua_pushnumber(L, val);
    }
    else if (typeId == types.enumProperty) {
        // Enums stored as uint8 in UE5
        lua_pushinteger(L, *(uint8_t*)data);
    }
    // String types
    else if (typeId == types.strProperty) {
        // FString layout: TCHAR* Data (pointer), int32 Num, int32 Max
        void* strData = nullptr;
        readPtr(data, &strData);
        if (strData) {
            // Convert TCHAR (wchar_t) to UTF-8
            const wchar_t* wstr = (const wchar_t*)strData;
            int len = 0;
            while (wstr[len] && len < 4096) len++;
            std::string utf8(len, '\0');
            for (int i = 0; i < len; i++) utf8[i] = (char)wstr[i]; // Simple narrow (ASCII safe)
            lua_pushlstring(L, utf8.c_str(), utf8.size());
        } else {
            lua_pushstring(L, "");
        }
    }
    else if (typeId == types.nameProperty) {
        // FName: just push the comparison index as a number for now
        uint32_t nameIdx = 0;
        readInt32(data, (int32_t*)&nameIdx);
        lua_pushinteger(L, nameIdx);
    }
    // Object references
    else if (typeId == types.objectProperty || typeId == types.classProperty) {
        void* ref = nullptr;
        readPtr(data, &ref);
        pushUObject(L, ref);
    }
    // Struct types
    else if (typeId == types.structProperty) {
        // Push as a table with raw bytes accessible
        // For common structs (FVector, FRotator), provide named fields
        // For now: push FVector-like structs as {X, Y, Z}
        if (size == 24) {
            // Likely FVector (3 doubles in UE5)
            lua_newtable(L);
            double x, y, z;
            memcpy(&x, data, 8);
            memcpy(&y, data + 8, 8);
            memcpy(&z, data + 16, 8);
            lua_pushnumber(L, x); lua_setfield(L, -2, "X");
            lua_pushnumber(L, y); lua_setfield(L, -2, "Y");
            lua_pushnumber(L, z); lua_setfield(L, -2, "Z");
        } else if (size == 12) {
            // Likely FVector with floats (pre-UE5)
            lua_newtable(L);
            float x, y, z;
            memcpy(&x, data, 4);
            memcpy(&y, data + 4, 4);
            memcpy(&z, data + 8, 4);
            lua_pushnumber(L, x); lua_setfield(L, -2, "X");
            lua_pushnumber(L, y); lua_setfield(L, -2, "Y");
            lua_pushnumber(L, z); lua_setfield(L, -2, "Z");
        } else {
            // Generic struct - push as lightuserdata for now
            lua_pushlightuserdata(L, data);
        }
    }
    // Fallback
    else {
        lua_pushnil(L);
    }
}

// Lua closure body for invoking a UFunction discovered via obj:FunctionName().
// Called from the __index metamethod's function-lookup path.
// Upvalues: [1] = target UObject*, [2] = UFunction*.
static int uobject_call_function(lua_State* L) {
    void* obj = lua_touserdata(L, lua_upvalueindex(1));
    void* fn = lua_touserdata(L, lua_upvalueindex(2));

    // Read ParmsSize from UFunction
    uint16_t parmsSize = 0;
    memcpy(&parmsSize, (uint8_t*)fn + Engine::UFUNC_PARMS_SIZE, 2);
    uint16_t retOffset = 0;
    memcpy(&retOffset, (uint8_t*)fn + Engine::UFUNC_RET_VAL_OFFSET, 2);

    // Allocate param buffer
    uint8_t* params = (uint8_t*)alloca(parmsSize > 0 ? parmsSize : 8);
    memset(params, 0, parmsSize > 0 ? parmsSize : 8);

    // Fill params from Lua arguments by walking the UFunction's property chain
    void* paramProp = Engine::getChildProperties(fn);
    int argIdx = 1; // Lua args start at 1 (self is handled by upvalue)
    while (paramProp) {
        int32_t off = Engine::getPropertyOffset(paramProp);
        int32_t sz = Engine::getPropertyElementSize(paramProp);
        uint32_t typeId = Engine::getPropertyTypeNameIndex(paramProp);
        const auto& types = Engine::getPropertyTypeNames();

        uint64_t pflags = Engine::getPropertyFlags(paramProp);

        // Skip return value - identified by CPF_ReturnParm flag
        if (pflags & Engine::CPF_ReturnParm) {
            paramProp = Engine::getNextProperty(paramProp);
            continue;
        }

        if (argIdx <= lua_gettop(L)) {
            uint8_t* dest = params + off;

            if (typeId == types.intProperty || typeId == types.int8Property ||
                typeId == types.int16Property || typeId == types.enumProperty ||
                typeId == types.byteProperty) {
                int32_t val = (int32_t)luaL_checkinteger(L, argIdx);
                memcpy(dest, &val, sz > 4 ? 4 : sz);
            }
            else if (typeId == types.floatProperty) {
                float val = (float)luaL_checknumber(L, argIdx);
                memcpy(dest, &val, 4);
            }
            else if (typeId == types.doubleProperty) {
                double val = luaL_checknumber(L, argIdx);
                memcpy(dest, &val, 8);
            }
            else if (typeId == types.boolProperty) {
                uint8_t val = lua_toboolean(L, argIdx) ? 1 : 0;
                memcpy(dest, &val, 1);
            }
            else if (typeId == types.objectProperty || typeId == types.classProperty) {
                void* ref = checkUObject(L, argIdx);
                memcpy(dest, &ref, 8);
            }
            // TODO: FString, FVector, struct params
            argIdx++;
        }

        paramProp = Engine::getNextProperty(paramProp);
    }

    // Call ProcessEvent
    if (!Engine::callFunction(obj, fn, params)) {
        return luaL_error(L, "ProcessEvent failed");
    }

    // Return value
    if (retOffset < parmsSize) {
        // For now, return as lightuserdata or number based on size
        void* retVal = nullptr;
        memcpy(&retVal, params + retOffset, 8);
        if (retVal) {
            // UObject* detection heuristic: without reflection metadata on
            // the return slot's type, probe whether the pointer's
            // ClassPrivate slot (offset UOBJ_CLASS) dereferences via
            // readPtr (SEH-guarded) to a non-null pointer. If so, treat
            // retVal as a UObject* and wrap it; otherwise fall back to
            // exposing the raw pointer as lightuserdata.
            void* retClass = nullptr;
            if (Engine::readPtr((uint8_t*)retVal + Engine::UOBJ_CLASS, &retClass) && retClass) {
                pushUObject(L, retVal);
            } else {
                lua_pushlightuserdata(L, retVal);
            }
        } else {
            lua_pushnil(L);
        }
        return 1;
    }

    return 0;
}

// __index: obj.Property or obj:Function

// Built-in methods

static int uobject_isvalid(lua_State* L) {
    UObjectUD* ud = (UObjectUD*)luaL_checkudata(L, 1, UOBJECT_MT);
    lua_pushboolean(L, ud->ptr != nullptr);
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

    const auto& types = Engine::getPropertyTypeNames();
    uint32_t typeId = Engine::getPropertyTypeNameIndex(prop);
    int32_t offset = Engine::getPropertyOffset(prop);
    int32_t size = Engine::getPropertyElementSize(prop);
    uint8_t* data = (uint8_t*)ud->ptr + offset;

    if (typeId == types.intProperty) {
        int32_t val = (int32_t)luaL_checkinteger(L, 3);
        memcpy(data, &val, 4);
    }
    else if (typeId == types.floatProperty) {
        float val = (float)luaL_checknumber(L, 3);
        memcpy(data, &val, 4);
    }
    else if (typeId == types.doubleProperty) {
        double val = luaL_checknumber(L, 3);
        memcpy(data, &val, 8);
    }
    else if (typeId == types.boolProperty) {
        uint8_t val = lua_toboolean(L, 3) ? 1 : 0;
        memcpy(data, &val, 1);
    }
    else if (typeId == types.byteProperty || typeId == types.int8Property) {
        int8_t val = (int8_t)luaL_checkinteger(L, 3);
        memcpy(data, &val, 1);
    }
    else if (typeId == types.objectProperty || typeId == types.classProperty) {
        void* ref = checkUObject(L, 3);
        memcpy(data, &ref, 8);
    }
    else {
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
