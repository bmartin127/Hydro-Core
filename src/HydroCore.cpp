#include "HydroCore.h"
#include <filesystem>
#include <cstdio>
#include <cstdarg>
#include <fstream>
#include <algorithm>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace Hydro {

// DLL path utility

// Get the directory containing this DLL. More reliable than
// current_path() which depends on how the host process was launched.
static fs::path getDllDirectory() {
#ifdef _WIN32
    HMODULE hModule = nullptr;
    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&getDllDirectory),
        &hModule
    );
    if (hModule) {
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(hModule, path, MAX_PATH);
        return fs::path(path).parent_path();
    }
#endif
    return fs::current_path();
}

// Logging

static LogCallback s_callback = nullptr;

// Fallback file logger - used when no callback is set (e.g. unit tests,
// standalone runs). Writes HydroCore.log next to the DLL.
static void fallbackLog(LogLevel level, const char* message) {
    static std::ofstream s_file;
    static bool s_opened = false;

    if (!s_opened) {
        s_opened = true;
        fs::path logPath = getDllDirectory() / "HydroCore.log";
        s_file.open(logPath, std::ios::out | std::ios::trunc);
    }

    const char* tag = "INFO";
    if (level == LogLevel::Warn)  tag = "WARN";
    if (level == LogLevel::Error) tag = "ERROR";

    if (s_file.is_open()) {
        s_file << "[HydroCore] [" << tag << "] " << message << "\n";
        s_file.flush();
    }
}

static void logImpl(LogLevel level, const char* fmt, va_list args) {
    char buf[2048];
    vsnprintf(buf, sizeof(buf), fmt, args);

    if (s_callback) {
        s_callback(level, buf);
    } else {
        fallbackLog(level, buf);
    }
}

void setLogCallback(LogCallback callback) {
    s_callback = callback;
}

void logInfo(const char* fmt, ...) {
    va_list args; va_start(args, fmt); logImpl(LogLevel::Info, fmt, args); va_end(args);
}

void logWarn(const char* fmt, ...) {
    va_list args; va_start(args, fmt); logImpl(LogLevel::Warn, fmt, args); va_end(args);
}

void logError(const char* fmt, ...) {
    va_list args; va_start(args, fmt); logImpl(LogLevel::Error, fmt, args); va_end(args);
}

// Core

Core::Core() {
    logInfo("Core created");
}

Core::~Core() {
    logInfo("Core destroyed");
}

std::string Core::findGameDirectory() const {
    // Walk up from the DLL's location looking for hydro_mods.json.
    // Typical UE5 packaged game layout:
    //
    //   GameRoot/
    //     GameName.exe
    //     hydro_mods.json                 (launcher writes this)
    //     GameName/Binaries/Win64/ue4ss/Mods/HydroCore/dlls/main.dll

    fs::path dir = getDllDirectory();

    for (int i = 0; i < 10; i++) {
        if (fs::exists(dir / "hydro_mods.json")) {
            return dir.string();
        }
        fs::path parent = dir.parent_path();
        if (parent == dir) break; // filesystem root
        dir = parent;
    }

    return "";
}

void Core::initialize(const std::string& gameRootHint) {
    if (m_initialized) {
        logWarn("Already initialized, skipping");
        return;
    }

    logInfo("=== Initialization Start ===");

    try {
        // Determine game directory
        if (!gameRootHint.empty() && fs::is_directory(gameRootHint)) {
            m_gameDirectory = gameRootHint;
            logInfo("Game directory (from hint): %s", m_gameDirectory.c_str());
        } else {
            m_gameDirectory = findGameDirectory();
            if (!m_gameDirectory.empty()) {
                logInfo("Game directory (auto-detected): %s", m_gameDirectory.c_str());
            }
        }

        // Phase 1: Load the manifest
        if (!m_gameDirectory.empty()) {
            if (loadManifest()) {
                logInfo("Manifest loaded - profile: %s, mods: %zu",
                    m_manifest->getProfileName().c_str(),
                    m_manifest->getModCount());

                for (const auto& mod : m_manifest->getMods()) {
                    logInfo("  %s v%s (%zu files)",
                        mod.modId.c_str(),
                        mod.versionNumber.c_str(),
                        mod.files.size());

                    for (const auto& file : mod.files) {
                        logInfo("    %s [%s]", file.fileName.c_str(), file.fileType.c_str());
                    }
                }
                // Phase 4: Verify paks deployed by launcher
                verifyPaks();
            } else {
                logInfo("No manifest - running without launcher mods");
            }
        } else {
            logInfo("No game directory found - manual mod installation only");
            logInfo("DLL directory: %s", getDllDirectory().string().c_str());
        }

    } catch (const std::exception& e) {
        logError("Initialization failed: %s", e.what());
    } catch (...) {
        logError("Initialization failed: unknown error");
    }

    // Mark initialized even on failure - don't retry, the game should
    // still run. Future phases can check m_manifest != nullptr.
    m_initialized = true;
    logInfo("=== Initialization Complete ===");
}

bool Core::loadManifest() {
    fs::path manifestPath = fs::path(m_gameDirectory) / "hydro_mods.json";

    if (!fs::exists(manifestPath)) {
        logInfo("Manifest not found: %s", manifestPath.string().c_str());
        return false;
    }

    m_manifest = std::make_unique<Manifest>();
    return m_manifest->loadFromFile(manifestPath.string());
}

void Core::verifyPaks() {
    if (!m_manifest || m_manifest->getModCount() == 0) return;

    size_t expectedPaks = 0;
    for (const auto& mod : m_manifest->getMods()) {
        if (!mod.enabled) continue;
        for (const auto& file : mod.files) {
            if (file.fileType == "pak") expectedPaks++;
        }
    }

    if (expectedPaks == 0) return;

    PakLoader loader;
    size_t found = loader.verifyDeployedPaks(m_gameDirectory);
    logInfo("Pak verification: %zu/%zu Hydro paks found in HydroMods/", found, expectedPaks);
    if (found < expectedPaks) {
        logWarn("Some paks missing - engine may not mount all mods");
    }
}

void Core::initPakLoader() {
    if (!m_manifest || m_manifest->getModCount() == 0) return;
    bool hasPaks = false;
    for (const auto& mod : m_manifest->getMods()) {
        if (!mod.enabled) continue;
        for (const auto& file : mod.files)
            if (file.fileType == "pak") { hasPaks = true; break; }
        if (hasPaks) break;
    }
    if (!hasPaks) return;

    logInfo("Creating PakLoader...");
    m_pakLoader = std::make_unique<PakLoader>();
    logInfo("Calling PakLoader::initialize()...");
    if (!m_pakLoader->initialize()) {
        logError("PakLoader init failed");
    }
    logInfo("PakLoader init done");
}

void Core::executePakMounts() {
    if (!m_pakLoader || !m_pakLoader->isReady()) return;
    if (!m_manifest) return;

    std::vector<std::tuple<std::string, std::string, int>> paks;
    int priority = 100;
    for (const auto& mod : m_manifest->getMods()) {
        if (!mod.enabled) continue;
        for (const auto& file : mod.files)
            if (file.fileType == "pak")
                paks.push_back({file.filePath, mod.modId, priority});
        priority += 10;
    }
    if (paks.empty()) return;

    auto results = m_pakLoader->mountAll(paks);
    for (const auto& r : results) {
        if (r.success) logInfo("Mounted: %s (%s)", r.pakPath.c_str(), r.modId.c_str());
        else logError("Mount failed: %s - %s", r.modId.c_str(), r.error.c_str());
    }
    logInfo("Mounting: %zu/%zu succeeded", m_pakLoader->getMountedCount(), paks.size());
}

} // namespace Hydro
