#include "HydroRegistry.h"
#include "ModuleRegistry.h"
#include "../HydroCore.h"
#include "../registry/RegistryManager.h"

extern "C" {
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>
}

#include <nlohmann/json.hpp>
#include <string>

namespace Hydro::API {

static const char* kRegistryMetatable = "Hydro.Registry.Handle";

// Lua <-> JSON conversion

// Convert a Lua table at absolute stack index 'idx' to nlohmann::json.
// Mixed-key tables become objects with stringified keys.
static nlohmann::json luaToJson(lua_State* L, int idx);

static nlohmann::json luaValueToJson(lua_State* L, int idx) {
    int type = lua_type(L, idx);
    switch (type) {
        case LUA_TNIL:     return nullptr;
        case LUA_TBOOLEAN: return lua_toboolean(L, idx) != 0;
        case LUA_TNUMBER:  return lua_tonumber(L, idx);
        case LUA_TSTRING:  return std::string(lua_tostring(L, idx));
        case LUA_TTABLE:   return luaToJson(L, idx);
        default:
            // Unsupported (function, userdata, thread) - stringify.
            return std::string("<") + lua_typename(L, type) + ">";
    }
}

// LuaJIT is Lua 5.1 - no lua_absindex. Convert relative to absolute manually.
static int absIndex(lua_State* L, int idx) {
    if (idx > 0 || idx <= LUA_REGISTRYINDEX) return idx;
    return lua_gettop(L) + idx + 1;
}

static nlohmann::json luaToJson(lua_State* L, int idx) {
    idx = absIndex(L, idx);

    // Detect array vs object: if all keys are positive integers 1..n, treat
    // as array; otherwise object.
    bool isArray = true;
    size_t arrayLen = 0;
    lua_pushnil(L);
    while (lua_next(L, idx) != 0) {
        if (lua_type(L, -2) != LUA_TNUMBER) {
            isArray = false;
        } else {
            double k = lua_tonumber(L, -2);
            if (k < 1 || k != (double)(int)k) isArray = false;
            else if ((size_t)k > arrayLen) arrayLen = (size_t)k;
        }
        lua_pop(L, 1);
        if (!isArray) break;
    }
    if (!isArray) {
        // Finish iteration if we bailed
        lua_pushnil(L);
        while (lua_next(L, idx) != 0) lua_pop(L, 1);
    }

    if (isArray && arrayLen > 0) {
        nlohmann::json arr = nlohmann::json::array();
        for (size_t i = 1; i <= arrayLen; i++) {
            lua_rawgeti(L, idx, (int)i);
            arr.push_back(luaValueToJson(L, -1));
            lua_pop(L, 1);
        }
        return arr;
    }

    nlohmann::json obj = nlohmann::json::object();
    lua_pushnil(L);
    while (lua_next(L, idx) != 0) {
        std::string key;
        if (lua_type(L, -2) == LUA_TSTRING) {
            key = lua_tostring(L, -2);
        } else {
            // Coerce without mutating the key on the stack
            lua_pushvalue(L, -2);
            key = lua_tostring(L, -1) ? lua_tostring(L, -1) : "";
            lua_pop(L, 1);
        }
        obj[key] = luaValueToJson(L, -1);
        lua_pop(L, 1);
    }
    return obj;
}

static void pushJsonToLua(lua_State* L, const nlohmann::json& j);

static void pushJsonToLua(lua_State* L, const nlohmann::json& j) {
    if (j.is_null())         lua_pushnil(L);
    else if (j.is_boolean()) lua_pushboolean(L, j.get<bool>() ? 1 : 0);
    else if (j.is_number())  lua_pushnumber(L, j.get<double>());
    else if (j.is_string())  lua_pushstring(L, j.get<std::string>().c_str());
    else if (j.is_array()) {
        lua_createtable(L, (int)j.size(), 0);
        int i = 1;
        for (const auto& item : j) {
            pushJsonToLua(L, item);
            lua_rawseti(L, -2, i++);
        }
    } else if (j.is_object()) {
        lua_createtable(L, 0, (int)j.size());
        for (auto it = j.begin(); it != j.end(); ++it) {
            pushJsonToLua(L, it.value());
            lua_setfield(L, -2, it.key().c_str());
        }
    } else {
        lua_pushnil(L);
    }
}

// Read _MOD_ID from the currently running Lua chunk's environment.
// Returns empty string if not available.
static std::string getCallerModId(lua_State* L) {
    lua_Debug ar;
    // Walk up the stack until we find a Lua frame with an env containing _MOD_ID.
    for (int level = 1; level < 16; level++) {
        if (lua_getstack(L, level, &ar) == 0) break;
        if (lua_getinfo(L, "f", &ar) == 0) continue;
        if (!lua_isfunction(L, -1)) { lua_pop(L, 1); continue; }
        lua_getfenv(L, -1);
        if (lua_istable(L, -1)) {
            lua_getfield(L, -1, "_MOD_ID");
            if (lua_isstring(L, -1)) {
                std::string modId = lua_tostring(L, -1);
                lua_pop(L, 3); // _MOD_ID, env, function
                return modId;
            }
            lua_pop(L, 1); // _MOD_ID (not a string)
        }
        lua_pop(L, 2); // env, function
    }
    return "";
}

// Registry handle userdata

struct RegistryHandle {
    Registry* reg;
};

static Registry* checkRegistryHandle(lua_State* L, int idx) {
    void* ud = luaL_checkudata(L, idx, kRegistryMetatable);
    auto* handle = static_cast<RegistryHandle*>(ud);
    if (!handle || !handle->reg) {
        luaL_error(L, "invalid registry handle");
    }
    return handle->reg;
}

static void pushRegistryHandle(lua_State* L, Registry* reg) {
    auto* handle = static_cast<RegistryHandle*>(
        lua_newuserdata(L, sizeof(RegistryHandle)));
    handle->reg = reg;
    luaL_getmetatable(L, kRegistryMetatable);
    lua_setmetatable(L, -2);
}

// Userdata methods

static int l_reg_register(lua_State* L) {
    Registry* reg = checkRegistryHandle(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);

    std::string modId = getCallerModId(L);
    if (modId.empty()) modId = "<unknown>";

    nlohmann::json data = luaToJson(L, 2);
    std::string err = reg->add(modId, std::move(data));
    if (!err.empty()) {
        lua_pushnil(L);
        lua_pushstring(L, err.c_str());
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int l_reg_get(lua_State* L) {
    Registry* reg = checkRegistryHandle(L, 1);
    const char* id = luaL_checkstring(L, 2);
    const RegistryEntry* entry = reg->get(id);
    if (!entry) {
        lua_pushnil(L);
        return 1;
    }
    pushJsonToLua(L, entry->data);
    // Also tack on _mod_id for traceability.
    if (lua_istable(L, -1)) {
        lua_pushstring(L, entry->sourceModId.c_str());
        lua_setfield(L, -2, "_mod_id");
    }
    return 1;
}

static int l_reg_all(lua_State* L) {
    Registry* reg = checkRegistryHandle(L, 1);
    auto entries = reg->all();
    lua_createtable(L, (int)entries.size(), 0);
    for (size_t i = 0; i < entries.size(); i++) {
        pushJsonToLua(L, entries[i]->data);
        if (lua_istable(L, -1)) {
            lua_pushstring(L, entries[i]->sourceModId.c_str());
            lua_setfield(L, -2, "_mod_id");
        }
        lua_rawseti(L, -2, (int)i + 1);
    }
    return 1;
}

static int l_reg_by_mod(lua_State* L) {
    Registry* reg = checkRegistryHandle(L, 1);
    const char* modId = luaL_checkstring(L, 2);
    auto entries = reg->byMod(modId);
    lua_createtable(L, (int)entries.size(), 0);
    for (size_t i = 0; i < entries.size(); i++) {
        pushJsonToLua(L, entries[i]->data);
        lua_rawseti(L, -2, (int)i + 1);
    }
    return 1;
}

static int l_reg_size(lua_State* L) {
    Registry* reg = checkRegistryHandle(L, 1);
    lua_pushinteger(L, (lua_Integer)reg->size());
    return 1;
}

static int l_reg_name(lua_State* L) {
    Registry* reg = checkRegistryHandle(L, 1);
    lua_pushstring(L, reg->spec().name.c_str());
    return 1;
}

static const luaL_Reg registry_methods[] = {
    {"register", l_reg_register},
    {"get",      l_reg_get},
    {"all",      l_reg_all},
    {"by_mod",   l_reg_by_mod},
    {"size",     l_reg_size},
    {"name",     l_reg_name},
    {nullptr,    nullptr}
};

// Module functions

static ConflictPolicy parsePolicy(const std::string& s) {
    if (s == "warn")     return ConflictPolicy::Warn;
    if (s == "override") return ConflictPolicy::Override;
    return ConflictPolicy::Fail;
}

static std::vector<std::string> readStringArray(lua_State* L, int tableIdx, const char* field) {
    std::vector<std::string> result;
    lua_getfield(L, tableIdx, field);
    if (lua_istable(L, -1)) {
        int n = (int)lua_objlen(L, -1);
        for (int i = 1; i <= n; i++) {
            lua_rawgeti(L, -1, i);
            if (lua_isstring(L, -1)) result.emplace_back(lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    return result;
}

static int l_registry_create(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);

    lua_getfield(L, 1, "name");
    if (!lua_isstring(L, -1)) {
        return luaL_error(L, "Hydro.Registry.create: 'name' is required (string)");
    }
    std::string name = lua_tostring(L, -1);
    lua_pop(L, 1);

    std::string modId = getCallerModId(L);
    if (modId.empty()) {
        return luaL_error(L, "Hydro.Registry.create: cannot determine calling mod");
    }

    auto& mgr = RegistryManager::instance();
    if (!mgr.modProvides(modId, name)) {
        return luaL_error(L,
            "Hydro.Registry.create: mod '%s' does not declare apiProvides = [\"%s\"] in hydromod.toml",
            modId.c_str(), name.c_str());
    }

    if (mgr.find(name) != nullptr) {
        return luaL_error(L, "Hydro.Registry.create: registry '%s' already exists", name.c_str());
    }

    RegistrySpec spec;
    spec.name = name;
    spec.providerModId = modId;
    spec.requiredFields = readStringArray(L, 1, "required");
    spec.uniqueFields = readStringArray(L, 1, "unique");

    lua_getfield(L, 1, "conflict");
    if (lua_isstring(L, -1)) {
        spec.conflictPolicy = parsePolicy(lua_tostring(L, -1));
    }
    lua_pop(L, 1);

    lua_getfield(L, 1, "on_commit");
    if (lua_isfunction(L, -1)) {
        spec.commitCallbackRef = luaL_ref(L, LUA_REGISTRYINDEX);
    } else {
        lua_pop(L, 1);
    }

    Registry* reg = mgr.create(std::move(spec));
    if (!reg) {
        return luaL_error(L, "Hydro.Registry.create: failed to create '%s'", name.c_str());
    }

    pushRegistryHandle(L, reg);
    return 1;
}

static int l_registry_publish(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    checkRegistryHandle(L, 2);

    // Store the userdata in _HYDRO_MODULES (Tier 1 safe require path).
    lua_getfield(L, LUA_REGISTRYINDEX, "_HYDRO_MODULES");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return luaL_error(L, "Hydro.Registry.publish: module registry unavailable");
    }
    lua_pushvalue(L, 2);
    lua_setfield(L, -2, name);
    lua_pop(L, 1);

    // Also store in package.loaded so Tier 2 mods using stock require() find it.
    lua_getglobal(L, "package");
    if (lua_istable(L, -1)) {
        lua_getfield(L, -1, "loaded");
        if (lua_istable(L, -1)) {
            lua_pushvalue(L, 2);
            lua_setfield(L, -2, name);
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);

    logInfo("[Registry] published '%s' - consumers can now require() it", name);
    return 0;
}

static int l_registry_get(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    Registry* reg = RegistryManager::instance().find(name);
    if (!reg) {
        lua_pushnil(L);
        return 1;
    }
    pushRegistryHandle(L, reg);
    return 1;
}

static int l_registry_list(lua_State* L) {
    auto registries = RegistryManager::instance().all();
    lua_createtable(L, (int)registries.size(), 0);
    for (size_t i = 0; i < registries.size(); i++) {
        lua_pushstring(L, registries[i]->spec().name.c_str());
        lua_rawseti(L, -2, (int)i + 1);
    }
    return 1;
}

static int l_registry_dump(lua_State* L) {
    (void)L;
    RegistryManager::instance().dumpAll();
    return 0;
}

static const luaL_Reg module_functions[] = {
    {"create",  l_registry_create},
    {"publish", l_registry_publish},
    {"get",     l_registry_get},
    {"list",    l_registry_list},
    {"dump",    l_registry_dump},
    {nullptr,   nullptr}
};

void registerRegistryModule(lua_State* L) {
    // Create the handle metatable once. __index points at the methods table.
    if (luaL_newmetatable(L, kRegistryMetatable)) {
        lua_newtable(L);
        for (const luaL_Reg* fn = registry_methods; fn->name; fn++) {
            lua_pushcfunction(L, fn->func);
            lua_setfield(L, -2, fn->name);
        }
        lua_setfield(L, -2, "__index");
    }
    lua_pop(L, 1); // metatable

    buildModuleTable(L, module_functions);
}

} // namespace Hydro::API
