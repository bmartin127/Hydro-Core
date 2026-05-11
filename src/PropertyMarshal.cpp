/// PropertyMarshal - single seam for UE5 property <-> Lua value conversion.
///
/// Before this module existed, type dispatch was duplicated across four call
/// sites in LuaUObject.cpp (property read, property write, function param
/// fill, function return read), each with a different and mostly smaller
/// subset of supported types. Most notably the function-return path was a
/// blind 8-byte memcpy that returned junk for every scalar return type.
///
/// All four sites now route through readToLua / writeFromLua. New property
/// types are added in one place and become visible everywhere.

#include "PropertyMarshal.h"
#include "EngineAPI.h"
#include "LuaUObject.h"

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

#include <cstring>
#include <string>

namespace Hydro::PropertyMarshal {

bool readToLua(lua_State* L, void* prop, const uint8_t* data) {
    using namespace Engine;
    const auto& types = getPropertyTypeNames();
    uint32_t typeId = getPropertyTypeNameIndex(prop);
    int32_t size = getPropertyElementSize(prop);

    if (!data) { lua_pushnil(L); return false; }

    // Numeric types
    if (typeId == types.intProperty) {
        int32_t val = 0;
        readInt32((void*)data, &val);
        lua_pushinteger(L, val);
        return true;
    }
    if (typeId == types.int64Property) {
        int64_t val = 0;
        memcpy(&val, data, 8);
        lua_pushnumber(L, (double)val);
        return true;
    }
    if (typeId == types.floatProperty) {
        float val = 0;
        memcpy(&val, data, 4);
        lua_pushnumber(L, val);
        return true;
    }
    if (typeId == types.doubleProperty) {
        double val = 0;
        memcpy(&val, data, 8);
        lua_pushnumber(L, val);
        return true;
    }
    if (typeId == types.boolProperty) {
        // BoolProperty bitfield mask is not yet discovered (Phase 2). For
        // size=1 simple bools the raw byte test is correct; for packed
        // bitfields it can read other bools' bits. Phase 2 fix lands
        // FieldMask discovery in EngineAPI.
        uint8_t val = 0;
        memcpy(&val, data, 1);
        lua_pushboolean(L, val != 0);
        return true;
    }
    if (typeId == types.byteProperty || typeId == types.int8Property) {
        lua_pushinteger(L, *(const int8_t*)data);
        return true;
    }
    if (typeId == types.uint16Property || typeId == types.int16Property) {
        int16_t val = 0;
        memcpy(&val, data, 2);
        lua_pushinteger(L, val);
        return true;
    }
    if (typeId == types.uint32Property) {
        uint32_t val = 0;
        memcpy(&val, data, 4);
        lua_pushnumber(L, val);
        return true;
    }
    if (typeId == types.enumProperty) {
        // Enums treated as uint8 - UE5 supports uint16/uint32/int64 backings
        // via FEnumProperty::UnderlyingProp; Phase 2 reads the actual size.
        lua_pushinteger(L, *(const uint8_t*)data);
        return true;
    }
    // String types
    if (typeId == types.strProperty) {
        // FString layout: TCHAR* Data (pointer), int32 Num, int32 Max
        void* strData = nullptr;
        readPtr((void*)data, &strData);
        if (strData) {
            const wchar_t* wstr = (const wchar_t*)strData;
            int len = 0;
            while (wstr[len] && len < 4096) len++;
            std::string utf8(len, '\0');
            for (int i = 0; i < len; i++) utf8[i] = (char)wstr[i]; // ASCII-safe narrow
            lua_pushlstring(L, utf8.c_str(), utf8.size());
        } else {
            lua_pushstring(L, "");
        }
        return true;
    }
    if (typeId == types.nameProperty) {
        // FName: resolved to its display string via getNameString. The raw
        // ComparisonIndex is meaningless to Lua callers - they want
        // `obj.Tags[1] == "EnemyAware"`, not `obj.Tags[1] == 12747`.
        uint32_t nameIdx = 0;
        readInt32((void*)data, (int32_t*)&nameIdx);
        std::string s = Engine::getNameString(nameIdx);
        lua_pushlstring(L, s.c_str(), s.size());
        return true;
    }
    // Object references
    if (typeId == types.objectProperty || typeId == types.classProperty) {
        void* ref = nullptr;
        readPtr((void*)data, &ref);
        Hydro::Lua::pushUObject(L, ref);
        return true;
    }
    // TArray. FScriptArray = { void* Data; int32 Num; int32 Max } - 16 bytes
    // at the property's offset. Recurse readToLua per element using the
    // discovered FArrayProperty::Inner. Empty / unrecoverable arrays push an
    // empty table rather than nil so callers can always iterate the result.
    if (typeId == types.arrayProperty) {
        void* arrData = nullptr;
        int32_t arrNum = 0;
        readPtr((void*)data, &arrData);
        readInt32((void*)(data + 8), &arrNum);

        lua_newtable(L);
        void* inner = Engine::getArrayInner(prop);
        if (!inner || !arrData || arrNum <= 0) return true;

        int32_t stride = Engine::getPropertyElementSize(inner);
        if (stride <= 0) return true;

        // Cap matches findAllOf's; protects against corrupt Num values that
        // would otherwise drive a multi-GB walk on bad memory.
        constexpr int32_t kArrayMax = 4096;
        if (arrNum > kArrayMax) arrNum = kArrayMax;

        for (int32_t i = 0; i < arrNum; ++i) {
            readToLua(L, inner, (const uint8_t*)arrData + (intptr_t)i * stride);
            lua_rawseti(L, -2, i + 1); // 1-indexed Lua
        }
        return true;
    }
    // Structs. When Stage H validated the FStructProperty::Struct offset,
    // walk the UScriptStruct's ChildProperties chain and recurse on each
    // field - produces real field names (FRotator -> {Pitch, Yaw, Roll}, not
    // FVector's {X, Y, Z}). Falls back to the FVector size heuristic if
    // discovery couldn't validate the offset, so non-UE-5.5 hosts still
    // decode FVector correctly.
    if (typeId == types.structProperty) {
        void* sstruct = Engine::getStructStruct(prop);
        if (sstruct) {
            lua_newtable(L);
            for (void* child = Engine::getChildProperties(sstruct);
                 child;
                 child = Engine::getNextProperty(child)) {
                int32_t coff = Engine::getPropertyOffset(child);
                if (coff < 0) continue;
                std::string fname = Engine::getFieldName(child);
                if (fname.empty()) continue;
                readToLua(L, child, data + coff);
                lua_setfield(L, -2, fname.c_str());
            }
            return true;
        }
        // Fallback: layout discovery failed -> keep the size heuristic so
        // FVector-shaped reads don't regress.
        if (size == 24) {
            lua_newtable(L);
            double x, y, z;
            memcpy(&x, data, 8);
            memcpy(&y, data + 8, 8);
            memcpy(&z, data + 16, 8);
            lua_pushnumber(L, x); lua_setfield(L, -2, "X");
            lua_pushnumber(L, y); lua_setfield(L, -2, "Y");
            lua_pushnumber(L, z); lua_setfield(L, -2, "Z");
            return true;
        }
        if (size == 12) {
            lua_newtable(L);
            float x, y, z;
            memcpy(&x, data, 4);
            memcpy(&y, data + 4, 4);
            memcpy(&z, data + 8, 4);
            lua_pushnumber(L, x); lua_setfield(L, -2, "X");
            lua_pushnumber(L, y); lua_setfield(L, -2, "Y");
            lua_pushnumber(L, z); lua_setfield(L, -2, "Z");
            return true;
        }
        lua_pushlightuserdata(L, (void*)data);
        return true;
    }

    // Unsupported type - caller sees nil.
    lua_pushnil(L);
    return false;
}

bool writeFromLua(lua_State* L, void* prop, uint8_t* data, int luaIdx) {
    using namespace Engine;
    const auto& types = getPropertyTypeNames();
    uint32_t typeId = getPropertyTypeNameIndex(prop);
    int32_t size = getPropertyElementSize(prop);

    if (!data) return false;

    if (typeId == types.intProperty) {
        int32_t val = (int32_t)luaL_checkinteger(L, luaIdx);
        memcpy(data, &val, 4);
        return true;
    }
    if (typeId == types.floatProperty) {
        float val = (float)luaL_checknumber(L, luaIdx);
        memcpy(data, &val, 4);
        return true;
    }
    if (typeId == types.doubleProperty) {
        double val = luaL_checknumber(L, luaIdx);
        memcpy(data, &val, 8);
        return true;
    }
    if (typeId == types.boolProperty) {
        uint8_t val = lua_toboolean(L, luaIdx) ? 1 : 0;
        memcpy(data, &val, 1);
        return true;
    }
    if (typeId == types.byteProperty || typeId == types.int8Property) {
        int8_t val = (int8_t)luaL_checkinteger(L, luaIdx);
        memcpy(data, &val, 1);
        return true;
    }
    if (typeId == types.objectProperty || typeId == types.classProperty) {
        void* ref = Hydro::Lua::checkUObject(L, luaIdx);
        memcpy(data, &ref, 8);
        return true;
    }
    // Param-fill at the call site needs to write small ints clamped to the
    // property's element size; preserve that behaviour for int8/16/enum/byte
    // when written via the param path (the property write path above already
    // covers them via individual branches).
    if (typeId == types.int16Property || typeId == types.uint16Property ||
        typeId == types.enumProperty) {
        int32_t val = (int32_t)luaL_checkinteger(L, luaIdx);
        memcpy(data, &val, size > 4 ? 4 : size);
        return true;
    }
    if (typeId == types.nameProperty) {
        // Mirror the read path: accept Lua strings, route through makeFName.
        size_t slen = 0;
        const char* s = luaL_checklstring(L, luaIdx, &slen);
        if (slen > 255) slen = 255;
        wchar_t wbuf[256];
        for (size_t i = 0; i < slen; ++i) wbuf[i] = (wchar_t)(uint8_t)s[i];
        wbuf[slen] = 0;
        uint32_t idx = Engine::makeFName(wbuf);
        memcpy(data, &idx, 4);
        return true;
    }
    // Struct write - accept a Lua table whose keys match the struct's field
    // names, recursively write each field through writeFromLua. Mirrors the
    // struct read path in spirit. Unspecified fields stay zero (we memset
    // before populating). Requires Stage H struct discovery - without it,
    // structs are opaque and the write fails.
    if (typeId == types.structProperty) {
        if (!lua_istable(L, luaIdx)) return false;
        void* sstruct = Engine::getStructStruct(prop);
        if (!sstruct) return false;

        // Convert negative idx to absolute so subsequent stack pushes don't
        // shift it out from under us.
        int absIdx = (luaIdx < 0) ? lua_gettop(L) + luaIdx + 1 : luaIdx;

        memset(data, 0, size);
        for (void* child = Engine::getChildProperties(sstruct);
             child;
             child = Engine::getNextProperty(child)) {
            int32_t coff = Engine::getPropertyOffset(child);
            if (coff < 0) continue;
            std::string fname = Engine::getFieldName(child);
            if (fname.empty()) continue;
            lua_getfield(L, absIdx, fname.c_str());
            if (!lua_isnil(L, -1)) {
                writeFromLua(L, child, data + coff, lua_gettop(L));
            }
            lua_pop(L, 1);
        }
        return true;
    }

    return false;
}

} // namespace Hydro::PropertyMarshal
