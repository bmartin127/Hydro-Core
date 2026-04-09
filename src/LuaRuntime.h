#pragma once

#include <string>
#include <vector>
#include <functional>

struct lua_State;

namespace Hydro {

enum class ModTier;

/// LuaRuntime - manages LuaJIT state for mod script execution.
///
/// Architecture:
///   - One lua_State shared by all mods
///   - Each mod gets its own environment table (isolated globals)
///   - Tier 1: sandboxed (no io/os/debug/ffi)
///   - Tier 2: privileged (has ffi, EngineAPI bindings)
///   - Tier 3: uses native DLLs, may optionally have Lua helpers

class LuaRuntime {
public:
    LuaRuntime();
    ~LuaRuntime();

    // Initialize the Lua state. Call once.
    bool initialize();

    // Is the runtime ready?
    bool isReady() const { return m_state != nullptr; }

    // Execute a Lua script file for a mod.
    // modId: used for error messages and environment isolation
    // scriptPath: absolute path to the .lua file
    // tier: determines sandbox level
    bool executeModScript(const std::string& modId, const std::string& scriptPath, ModTier tier);

    // Register a C function as a global Lua function (available to all mods).
    // Used by Tier 2 modules to register API functions.
    void registerGlobal(const char* name, int(*func)(lua_State*));

    // Register a module table (e.g., "Hydro.Assets") that mods can require().
    // The callback should push the module table onto the Lua stack.
    void registerModule(const char* moduleName, std::function<void(lua_State*)> initializer);

    lua_State* state() { return m_state; }

private:
    // Create a sandboxed environment for a Tier 1 mod
    void createTier1Environment(const std::string& modId);

    // Create a privileged environment for a Tier 2 mod
    void createTier2Environment(const std::string& modId);

    // Set up the base safe globals (math, string, table, etc.)
    void setupSafeGlobals();

    lua_State* m_state = nullptr;
};

} // namespace Hydro
