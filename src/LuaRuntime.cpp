#include "LuaRuntime.h"
#include "HydroCore.h"
#include "EngineAPI.h"
#include "ModManifest.h"
#include "LuaUObject.h"

extern "C" {
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>
}

#include <fstream>
#include <sstream>

namespace Hydro {

// Lua print override - routes to HydroCore logging

static int l_hydro_print(lua_State* L) {
    int n = lua_gettop(L);
    std::string msg;
    for (int i = 1; i <= n; i++) {
        if (i > 1) msg += "\t";
        // LuaJIT is Lua 5.1 - use lua_tostring instead of luaL_tolstring
        lua_getglobal(L, "tostring");
        lua_pushvalue(L, i);
        lua_call(L, 1, 1);
        const char* s = lua_tostring(L, -1);
        if (s) msg += s;
        lua_pop(L, 1);
    }
    Hydro::logInfo("[Lua] %s", msg.c_str());
    return 0;
}

static int l_hydro_warn(lua_State* L) {
    const char* msg = luaL_checkstring(L, 1);
    Hydro::logWarn("[Lua] %s", msg);
    return 0;
}

static int l_hydro_error_log(lua_State* L) {
    const char* msg = luaL_checkstring(L, 1);
    Hydro::logError("[Lua] %s", msg);
    return 0;
}

// Sandboxed require - only allows approved modules

static int l_safe_require(lua_State* L) {
    const char* modname = luaL_checkstring(L, 1);

    // Look up in the preloaded modules table
    lua_getfield(L, LUA_REGISTRYINDEX, "_HYDRO_MODULES");
    if (lua_istable(L, -1)) {
        lua_getfield(L, -1, modname);
        if (!lua_isnil(L, -1)) {
            return 1; // Return the module
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);

    return luaL_error(L, "module '%s' not available - check dependencies in hydromod.toml", modname);
}

// LuaRuntime

LuaRuntime::LuaRuntime() = default;

LuaRuntime::~LuaRuntime() {
    if (m_state) {
        lua_close(m_state);
        m_state = nullptr;
    }
}

bool LuaRuntime::initialize() {
    if (m_state) return true;

    m_state = luaL_newstate();
    if (!m_state) {
        logError("LuaRuntime: Failed to create Lua state");
        return false;
    }

    // Open standard libraries (we'll restrict per-mod via environments)
    luaL_openlibs(m_state);

    // Create module registry for safe require()
    lua_newtable(m_state);
    lua_setfield(m_state, LUA_REGISTRYINDEX, "_HYDRO_MODULES");

    // Create mod environments registry
    lua_newtable(m_state);
    lua_setfield(m_state, LUA_REGISTRYINDEX, "_HYDRO_ENVS");

    // Initialize UObject metatable for reflection
    Lua::initUObjectMetatable(m_state);

    // Initialize property type names for type dispatch
    Engine::initPropertyTypeNames();

    logInfo("LuaRuntime: Initialized (LuaJIT + UObject reflection)");
    return true;
}

void LuaRuntime::setupSafeGlobals() {
    // This is called when building a Tier 1 environment.
    // The safe globals are: math, string, table, pairs, ipairs,
    // type, tostring, tonumber, select, unpack, error, pcall, xpcall
    // Dangerous globals (removed for Tier 1): io, os, debug, ffi,
    // loadfile, dofile, load, rawget, rawset, rawequal, rawlen,
    // collectgarbage, newproxy, getfenv, setfenv
}

void LuaRuntime::createTier1Environment(const std::string& modId) {
    lua_State* L = m_state;

    // Create a new environment table for this mod
    lua_newtable(L); // env

    // Copy safe globals from _G
    const char* safeGlobals[] = {
        "math", "string", "table", "coroutine",
        "pairs", "ipairs", "next",
        "type", "tostring", "tonumber",
        "select", "unpack", "error",
        "pcall", "xpcall", "assert",
        nullptr
    };

    for (int i = 0; safeGlobals[i]; i++) {
        lua_getglobal(L, safeGlobals[i]);
        lua_setfield(L, -2, safeGlobals[i]);
    }

    // Override print with our logging version
    lua_pushcfunction(L, l_hydro_print);
    lua_setfield(L, -2, "print");

    // Add Hydro logging helpers
    lua_pushcfunction(L, l_hydro_warn);
    lua_setfield(L, -2, "warn");

    lua_pushcfunction(L, l_hydro_error_log);
    lua_setfield(L, -2, "log_error");

    // Safe require - only loads registered Hydro modules
    lua_pushcfunction(L, l_safe_require);
    lua_setfield(L, -2, "require");

    // Store mod ID in the environment
    lua_pushstring(L, modId.c_str());
    lua_setfield(L, -2, "_MOD_ID");

    // Store in the environments registry
    lua_getfield(L, LUA_REGISTRYINDEX, "_HYDRO_ENVS");
    lua_pushvalue(L, -2); // copy env
    lua_setfield(L, -2, modId.c_str());
    lua_pop(L, 1); // pop _HYDRO_ENVS

    // env is now on top of stack
}

void LuaRuntime::createTier2Environment(const std::string& modId) {
    lua_State* L = m_state;

    // Tier 2 gets full Lua access (including ffi)
    lua_newtable(L); // env

    // Set metatable to fall through to _G for anything not overridden
    lua_newtable(L); // metatable
    lua_pushvalue(L, LUA_GLOBALSINDEX);
    lua_setfield(L, -2, "__index");
    lua_setmetatable(L, -2);

    // Override print
    lua_pushcfunction(L, l_hydro_print);
    lua_setfield(L, -2, "print");

    // Store mod ID
    lua_pushstring(L, modId.c_str());
    lua_setfield(L, -2, "_MOD_ID");

    // Store in registry
    lua_getfield(L, LUA_REGISTRYINDEX, "_HYDRO_ENVS");
    lua_pushvalue(L, -2);
    lua_setfield(L, -2, modId.c_str());
    lua_pop(L, 1);
}

bool LuaRuntime::executeModScript(const std::string& modId, const std::string& scriptPath, ModTier tier) {
    if (!m_state) {
        logError("LuaRuntime: Not initialized");
        return false;
    }

    // Read script file
    std::ifstream file(scriptPath);
    if (!file.is_open()) {
        logError("LuaRuntime: Cannot open script: %s", scriptPath.c_str());
        return false;
    }
    std::stringstream buf;
    buf << file.rdbuf();
    std::string code = buf.str();

    // Create appropriate environment
    if (tier == ModTier::Tier1) {
        createTier1Environment(modId);
    } else {
        createTier2Environment(modId);
    }
    // env is now on top of stack

    // Load the script
    std::string chunkName = "@" + modId + "/" + scriptPath.substr(scriptPath.find_last_of("/\\") + 1);
    int loadResult = luaL_loadbuffer(m_state, code.c_str(), code.size(), chunkName.c_str());
    if (loadResult != 0) {
        const char* err = lua_tostring(m_state, -1);
        logError("LuaRuntime: [%s] Load error: %s", modId.c_str(), err ? err : "unknown");
        lua_pop(m_state, 2); // error + env
        return false;
    }

    // Set the environment on the loaded chunk
    lua_pushvalue(m_state, -2); // push env
    lua_setfenv(m_state, -2);   // setfenv(chunk, env)

    // Remove env from under the chunk
    lua_remove(m_state, -2);

    // Execute
    int execResult = lua_pcall(m_state, 0, 0, 0);
    if (execResult != 0) {
        const char* err = lua_tostring(m_state, -1);
        logError("LuaRuntime: [%s] Runtime error: %s", modId.c_str(), err ? err : "unknown");
        lua_pop(m_state, 1);
        return false;
    }

    logInfo("LuaRuntime: [%s] Script executed successfully", modId.c_str());
    return true;
}

void LuaRuntime::registerGlobal(const char* name, int(*func)(lua_State*)) {
    if (!m_state) return;
    lua_pushcfunction(m_state, func);
    lua_setglobal(m_state, name);
}

void LuaRuntime::registerModule(const char* moduleName, std::function<void(lua_State*)> initializer) {
    if (!m_state) return;

    // Call the initializer to push the module table
    initializer(m_state);

    // Store in the modules registry
    lua_getfield(m_state, LUA_REGISTRYINDEX, "_HYDRO_MODULES");
    lua_pushvalue(m_state, -2); // copy module table
    lua_setfield(m_state, -2, moduleName);
    lua_pop(m_state, 2); // pop registry + module table

    logInfo("LuaRuntime: Registered module '%s'", moduleName);
}

} // namespace Hydro
