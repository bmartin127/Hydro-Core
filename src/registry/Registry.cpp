#include "Registry.h"
#include "../HydroCore.h"

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

namespace Hydro {

Registry::Registry(RegistrySpec spec) : m_spec(std::move(spec)) {
    // 'id' is always required - it's the primary key.
    bool hasId = false;
    for (const auto& f : m_spec.requiredFields) {
        if (f == "id") { hasId = true; break; }
    }
    if (!hasId) m_spec.requiredFields.insert(m_spec.requiredFields.begin(), "id");
}

std::string Registry::add(const std::string& sourceModId, nlohmann::json data) {
    if (m_committed) {
        return "registry '" + m_spec.name + "' is already committed, cannot add entries";
    }
    if (!data.is_object()) {
        return "entry must be a table";
    }

    // Required fields
    for (const auto& field : m_spec.requiredFields) {
        if (!data.contains(field) || data[field].is_null()) {
            return "missing required field '" + field + "'";
        }
    }

    // Extract id
    if (!data["id"].is_string()) {
        return "field 'id' must be a string";
    }
    std::string entryId = data["id"].get<std::string>();
    if (entryId.empty()) {
        return "field 'id' must not be empty";
    }

    // Duplicate id check
    auto existing = m_entries.find(entryId);
    if (existing != m_entries.end()) {
        std::string msg = "duplicate id '" + entryId + "' in registry '" + m_spec.name
                        + "' (already registered by '" + existing->second.sourceModId + "')";
        switch (m_spec.conflictPolicy) {
            case ConflictPolicy::Fail:
                return msg;
            case ConflictPolicy::Warn:
                logWarn("[Registry] %s", msg.c_str());
                return msg;
            case ConflictPolicy::Override:
                logInfo("[Registry] '%s' override: '%s' from '%s' replaces entry from '%s'",
                        m_spec.name.c_str(), entryId.c_str(),
                        sourceModId.c_str(), existing->second.sourceModId.c_str());
                // fall through to insert below, overwriting
                break;
        }
    }

    // Unique field check
    std::string conflictingEntry, conflictingField;
    if (uniqueFieldConflict(data, entryId, conflictingEntry, conflictingField)) {
        std::string msg = "unique field '" + conflictingField + "' collides with entry '"
                        + conflictingEntry + "' in registry '" + m_spec.name + "'";
        switch (m_spec.conflictPolicy) {
            case ConflictPolicy::Fail:
                return msg;
            case ConflictPolicy::Warn:
                logWarn("[Registry] %s", msg.c_str());
                return msg;
            case ConflictPolicy::Override:
                logInfo("[Registry] '%s' override on unique field '%s'",
                        m_spec.name.c_str(), conflictingField.c_str());
                break;
        }
    }

    RegistryEntry entry;
    entry.entryId = entryId;
    entry.sourceModId = sourceModId;
    entry.data = std::move(data);
    entry.insertionOrder = m_nextOrder++;

    // If this is a fresh insertion (not an override of an existing key), track order.
    if (existing == m_entries.end()) {
        m_insertionOrder.push_back(entryId);
    }
    m_entries[entryId] = std::move(entry);

    logInfo("[Registry] '%s' add '%s' from '%s'",
            m_spec.name.c_str(), entryId.c_str(), sourceModId.c_str());
    return "";
}

const RegistryEntry* Registry::get(const std::string& entryId) const {
    auto it = m_entries.find(entryId);
    return it == m_entries.end() ? nullptr : &it->second;
}

std::vector<const RegistryEntry*> Registry::all() const {
    std::vector<const RegistryEntry*> result;
    result.reserve(m_insertionOrder.size());
    for (const auto& id : m_insertionOrder) {
        auto it = m_entries.find(id);
        if (it != m_entries.end()) result.push_back(&it->second);
    }
    return result;
}

std::vector<const RegistryEntry*> Registry::byMod(const std::string& modId) const {
    std::vector<const RegistryEntry*> result;
    for (const auto& id : m_insertionOrder) {
        auto it = m_entries.find(id);
        if (it != m_entries.end() && it->second.sourceModId == modId) {
            result.push_back(&it->second);
        }
    }
    return result;
}

bool Registry::uniqueFieldConflict(const nlohmann::json& data,
                                   const std::string& excludeEntryId,
                                   std::string& conflictingEntryOut,
                                   std::string& conflictingFieldOut) const {
    for (const auto& field : m_spec.uniqueFields) {
        if (!data.contains(field)) continue;
        const auto& value = data[field];
        for (const auto& [id, entry] : m_entries) {
            if (id == excludeEntryId) continue;
            if (entry.data.contains(field) && entry.data[field] == value) {
                conflictingEntryOut = id;
                conflictingFieldOut = field;
                return true;
            }
        }
    }
    return false;
}

void Registry::commit(lua_State* L) {
    if (m_committed) return;
    m_committed = true;

    if (m_spec.commitCallbackRef == -2 /* LUA_NOREF */) {
        return;
    }

    // Push the callback
    lua_rawgeti(L, LUA_REGISTRYINDEX, m_spec.commitCallbackRef);
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        return;
    }

    // Build the entries array as a Lua table. Each entry is itself a table
    // with the original fields plus _mod_id.
    lua_createtable(L, (int)m_insertionOrder.size(), 0);
    int tableIdx = lua_gettop(L);

    int i = 1;
    for (const auto& id : m_insertionOrder) {
        auto it = m_entries.find(id);
        if (it == m_entries.end()) continue;
        const auto& entry = it->second;

        lua_createtable(L, 0, (int)entry.data.size() + 1);

        // Copy JSON fields into the entry table
        for (auto jt = entry.data.begin(); jt != entry.data.end(); ++jt) {
            const auto& key = jt.key();
            const auto& val = jt.value();
            if (val.is_string())      lua_pushstring(L, val.get<std::string>().c_str());
            else if (val.is_boolean()) lua_pushboolean(L, val.get<bool>() ? 1 : 0);
            else if (val.is_number())  lua_pushnumber(L, val.get<double>());
            else if (val.is_null())    lua_pushnil(L);
            else                       lua_pushstring(L, val.dump().c_str());
            lua_setfield(L, -2, key.c_str());
        }

        lua_pushstring(L, entry.sourceModId.c_str());
        lua_setfield(L, -2, "_mod_id");

        lua_rawseti(L, tableIdx, i++);
    }

    // Call on_commit(entries)
    if (lua_pcall(L, 1, 0, 0) != 0) {
        const char* err = lua_tostring(L, -1);
        logError("[Registry] '%s' on_commit error: %s",
                 m_spec.name.c_str(), err ? err : "unknown");
        lua_pop(L, 1);
    }
}

} // namespace Hydro
