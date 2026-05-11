#pragma once

#include <string>
#include <memory>
#include <set>
#include "Manifest.h"
#include "PakLoader.h"

namespace Hydro {

// Log levels

enum class LogLevel { Info, Warn, Error };

// Log callback
// Signature: void callback(LogLevel level, const char* message)
// Set by the host (dllmain.cpp) to route logs through UE4SS.
// If not set, falls back to a log file next to the DLL.
using LogCallback = void(*)(LogLevel level, const char* message);

void setLogCallback(LogCallback callback);

void logInfo(const char* fmt, ...);
void logWarn(const char* fmt, ...);
void logError(const char* fmt, ...);

// Globally-accessible pointer to the current loaded manifest. Set by
// Core::initialize after a successful load; nullptr before that. Lets
// API modules (HydroNet, etc.) read the active mod set without taking
// a Core& reference through every registration path.
const class Manifest* getCurrentManifest();

// Core loader

class Core {
public:
    Core();
    ~Core();

    // Main initialization - called when Unreal Engine is ready.
    // gameRootHint: if provided, used as the game directory instead
    // of auto-detection. Pass empty string to auto-detect.
    void initialize(const std::string& gameRootHint = "");

    void verifyPaks();
    void initPakLoader();     // Heavy scan - call from on_unreal_init (any thread)
    void executePakMounts();  // Light mount call - call from on_update (game thread)

    // Trigger IAssetRegistry::SearchAllAssets(true) once per launch so
    // UE 5.6's bSearchAllAssets-gated on-mount gather actually fires for
    // our LogicMods/ paks. Idempotent; safe to call every engine tick.
    // Sets m_arSearchAllAssetsDone on first success.
    void wireArSearchAllAssets();

    // Symlink each enabled mod's pak triple (.pak/.utoc/.ucas) into the
    // game's `Content/Paks/HydroMods/` so UE auto-mounts them at engine
    // init. Naming: `Hydro_<modId>_<base>_P.<ext>` - the `_P` suffix bumps
    // pak load priority above the game's own pak. Called from initialize()
    // BEFORE engine pak scan runs (UE4SS injects HydroCore early enough).
    bool deployPaks();

    // Remove every `Hydro_*` file from `Content/Paks/HydroMods/`. Called
    // at shutdown so the game runs vanilla when HydroCore isn't loaded.
    bool cleanupDeployedPaks();

    // Access manifest for mod loading
    const Manifest* getManifest() const { return m_manifest.get(); }

private:
    std::string findGameDirectory() const;
    bool loadManifest();

    // Future phases:
    // Phase 2: Parse hydromod.toml for each mod
    // Phase 3: Build dependency graph and resolve load order
    // Phase 5: Initialize Lua runtime with sandbox
    // Phase 6: Execute mod init scripts

    std::string m_gameDirectory;
    std::unique_ptr<Manifest> m_manifest;
    std::unique_ptr<PakLoader> m_pakLoader;
    bool m_initialized = false;
    bool m_arSearchAllAssetsDone = false;  // SearchAllAssets fired once per launch (UE 5.6 mod-content gather)
    bool m_pakMountsExecuted = false;      // Runtime pak mount fired once per launch (UE 5.6 PackageStore registration)
    int  m_pakMountsTickCounter = 0;       // ticks since first executePakMounts attempt - gated by HYDRO_AR_SERIALIZE_DELAY_TICKS

public:
    // Poll <gameDir>/HydroPaks/ for new *.pak files and mount any that
    // weren't seen before. Called periodically from on_update (every N
    // ticks). Off by default; enable by setting env var HYDRO_PAKS_WATCH=1.
    void pollHydroPaks();

private:
    bool m_dropWatchEnabled = false;
    bool m_dropWatchInitDone = false;
    int  m_dropWatchTickCounter = 0;
    std::set<std::string> m_dropWatchSeen;  // pak files already mounted
    // Folder mtime cache for fast-path skip when nothing changed.
    int64_t m_dropWatchLastMtime = 0;
};

} // namespace Hydro
