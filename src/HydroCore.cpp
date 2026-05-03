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

// Get the directory containing this DLL.
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

static const Manifest* s_currentManifest = nullptr; // set by Core::initialize; read by API modules

const Manifest* getCurrentManifest() { return s_currentManifest; }

// Fallback file logger: writes HydroCore.log next to the DLL when no callback is set.
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
    cleanupDeployedPaks(); // remove our symlinks so the game runs vanilla if HydroCore is absent
    logInfo("Core destroyed");
}

std::string Core::findGameDirectory() const {
    // Walk up from the DLL until hydro_mods.json is found (up to 10 levels).
    fs::path dir = getDllDirectory();

    for (int i = 0; i < 10; i++) {
        if (fs::exists(dir / "hydro_mods.json")) {
            return dir.string();
        }
        fs::path parent = dir.parent_path();
        if (parent == dir) break;
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
    logInfo("HydroCore build fingerprint: %s %s", __DATE__, __TIME__);

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
                s_currentManifest = m_manifest.get();
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
                deployPaks();
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

    m_initialized = true; // mark even on failure - don't retry; game should still run
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
    logInfo("Pak verification: %zu/%zu Hydro paks found in LogicMods/", found, expectedPaks);
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
    // Paks are deployed to LogicMods/ at construction time so UE's startup scan picks them up.
    // Runtime mount is disabled by default (IoStore games seal FPackageStore before any runtime
    // mount fires). Set HYDRO_RUNTIME_MOUNT=1 to enable the runtime path for experiments.
    {
        char buf[16] = {};
        DWORD got = GetEnvironmentVariableA("HYDRO_RUNTIME_MOUNT", buf, sizeof(buf));
        if (!(got > 0 && buf[0] == '1')) {
            logInfo("Pak mount: relying on UE startup auto-mount (set HYDRO_RUNTIME_MOUNT=1 to opt into runtime mount)");
            return;
        }
        logInfo("HYDRO_RUNTIME_MOUNT=1 - runtime pak mount active");
    }

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

// Pak deployment - symlink mods into Content/Paks/LogicMods/

namespace {

// Returns Content/Paks/LogicMods/, creating it if missing. Empty on failure.
fs::path findHydroModsDir(const std::string& gameDir) {
    fs::path dir(gameDir);
    for (int i = 0; i < 5; i++) {
        fs::path content = dir / "Content" / "Paks";
        if (fs::is_directory(content)) {
            fs::path target = content / "LogicMods";
            std::error_code ec;
            fs::create_directories(target, ec);
            return target;
        }
        fs::path parent = dir.parent_path();
        if (parent == dir) break;
        dir = parent;
    }
    return {};
}

// Try to symlink src to dst; fall back to copy. Returns "symlink"/"copy"/"" on failure.
const char* linkOrCopy(const fs::path& src, const fs::path& dst) {
#ifdef _WIN32
    if (CreateSymbolicLinkW(dst.c_str(), src.c_str(), 0)) {
        return "symlink";
    }
#else
    std::error_code sec;
    fs::create_symlink(src, dst, sec);
    if (!sec) return "symlink";
#endif
    std::error_code cec;
    fs::copy_file(src, dst, fs::copy_options::overwrite_existing, cec);
    return cec ? "" : "copy";
}

}  // namespace

bool Core::cleanupDeployedPaks() {
    if (m_gameDirectory.empty()) return false;
    fs::path dir = findHydroModsDir(m_gameDirectory);
    if (dir.empty()) return false;

    int removed = 0;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file() && !entry.is_symlink()) continue;
        std::string name = entry.path().filename().string();
        // Match: legacy "Hydro_*", transient "pakchunk<N>-Hydro_*", and
        // current "pakchunk<N>-Windows.*" where N ∈ [10000,99999] (our FNV-1a range).
        bool isOurs = (name.rfind("Hydro_", 0) == 0) ||
                      (name.find("-Hydro_") != std::string::npos);
        if (!isOurs && name.rfind("pakchunk", 0) == 0) {
            size_t end = 8;
            while (end < name.size() && name[end] >= '0' && name[end] <= '9') end++;
            if (end > 8) {
                int n = atoi(name.substr(8, end - 8).c_str());
                if (n >= 10000 && n <= 99999) isOurs = true;
            }
        }
        if (!isOurs) continue;
        std::error_code rmec;
        fs::remove(entry.path(), rmec);
        if (!rmec) removed++;
    }
    if (removed > 0) logInfo("Cleaned %d Hydro_* file(s) from LogicMods/", removed);
    return true;
}

bool Core::deployPaks() {
    if (!m_manifest || m_manifest->getModCount() == 0) return false;
    if (m_gameDirectory.empty()) {
        logWarn("Pak deploy skipped - no game directory");
        return false;
    }

    fs::path target = findHydroModsDir(m_gameDirectory);
    if (target.empty()) {
        logWarn("Pak deploy skipped - couldn't locate Content/Paks/LogicMods/ (or HydroMods/)");
        return false;
    }

    cleanupDeployedPaks(); // remove orphans from previous session

    int linked = 0;
    for (const auto& mod : m_manifest->getMods()) {
        if (!mod.enabled) continue;
        for (const auto& file : mod.files) {
            if (file.fileType != "pak") continue;
            fs::path pakSrc(file.filePath);
            if (!fs::exists(pakSrc)) {
                logWarn("Pak source missing: %s", file.filePath.c_str());
                continue;
            }
            std::string baseStem = pakSrc.stem().string();

            // Deploy .pak plus IoStore companions (.utoc, .ucas).
            for (const char* ext : {"pak", "utoc", "ucas"}) {
                fs::path sibling = pakSrc;
                sibling.replace_extension(ext);
                if (!fs::exists(sibling)) continue;

                // Filename must start with "pakchunk<N>" so PakGetPakchunkIndex extracts N,
                // which UE uses in GetShaderLibraryNameForChunk to auto-merge our shader archive.
                // N = FNV-1a(mod.modId) mapped to [10000,99999] - MUST match hydro-cli build.rs.
                uint32_t chunkIndex = 0x811C9DC5u;
                for (unsigned char c : mod.modId) {
                    chunkIndex ^= c;
                    chunkIndex *= 0x01000193u;
                }
                chunkIndex = 10000u + (chunkIndex % 90000u);

                std::string destName =
                    "pakchunk" + std::to_string(chunkIndex) + "-Windows." + std::string(ext);
                fs::path dest = target / destName;

                std::error_code rmec;
                fs::remove(dest, rmec);

                const char* method = linkOrCopy(sibling, dest);
                if (method[0]) {
                    logInfo("Deployed %s (%s)", destName.c_str(), method);
                    if (std::string(ext) == "pak") linked++;
                } else {
                    logError("Failed to deploy %s", destName.c_str());
                }
            }
        }
    }

    logInfo("Pak deploy: %d pak(s) linked into LogicMods/", linked);
    return linked > 0;
}

} // namespace Hydro
