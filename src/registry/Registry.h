#pragma once

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <nlohmann/json.hpp>

struct lua_State;

namespace Hydro {

// How a registry responds when an incoming entry collides with an existing one.
//
//   Fail     - reject the new entry, return an error to the caller
//   Warn     - reject the new entry, log a warning, return an error
//   Override - replace the existing entry, log which mod overrode which
enum class ConflictPolicy { Fail, Warn, Override };

// A single entry contributed to a registry by a mod. The 'data' field holds
// whatever the mod passed in (converted from Lua table to JSON). 'entryId'
// is always present and serves as the primary key.
struct RegistryEntry {
    std::string entryId;
    std::string sourceModId;
    nlohmann::json data;
    size_t insertionOrder = 0;
};

// Declarative description of a registry. Provided once by a Tier 2 mod when
// it calls Hydro.Registry.create.
struct RegistrySpec {
    std::string name;
    std::string providerModId;
    std::vector<std::string> requiredFields;
    std::vector<std::string> uniqueFields;
    ConflictPolicy conflictPolicy = ConflictPolicy::Fail;
    // Lua ref to an on_commit(entries) callback, or LUA_NOREF / -2 if none.
    int commitCallbackRef = -2;
};

class Registry {
public:
    explicit Registry(RegistrySpec spec);

    // Insert an entry. Returns empty string on success, error message on
    // failure. Validates required fields, unique fields, and applies the
    // configured conflict policy.
    std::string add(const std::string& sourceModId, nlohmann::json data);

    const RegistryEntry* get(const std::string& entryId) const;
    std::vector<const RegistryEntry*> all() const;
    std::vector<const RegistryEntry*> byMod(const std::string& modId) const;
    size_t size() const { return m_entries.size(); }

    const RegistrySpec& spec() const { return m_spec; }
    bool committed() const { return m_committed; }

    // Fire the on_commit(entries) callback if one was registered. Idempotent.
    void commit(lua_State* L);

private:
    RegistrySpec m_spec;
    std::unordered_map<std::string, RegistryEntry> m_entries;
    std::vector<std::string> m_insertionOrder;
    size_t m_nextOrder = 0;
    bool m_committed = false;

    bool uniqueFieldConflict(const nlohmann::json& data,
                             const std::string& excludeEntryId,
                             std::string& conflictingEntryOut,
                             std::string& conflictingFieldOut) const;
};

} // namespace Hydro
