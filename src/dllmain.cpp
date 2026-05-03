#include <Mod/CppUserModBase.hpp>
#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/Hooks/Hooks.hpp>
#include "HydroCore.h"
#include "EngineAPI.h"
#include "Manifest.h"
#include "ModManifest.h"
#include "LuaRuntime.h"
#include "api/HydroAssets.h"
#include "api/HydroWorld.h"
#include "api/HydroEvents.h"
#include "api/HydroRegistry.h"
#include "api/HydroReflect.h"
#include "api/HydroNet.h"
#include "registry/RegistryManager.h"
#include <filesystem>
#include <windows.h>

/*
 * dllmain.cpp: UE4SS lifecycle plumbing.
 *
 * UE4SS is used for:
 *   - start_mod() / uninstall_mod() - DLL injection entry points
 *   - on_unreal_init() - notification that the engine is ready
 *   - RegisterEngineTickPostCallback() - polling until the world is ready
 *   - RC::Output::send() - log routing into the UE4SS console
 *
 * Everything that touches the engine itself lives in EngineAPI.
 */

static void ue4ssLogCallback(Hydro::LogLevel level, const char* message) {
    std::wstring wide(message, message + strlen(message));
    std::wstring prefix = L"[HydroCore] ";
    switch (level) {
        case Hydro::LogLevel::Warn:  prefix += L"[WARN] ";  break;
        case Hydro::LogLevel::Error: prefix += L"[ERROR] "; break;
        default:                     prefix += L"[INFO] ";  break;
    }
    RC::Output::send<RC::LogLevel::Normal>(prefix + wide + L"\n");
}

// State

static Hydro::Core* s_core = nullptr;
static bool s_engineReady = false;
static bool s_modsLoaded = false;
static Hydro::LuaRuntime s_lua;

// Mod loading pipeline
// 1. Read hydro_mods.json (launcher manifest) to find installed mods
// 2. For each mod, read its hydromod.toml (mod manifest) for load config
// 3. Mount paks, spawn actors, run scripts (future: Lua)
// Load order: Tier 3 -> Tier 2 -> Tier 1

static void LoadMods() {
    if (s_modsLoaded) return;

    // EngineAPI bootstrap
    if (!s_engineReady) {
        if (!Hydro::Engine::initialize()) {
            return; // Not ready yet - will retry next tick
        }
        s_engineReady = true;
        Hydro::logInfo("EngineAPI ready");
    }

    const Hydro::Manifest* manifest = s_core ? s_core->getManifest() : nullptr;
    if (!manifest || manifest->getModCount() == 0) {
        s_modsLoaded = true;
        return;
    }

    // Collect mod manifests, sorted by tier (3 -> 2 -> 1)
    struct LoadableMod {
        Hydro::ModManifest manifest;
        std::string baseDir; // Directory containing the mod files
    };
    std::vector<LoadableMod> mods;

    for (const auto& entry : manifest->getMods()) {
        if (!entry.enabled) continue;

        // Find the mod's base directory - prefer the hydromod.toml location,
        // otherwise use the version directory (parent of Content/, Scripts/)
        namespace fs = std::filesystem;
        std::string modDir;

        // First: look for the manifest file directly
        for (const auto& file : entry.files) {
            if (file.fileType == "manifest" && !file.filePath.empty()) {
                modDir = fs::path(file.filePath).parent_path().string();
                break;
            }
        }

        // Fallback: use any file's path, walking up past Content/Scripts subdirs
        if (modDir.empty()) {
            for (const auto& file : entry.files) {
                if (!file.filePath.empty()) {
                    fs::path dir = fs::path(file.filePath).parent_path();
                    // Walk up if we're inside Content/ or Scripts/ subdirectory
                    std::string dirName = dir.filename().string();
                    if (dirName == "Content" || dirName == "Scripts") {
                        dir = dir.parent_path();
                    }
                    modDir = dir.string();
                    break;
                }
            }
        }
        if (modDir.empty()) continue;

        // Look for hydromod.toml in the mod directory
        std::string tomlPath = modDir + "/hydromod.toml";
        if (!std::filesystem::exists(tomlPath)) {
            // Fallback: Blueprint-only mod without manifest - use convention path
            Hydro::logInfo("Mod '%s' - no hydromod.toml, using convention path", entry.modId.c_str());
            Hydro::ModManifest fallback;
            fallback.id = entry.modId;
            fallback.name = entry.modId;
            fallback.version = entry.versionNumber;
            fallback.tier = Hydro::ModTier::Tier1;
            fallback.actors.push_back(
                "/Game/Mods/" + entry.modId + "/ModActor.ModActor_C");
            mods.push_back({std::move(fallback), modDir});
            continue;
        }

        Hydro::ModManifest modManifest;
        if (!modManifest.loadFromFile(tomlPath)) {
            Hydro::logError("Mod '%s' - failed to parse hydromod.toml, skipping", entry.modId.c_str());
            continue;
        }

        // Validate tier rules
        std::string err = modManifest.validate();
        if (!err.empty()) {
            Hydro::logError("Mod '%s' - invalid: %s", modManifest.id.c_str(), err.c_str());
            continue;
        }

        mods.push_back({std::move(modManifest), modDir});
    }

    // Sort by tier: Tier 3 first, then 2, then 1
    std::sort(mods.begin(), mods.end(), [](const LoadableMod& a, const LoadableMod& b) {
        return static_cast<int>(a.manifest.tier) > static_cast<int>(b.manifest.tier);
    });

    // Dependency resolution
    // Build a map of available mod IDs -> versions for quick lookup.
    // Then check each mod's dependencies, conflicts, and optional deps.
    std::map<std::string, std::string> availableMods;
    for (const auto& mod : mods) {
        availableMods[mod.manifest.id] = mod.manifest.version;
    }

    // Mark mods that fail dependency/conflict checks for skipping
    std::vector<bool> skipMod(mods.size(), false);

    for (size_t i = 0; i < mods.size(); i++) {
        const auto& m = mods[i].manifest;

        // Required dependencies - mod is skipped if any are missing
        for (const auto& [depId, depRange] : m.dependencies) {
            if (availableMods.find(depId) == availableMods.end()) {
                Hydro::logError("Mod '%s' requires '%s' (%s) which is not installed or enabled - skipping",
                    m.id.c_str(), depId.c_str(), depRange.c_str());
                skipMod[i] = true;
                break;
            }
            // TODO: Full semver range matching (for now, presence check only)
        }
        if (skipMod[i]) continue;

        // Conflicts - mod is skipped if a conflicting mod is present
        for (const auto& [conflictId, conflictRange] : m.conflicts) {
            if (availableMods.find(conflictId) != availableMods.end()) {
                Hydro::logError("Mod '%s' conflicts with '%s' (%s) which is installed - skipping",
                    m.id.c_str(), conflictId.c_str(), conflictRange.c_str());
                skipMod[i] = true;
                break;
            }
        }
        if (skipMod[i]) continue;

        // Optional dependencies - warn if missing, don't skip
        for (const auto& [optDepId, optDepRange] : m.optionalDependencies) {
            if (availableMods.find(optDepId) == availableMods.end()) {
                Hydro::logWarn("Mod '%s' has optional dependency '%s' (%s) which is not installed - some features may be unavailable",
                    m.id.c_str(), optDepId.c_str(), optDepRange.c_str());
            }
        }
    }

    // Publish each mod's apiProvides to the registry manager so
    // Hydro.Registry.create can validate ownership at script time.
    for (size_t i = 0; i < mods.size(); i++) {
        if (skipMod[i]) continue;
        Hydro::RegistryManager::instance().setModProvides(
            mods[i].manifest.id, mods[i].manifest.apiProvides);
    }

    int loaded = 0;
    int skipped = 0;

    for (size_t i = 0; i < mods.size(); i++) {
        if (skipMod[i]) {
            skipped++;
            continue;
        }
        const auto& mod = mods[i];
        const auto& m = mod.manifest;
        Hydro::logInfo("Loading mod '%s' v%s (tier %d)", m.id.c_str(), m.version.c_str(), (int)m.tier);

        // Auto-spawn actors for Blueprint-only mods (no scripts).
        // Mods with scripts handle their own spawning via the Hydro.Assets API.
        if (m.hasActors() && !m.hasScripts()) {
            for (const auto& actorPath : m.actors) {
                std::wstring widePath(actorPath.begin(), actorPath.end());

                void* actorClass = Hydro::Engine::loadAsset(widePath.c_str());
                if (!actorClass)
                    actorClass = Hydro::Engine::findObject(widePath.c_str());

                if (!actorClass) {
                    Hydro::logWarn("  Actor '%s' not found", actorPath.c_str());
                    continue;
                }

                void* actor = Hydro::Engine::spawnActor(actorClass, 0.0, 0.0, 0.0);
                if (actor)
                    Hydro::logInfo("  Spawned '%s' at %p", actorPath.c_str(), actor);
                else
                    Hydro::logError("  Failed to spawn '%s'", actorPath.c_str());
            }
        }

        // Run Lua scripts (Tier 1 & 2) - scripts own all game interaction
        if (m.hasScripts()) {
            if (!s_lua.isReady()) {
                if (!s_lua.initialize()) {
                    Hydro::logError("  LuaJIT initialization failed");
                } else {
                    // Register built-in Tier 2 API modules
                    s_lua.registerModule("Hydro.Assets", Hydro::API::registerAssetsModule);
                    s_lua.registerModule("Hydro.World", Hydro::API::registerWorldModule);
                    s_lua.registerModule("Hydro.Events", Hydro::API::registerEventsModule);
                    s_lua.registerModule("Hydro.Registry", Hydro::API::registerRegistryModule);
                    s_lua.registerModule("Hydro.Reflect", Hydro::API::registerReflectModule);
                    s_lua.registerModule("Hydro.Net", Hydro::API::registerNetModule);
                }
            }
            if (s_lua.isReady()) {
                // Script path: try Scripts/ subdirectory first (package structure),
                // then root (flat structure)
                std::string scriptPath = mod.baseDir + "/Scripts/" + m.scriptEntry;
                if (!std::filesystem::exists(scriptPath)) {
                    scriptPath = mod.baseDir + "/scripts/" + m.scriptEntry;
                }
                if (!std::filesystem::exists(scriptPath)) {
                    scriptPath = mod.baseDir + "/" + m.scriptEntry;
                }
                s_lua.executeModScript(m.id, scriptPath, m.tier);
            }
        }

        // Tier 3: Native DLL loading
        // Loads a native DLL specified by [native] entry in hydromod.toml.
        // The DLL gets full EngineAPI access. If the DLL exports a
        // `hydro_init` function (void hydro_init(void)), it is called
        // after loading. If not exported, the DLL is expected to
        // self-initialize via DllMain.
        if (!m.nativeEntry.empty()) {
            std::string dllPath = mod.baseDir + "/" + m.nativeEntry;

            if (!std::filesystem::exists(dllPath)) {
                Hydro::logError("  Native DLL not found: %s", dllPath.c_str());
            } else {
                HMODULE hMod = LoadLibraryA(dllPath.c_str());
                if (!hMod) {
                    DWORD err = GetLastError();
                    Hydro::logError("  Failed to load native DLL '%s' (error %lu)",
                                    dllPath.c_str(), err);
                } else {
                    Hydro::logInfo("  Loaded native DLL: %s", dllPath.c_str());

                    using HydroInitFn = void(*)();
                    auto initFn = reinterpret_cast<HydroInitFn>(
                        GetProcAddress(hMod, "hydro_init"));
                    if (initFn) {
                        Hydro::logInfo("  Calling hydro_init()");
                        initFn();
                    }
                }
            }
        }

        loaded++;
    }

    Hydro::logInfo("Mod loading complete: %d/%zu loaded, %d skipped (dependency/conflict)", loaded, mods.size(), skipped);

    // Registry commit phase: fire on_commit for every registry now that all
    // Tier 1 consumers have finished contributing entries.
    if (s_lua.isReady()) {
        Hydro::RegistryManager::instance().commitAll(s_lua.state());
        Hydro::RegistryManager::instance().dumpAll();
    }

    s_modsLoaded = true;
}

// Mod class

class HydroCoreMod : public RC::CppUserModBase {
private:
    Hydro::Core m_core;

public:
    HydroCoreMod() : CppUserModBase() {
        ModName = STR("HydroCore");
        ModVersion = STR("0.1.0");
        ModDescription = STR("Hydro - Core Loader");
        ModAuthors = STR("Hydro Team");

        Hydro::setLogCallback(ue4ssLogCallback);
        Hydro::logInfo("DLL loaded - v0.1.0");

        s_core = &m_core;
        m_core.initialize();
    }

    ~HydroCoreMod() override {
        Hydro::logInfo("Shutting down");
        s_core = nullptr;
    }

    auto on_unreal_init() -> void override {
        Hydro::logInfo("Unreal Engine ready");

        // Pak mounting
        m_core.initPakLoader();
        m_core.executePakMounts();

        // Initialize EngineAPI eagerly - pattern scanning (or cache load)
        if (!s_engineReady) {
            if (Hydro::Engine::initialize()) {
                s_engineReady = true;
                Hydro::logInfo("EngineAPI initialized in on_unreal_init");
            }
        }

        // Poll each tick - load mods, then service background tasks
        RC::Unreal::Hook::RegisterEngineTickPostCallback([](RC::Unreal::UEngine*, float) {
                if (!s_modsLoaded) {
                LoadMods();
            }
            // Process reflection dump batches (no-op if no dump active)
            Hydro::API::tickDump();
            // Expire trace listeners (no-op if none active)
            Hydro::API::tickTraces();
            // Pump yielded coroutines. Without this, scripts that wait()
            // can stall on title/load screens where ProcessEvent is dormant.
            Hydro::API::tickEvents();
            // (Deferred AR discovery retry intentionally NOT called here -
            // discoverAssetRegistry walks GUObjectArray which freezes the
            // game thread for ~1s per call. Needs a faster scan or off-
            // thread approach before re-enabling.)
        });
    }

    auto on_update() -> void override {}
};

// UE4SS mod factory

extern "C" {
    __declspec(dllexport) RC::CppUserModBase* start_mod() {
        return new HydroCoreMod();
    }

    __declspec(dllexport) void uninstall_mod(RC::CppUserModBase* mod) {
        delete mod;
    }
}
