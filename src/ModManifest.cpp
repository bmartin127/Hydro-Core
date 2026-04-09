#include "ModManifest.h"
#include "HydroCore.h"

// Disable warnings in toml++ header
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4127 4244 4267 4996)
#endif

#include <toml.hpp>

#ifdef _MSC_VER
#pragma warning(pop)
#endif

namespace Hydro {

// Helpers

template<typename T>
static std::vector<std::string> readStringArray(const toml::node_view<T>& node) {
    std::vector<std::string> result;
    if (auto* arr = node.as_array()) {
        for (const auto& elem : *arr) {
            if (auto* str = elem.as_string())
                result.push_back(str->get());
        }
    }
    return result;
}

static std::map<std::string, std::string> readStringMap(const toml::table* tbl) {
    std::map<std::string, std::string> result;
    if (!tbl) return result;
    for (const auto& [key, val] : *tbl) {
        if (auto* str = val.as_string())
            result[std::string(key.str())] = str->get();
    }
    return result;
}

// Parser

bool ModManifest::loadFromFile(const std::string& path) {
    toml::table doc;
    try {
        doc = toml::parse_file(path);
    } catch (const toml::parse_error& err) {
        logError("hydromod.toml parse error: %s", err.what());
        return false;
    }

    // [mod]
    id          = doc["mod"]["id"].value_or(std::string(""));
    name        = doc["mod"]["name"].value_or(std::string(""));
    version     = doc["mod"]["version"].value_or(std::string(""));
    description = doc["mod"]["description"].value_or(std::string(""));
    license     = doc["mod"]["license"].value_or(std::string(""));
    authors     = readStringArray(doc["mod"]["authors"]);

    int tierVal = doc["mod"]["tier"].value_or(1);
    if (tierVal >= 1 && tierVal <= 3)
        tier = static_cast<ModTier>(tierVal);

    // [compatibility]
    game        = doc["compatibility"]["game"].value_or(std::string(""));
    minHydroApi = doc["compatibility"]["min_hydro_api"].value_or(std::string(""));

    std::string sideStr = doc["compatibility"]["side"].value_or(std::string("both"));
    if (sideStr == "client") side = ModSide::Client;
    else if (sideStr == "server") side = ModSide::Server;
    else side = ModSide::Both;

    // [content]
    paks   = readStringArray(doc["content"]["paks"]);
    actors = readStringArray(doc["content"]["actors"]);

    // [scripts]
    scriptEntry = doc["scripts"]["entry"].value_or(std::string(""));

    // [api]
    apiProvides = readStringArray(doc["api"]["provides"]);

    // [native]
    nativeEntry = doc["native"]["entry"].value_or(std::string(""));

    // [dependencies]
    dependencies = readStringMap(doc["dependencies"].as_table());

    // [optional_dependencies]
    optionalDependencies = readStringMap(doc["optional_dependencies"].as_table());

    // [conflicts]
    conflicts = readStringMap(doc["conflicts"].as_table());

    // [load_order]
    loadAfter  = readStringArray(doc["load_order"]["after"]);
    loadBefore = readStringArray(doc["load_order"]["before"]);

    // [config]
    configDefaults = doc["config"]["defaults"].value_or(std::string(""));

    // [platform] - injected by platform
    platformProjectId  = doc["platform"]["project_id"].value_or(std::string(""));
    platformVersionId  = doc["platform"]["version_id"].value_or(std::string(""));

    // Required field check
    if (id.empty()) {
        logError("hydromod.toml: 'mod.id' is required");
        return false;
    }

    logInfo("Parsed hydromod.toml: '%s' v%s (tier %d)", id.c_str(), version.c_str(), (int)tier);
    return true;
}

// Validation

std::string ModManifest::validate() const {
    if (id.empty()) return "mod.id is required";
    if (version.empty()) return "mod.version is required";

    switch (tier) {
        case ModTier::Tier1:
            if (!nativeEntry.empty())
                return "Tier 1 mods cannot have [native] entry - declare tier = 3";
            if (!apiProvides.empty())
                return "Tier 1 mods cannot provide APIs - declare tier = 2";
            break;

        case ModTier::Tier2:
            if (!nativeEntry.empty())
                return "Tier 2 mods cannot have [native] entry - declare tier = 3";
            if (apiProvides.empty())
                return "Tier 2 mods must provide at least one API module in [api].provides";
            if (scriptEntry.empty())
                return "Tier 2 mods must have a [scripts].entry";
            break;

        case ModTier::Tier3:
            if (nativeEntry.empty())
                return "Tier 3 mods must have a [native].entry";
            break;
    }

    return ""; // Valid
}

} // namespace Hydro
