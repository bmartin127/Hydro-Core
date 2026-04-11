#include "RegistryManager.h"
#include "../HydroCore.h"

namespace Hydro {

RegistryManager& RegistryManager::instance() {
    static RegistryManager s_instance;
    return s_instance;
}

void RegistryManager::setModProvides(const std::string& modId,
                                     std::vector<std::string> provides) {
    m_modProvides[modId] = std::move(provides);
}

bool RegistryManager::modProvides(const std::string& modId,
                                  const std::string& apiName) const {
    auto it = m_modProvides.find(modId);
    if (it == m_modProvides.end()) return false;
    for (const auto& p : it->second) {
        if (p == apiName) return true;
    }
    return false;
}

Registry* RegistryManager::create(RegistrySpec spec) {
    if (m_committed) {
        logError("[Registry] cannot create '%s' after commit phase", spec.name.c_str());
        return nullptr;
    }
    if (m_registries.find(spec.name) != m_registries.end()) {
        return nullptr;
    }
    std::string name = spec.name;
    std::string provider = spec.providerModId;
    auto reg = std::make_unique<Registry>(std::move(spec));
    Registry* raw = reg.get();
    m_registries[name] = std::move(reg);
    m_creationOrder.push_back(name);
    logInfo("[Registry] created '%s' (provider=%s)", name.c_str(), provider.c_str());
    return raw;
}

Registry* RegistryManager::find(const std::string& name) {
    auto it = m_registries.find(name);
    return it == m_registries.end() ? nullptr : it->second.get();
}

std::vector<Registry*> RegistryManager::all() {
    std::vector<Registry*> result;
    result.reserve(m_creationOrder.size());
    for (const auto& name : m_creationOrder) {
        auto it = m_registries.find(name);
        if (it != m_registries.end()) result.push_back(it->second.get());
    }
    return result;
}

void RegistryManager::commitAll(lua_State* L) {
    if (m_committed) return;
    m_committed = true;
    logInfo("[Registry] commitAll - %zu registries", m_creationOrder.size());
    for (const auto& name : m_creationOrder) {
        auto it = m_registries.find(name);
        if (it != m_registries.end()) it->second->commit(L);
    }
}

void RegistryManager::dumpAll() const {
    logInfo("[Registry] === dump ===");
    for (const auto& name : m_creationOrder) {
        auto it = m_registries.find(name);
        if (it == m_registries.end()) continue;
        const auto& reg = *it->second;
        logInfo("[Registry]   %s (provider=%s, entries=%zu, policy=%d)",
                name.c_str(),
                reg.spec().providerModId.c_str(),
                reg.size(),
                (int)reg.spec().conflictPolicy);
        auto entries = reg.all();
        size_t limit = entries.size() < 10 ? entries.size() : 10;
        for (size_t i = 0; i < limit; i++) {
            logInfo("[Registry]     - %s (from %s)",
                    entries[i]->entryId.c_str(),
                    entries[i]->sourceModId.c_str());
        }
        if (entries.size() > limit) {
            logInfo("[Registry]     ... and %zu more", entries.size() - limit);
        }
    }
}

void RegistryManager::reset() {
    m_registries.clear();
    m_creationOrder.clear();
    m_modProvides.clear();
    m_committed = false;
}

} // namespace Hydro
