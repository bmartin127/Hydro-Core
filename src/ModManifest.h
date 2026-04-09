#pragma once

#include <string>
#include <vector>
#include <map>

namespace Hydro {

// Mod tier levels
// Tier 1: Sandboxed Lua - can only call Tier 2 API functions
// Tier 2: API module - provides Hydro.* Lua modules, has engine access
// Tier 3: Native - loads a DLL with full EngineAPI access

enum class ModTier { Tier1 = 1, Tier2 = 2, Tier3 = 3 };

// Network side

enum class ModSide { Both, Client, Server };

// Per-mod manifest (hydromod.toml)
// Lives alongside the mod's files:
//   AppData/Roaming/Hydro/mods/{mod-id}/{version-id}/hydromod.toml

struct ModManifest {
    // [mod]
    std::string id;
    std::string name;
    std::string version;
    std::vector<std::string> authors;
    std::string description;
    std::string license;
    ModTier tier = ModTier::Tier1;

    // [compatibility]
    std::string game;
    std::string minHydroApi;
    ModSide side = ModSide::Both;

    // [content]
    std::vector<std::string> paks;      // Pak files to mount
    std::vector<std::string> actors;    // Blueprint actors to spawn

    // [scripts]
    std::string scriptEntry;            // Lua entry point (Tier 1 & 2)

    // [api] - Tier 2 & 3 only
    std::vector<std::string> apiProvides;   // e.g. ["Hydro.Creatures"]

    // [native] - Tier 3 only
    std::string nativeEntry;            // DLL path

    // [dependencies] - mod_id -> semver range
    std::map<std::string, std::string> dependencies;

    // [optional_dependencies]
    std::map<std::string, std::string> optionalDependencies;

    // [conflicts]
    std::map<std::string, std::string> conflicts;

    // [load_order]
    std::vector<std::string> loadAfter;
    std::vector<std::string> loadBefore;

    // [config]
    std::string configDefaults;         // Path to default config file

    // [platform] - injected by platform, not authored by modder
    std::string platformProjectId;
    std::string platformVersionId;

    // Loading

    // Parse a hydromod.toml file. Returns true on success.
    bool loadFromFile(const std::string& path);

    // Validate tier-specific rules. Returns empty string if valid,
    // or an error message describing the violation.
    std::string validate() const;

    // Does this mod have a Lua script entry point?
    bool hasScripts() const { return !scriptEntry.empty(); }

    // Does this mod have Blueprint actors to spawn?
    bool hasActors() const { return !actors.empty(); }

    // Does this mod have pak content?
    bool hasPaks() const { return !paks.empty(); }
};

} // namespace Hydro
