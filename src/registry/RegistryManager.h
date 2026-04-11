#pragma once

#include "Registry.h"
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

struct lua_State;

namespace Hydro {

// Process-wide singleton that owns every Registry by name. Also tracks which
// Tier 2 mods claim which apiProvides entries, so Hydro.Registry.create can
// refuse mods that try to create a registry they don't own.
class RegistryManager {
public:
    static RegistryManager& instance();

    // Populated by the mod loader during LoadMods, before any Lua scripts run.
    void setModProvides(const std::string& modId, std::vector<std::string> provides);
    bool modProvides(const std::string& modId, const std::string& apiName) const;

    // Create a registry if one with this name does not already exist.
    // Returns nullptr if the name is already taken.
    Registry* create(RegistrySpec spec);

    Registry* find(const std::string& name);
    std::vector<Registry*> all();

    // Fire on_commit for each registry in creation order.
    void commitAll(lua_State* L);

    // Log a summary of every registry and its entries.
    void dumpAll() const;

    // Reset state - used only if HydroCore ever reloads mods at runtime.
    void reset();

    bool committedPhase() const { return m_committed; }

private:
    RegistryManager() = default;

    std::unordered_map<std::string, std::unique_ptr<Registry>> m_registries;
    std::vector<std::string> m_creationOrder;
    std::unordered_map<std::string, std::vector<std::string>> m_modProvides;
    bool m_committed = false;
};

} // namespace Hydro
