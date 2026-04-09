#pragma once

#include <string>
#include <memory>
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
};

} // namespace Hydro
