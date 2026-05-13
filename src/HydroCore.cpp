#include "HydroCore.h"
#include "EngineAPI.h"
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

// Current manifest pointer - set by Core::initialize after a successful
// load. Read by API modules (HydroNet, etc.) without needing a Core ref.
static const Manifest* s_currentManifest = nullptr;

const Manifest* getCurrentManifest() { return s_currentManifest; }

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
    // Clean any symlinks we created so the game runs vanilla once HydroCore
    // isn't loaded (e.g. user launches via Steam directly with the proxy
    // disabled). Best-effort - if files are locked we just log and move on.
    cleanupDeployedPaks();
    logInfo("Core destroyed");
}

std::string Core::findGameDirectory() const {
    // Walk up from the DLL's location looking for hydro_mods.json.
    //
    // Typical UE5 packaged game layout:
    //   GameRoot/
    //   +-- GameName.exe
    //   +-- hydro_mods.json           <-- launcher writes this here
    //   +-- GameName/
    //       +-- Binaries/
    //           +-- Win64/
    //               +-- ue4ss/
    //                   +-- Mods/
    //                       +-- HydroCore/
    //                           +-- dlls/
    //                               +-- main.dll  <-- this DLL

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
                // Symlink the manifest's mod paks into Content/Paks/LogicMods/
                // BEFORE the engine's pak auto-mount runs at startup. We get
                // here from start_mod()/DLL load, which UE4SS calls early
                // enough for our symlinks to be visible to UE's first scan.
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

// FNV-1a 32-bit hash of mod_id mapped into [10000, 99999]. MUST stay in sync
// with `hydro-cli/src/build.rs::stable_chunk_index_for_mod` and the inline
// computation in `Core::deployPaks` below - change one, change all three.
static uint32_t stableChunkIndexForMod(const std::string& modId) {
    uint32_t h = 0x811C9DC5u;
    for (unsigned char c : modId) {
        h ^= c;
        h *= 0x01000193u;
    }
    return 10000u + (h % 90000u);
}

// Locate <Project>/Content/Paks/LogicMods/ relative to the game directory.
// Same walk-up-five-parents shape as findHydroModsDir below, but read-only -
// returns "" if not present rather than creating it. Used by AR.bin lookup
// at runtime (we never want to mkdir from the engine-tick callback).
static fs::path findExistingLogicModsDir(const std::string& gameDir) {
    fs::path dir(gameDir);
    for (int i = 0; i < 5; i++) {
        fs::path target = dir / "Content" / "Paks" / "LogicMods";
        std::error_code ec;
        if (fs::is_directory(target, ec)) return target;
        fs::path parent = dir.parent_path();
        if (parent == dir) break;
        dir = parent;
    }
    return {};
}

void Core::executePakMounts() {
    // UE 5.6 AR-merge bridge - successor to the runtime FPakPlatformFile::Mount
    // experiments that filled this slot through April-May 2026. Empirical
    // baseline: same HydroCore.dll runs both DMG@5.5 (sphere mod loads) and
    // DMG@5.6 (Assets.load returns nil). UE 5.5's engine-startup pak sweep
    // auto-merged the LogicMods/ pakchunk*.AssetRegistry.bin sidecars into
    // the live IAssetRegistry; UE 5.6 stopped doing that - packages mount
    // at the file level but stay invisible to AR / StaticLoadObject.
    //
    // Fix: per enabled mod, locate its deployed AR.bin and merge it via
    // UAssetRegistryImpl::Serialize(FArchive&) - the same entry point Epic's
    // own GameFeaturePlugin pipeline uses (paired with MountExplicitlyLoadedPlugin
    // for the file-mount side, which engine startup already handled for us).
    // The function pointer is resolved on first call by anchor-driven discovery
    // (EngineAPI::findArSerializeFn - two __FUNCTION__ anchors in the inner
    // callee chain → LEA xref → .pdata → E8-caller-climb to the outer virtual
    // → single SEH-guarded verify call) and persisted in ScanCache so warm
    // starts skip the scan entirely.
    //
    // Idempotency: on hosts where the engine already merged AR (5.5, Palworld
    // 5.1), Phase 4 will gate this behind a known_asset_path pre-check from
    // the manifest. Until that lands, we always run the merge - UE's AR
    // dedupes by ObjectPath internally so a double-merge is wasteful but not
    // corrupting. Phase 4 turns this from wasteful into elegant.
    //
    // Per-launch one-shot: once we've successfully run (or determined we have
    // nothing to do), m_pakMountsExecuted is set and subsequent calls no-op.
    // This is what lets us call from on_unreal_init (preferred - hidden in
    // loading screen) AND from the per-tick callback (fallback if AR singleton
    // wasn't bound yet at on_unreal_init time).
    if (m_pakMountsExecuted) return;
    if (!m_manifest || m_manifest->getModCount() == 0) {
        m_pakMountsExecuted = true;
        return;
    }
    if (!Engine::isReady()) {
        // AR singleton not yet bound - try again next tick.
        return;
    }

    // Optional: defer N ticks past engine-ready before invoking the bridge.
    // Hypothesis: AR has placeholder/uninitialized State entries with
    // invalid FNames at the moment engine is "ready"; the inner serialize
    // path crashes when it tries to decode those FNames. Setting
    // HYDRO_AR_SERIALIZE_DELAY_TICKS=N waits N ticks before running. Use
    // a generous value (e.g. 60-300) on first probe; if T1 starts passing
    // after a delay, the timing theory is confirmed.
    {
        char delayBuf[16] = {};
        DWORD delayLen = GetEnvironmentVariableA("HYDRO_AR_SERIALIZE_DELAY_TICKS",
                                                 delayBuf, sizeof(delayBuf));
        int delayTicks = 0;
        if (delayLen > 0 && delayLen < sizeof(delayBuf)) {
            delayTicks = atoi(delayBuf);
        }
        if (delayTicks > 0) {
            m_pakMountsTickCounter++;
            if (m_pakMountsTickCounter < delayTicks) {
                if (m_pakMountsTickCounter == 1 ||
                    m_pakMountsTickCounter % 30 == 0) {
                    logInfo("AR.Serialize bridge: deferring (tick %d / %d)",
                            m_pakMountsTickCounter, delayTicks);
                }
                return;  // not yet - try again next tick
            }
            logInfo("AR.Serialize bridge: delay reached (tick %d) - "
                    "running merge now", m_pakMountsTickCounter);
        }
    }
    fs::path logicMods = findExistingLogicModsDir(m_gameDirectory);
    if (logicMods.empty()) {
        logWarn("AR.Serialize bridge: no LogicMods/ dir under %s - skipping",
                m_gameDirectory.c_str());
        m_pakMountsExecuted = true;
        return;
    }

    int merged = 0;
    int missing = 0;
    int failed = 0;
    for (const auto& mod : m_manifest->getMods()) {
        if (!mod.enabled) continue;
        bool hasPak = false;
        for (const auto& f : mod.files) if (f.fileType == "pak") { hasPak = true; break; }
        if (!hasPak) continue;

        uint32_t chunkIndex = stableChunkIndexForMod(mod.modId);
        std::string arName =
            "pakchunk" + std::to_string(chunkIndex) + "-Windows.AssetRegistry.bin";
        fs::path arPath = logicMods / arName;
        std::error_code ec;
        if (!fs::is_regular_file(arPath, ec)) {
            // Cook may not have produced an AR.bin sidecar (some loose-file
            // mods don't ship one). Soft-skip.
            missing++;
            continue;
        }

        if (Engine::loadAssetRegistryBin(arPath.wstring().c_str())) {
            logInfo("AR.Serialize: merged %s for mod %s",
                    arName.c_str(), mod.modId.c_str());
            merged++;
        } else {
            logWarn("AR.Serialize: FAILED for mod %s (%ls)",
                    mod.modId.c_str(), arPath.wstring().c_str());
            failed++;
        }
    }
    logInfo("AR.Serialize bridge done - merged=%d missing=%d failed=%d",
            merged, missing, failed);
    m_pakMountsExecuted = true;
}

void Core::wireArSearchAllAssets() {
    if (m_arSearchAllAssetsDone) return;
    if (!Engine::isReady()) return;

    // SearchAllAssets requires the AR impl + GetAssetByObjectPath UFunction
    // (used as the anchor for the AR interface UClass). Both come from
    // discoverAssetRegistry, which can fail at boot if AR CDOs are not yet
    // in GUObjectArray. We don't gate on those directly here - searchAllAssets
    // logs a warn and returns false; we re-try every tick until it lands.
    if (!Engine::searchAllAssets(/*bSynchronousSearch=*/true)) {
        // Will retry next tick - soft fail.
        return;
    }
    m_arSearchAllAssetsDone = true;
    logInfo("AR.SearchAllAssets fired - mod content should now be indexed");
}

// HydroPaks watcher - polls <gameDir>/HydroPaks/ for new pak files and
// mounts them via Engine::mountPakAtRuntime. Off by default; enabled via
// env var HYDRO_PAKS_WATCH=1. Polls every ~60 ticks (~1 sec at 60 fps).
//
// Workflow: a modder (or external tool, or a future launcher feature)
// drops a `pakchunkN-Windows.pak` triple into HydroPaks/. On the next
// poll cycle, HydroCore detects the new file, locates its sibling
// .utoc/.ucas/.AssetRegistry.bin, calls FPakPlatformFile::Mount through
// the runtime-mount API, and merges the AR sidecar.
//
// Already-mounted files are tracked in `m_dropWatchSeen` to avoid double-
// mount across polls. Files removed and re-added with the same name are
// re-mounted (set is keyed by filename, not by mtime).
void Core::pollHydroPaks() {
    namespace fs = std::filesystem;

    // First call: read env var to decide whether to enable.
    if (!m_dropWatchInitDone) {
        m_dropWatchInitDone = true;
        char buf[8] = {};
        DWORD len = GetEnvironmentVariableA("HYDRO_PAKS_WATCH", buf, sizeof(buf));
        if (len > 0 && buf[0] == '1') {
            m_dropWatchEnabled = true;
            logInfo("HydroPaks watcher: ENABLED (HYDRO_PAKS_WATCH=1)");
        }
    }
    if (!m_dropWatchEnabled) return;

    // Throttle: poll every ~10 ticks (~6 polls/sec at 60 fps). Each poll
    // is one folder-mtime stat (fast path) and a tiny set-lookup; cheap.
    if (++m_dropWatchTickCounter < 10) return;
    m_dropWatchTickCounter = 0;

    // Locate the drop folder. Walk up from m_gameDirectory until we find
    // a "HydroPaks/" sibling/ancestor.
    if (m_gameDirectory.empty()) {
        static bool s_warnedNoGameDir = false;
        if (!s_warnedNoGameDir) {
            s_warnedNoGameDir = true;
            logWarn("HydroPaks watcher: m_gameDirectory empty - no walkup possible");
        }
        return;
    }
    fs::path dir(m_gameDirectory);
    fs::path dropDir;
    for (int i = 0; i < 6; i++) {
        fs::path candidate = dir / "HydroPaks";
        if (fs::is_directory(candidate)) { dropDir = candidate; break; }
        fs::path parent = dir.parent_path();
        if (parent == dir) break;
        dir = parent;
    }
    static bool s_loggedDropPath = false;
    if (!s_loggedDropPath) {
        s_loggedDropPath = true;
        if (dropDir.empty()) {
            logWarn("HydroPaks watcher: no 'HydroPaks' folder found in 6-level walk-up "
                    "from m_gameDirectory='%s'", m_gameDirectory.c_str());
        } else {
            logInfo("HydroPaks watcher: watching %s", dropDir.string().c_str());
        }
    }
    if (dropDir.empty()) return;

    // Heartbeat once per ~minute so we can tell the watcher is alive.
    static int s_heartbeatCount = 0;
    if (++s_heartbeatCount % 360 == 0) {
        logInfo("HydroPaks watcher: alive (poll #%d, mounted=%zu)",
                s_heartbeatCount, m_dropWatchSeen.size());
    }

    // Fast path: stat the folder's last_write_time. If it hasn't changed
    // since our previous poll, skip the directory_iterator (Windows
    // updates folder mtime when files are added/removed/renamed inside;
    // this is one cheap stat call vs walking every file).
    std::error_code mec;
    auto ft = fs::last_write_time(dropDir, mec);
    if (!mec) {
        int64_t mt = ft.time_since_epoch().count();
        if (mt == m_dropWatchLastMtime) return;
        m_dropWatchLastMtime = mt;
    }
    // (If stat failed, fall through to full scan - better than missing
    // the file altogether.)

    // Scan for *.pak; skip ones we've already mounted this session.
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dropDir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        const fs::path& p = entry.path();
        if (p.extension() != ".pak") continue;

        std::string fname = p.filename().string();
        if (m_dropWatchSeen.count(fname)) continue;

        // Locate sibling .AssetRegistry.bin (optional).
        fs::path arBin = p; arBin.replace_extension("AssetRegistry.bin");
        bool hasAr = fs::is_regular_file(arBin);

        std::wstring wpak  = p.wstring();
        std::wstring war   = hasAr ? arBin.wstring() : std::wstring();

        logInfo("HydroPaks watcher: detected '%s' (AR.bin=%s) - mounting",
                fname.c_str(), hasAr ? "yes" : "no");
        bool ok = Engine::mountPakAtRuntime(
            wpak.c_str(),
            war.empty() ? nullptr : war.c_str(),
            /*priority=*/1000);
        logInfo("HydroPaks watcher: mount '%s' → %s", fname.c_str(),
                ok ? "OK" : "FAIL");
        // Only mark as seen on success. If mount fails (typically because
        // FPakPlatformFile reverse-discovery hasn't completed yet), retry
        // on the next poll cycle.
        if (ok) m_dropWatchSeen.insert(fname);
    }
}

// Pak deployment - symlink mods into Content/Paks/LogicMods/

namespace {

// Returns the game's `<Project>/Content/Paks/LogicMods/` path, creating it
// if missing. Empty string on failure.
//
// `gameDir` is the location of `hydro_mods.json` - typically
// `<game>/<Project>/Binaries/Win64/`, NOT the project root. So we walk
// UP looking for `Content/Paks/` (same pattern as PakLoader uses).
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

// Copy a pak file into the deploy target. File symlinks on Windows need
// SeCreateSymbolicLinkPrivilege, which regular users don't have, so we
// just copy. Pak triples are <= ~16 MB and source/dest share a volume.
// Returns "copy" on success, "" on failure.
const char* linkOrCopy(const fs::path& src, const fs::path& dst) {
    std::error_code cec;
    fs::copy_file(src, dst, fs::copy_options::overwrite_existing, cec);
    return cec ? "" : "copy";
}

}  // namespace

bool Core::cleanupDeployedPaks() {
    if (m_gameDirectory.empty()) return false;
    fs::path logicMods = findHydroModsDir(m_gameDirectory);
    if (logicMods.empty()) return false;

    // Walk both LogicMods/ (current target) and HydroMods/ (legacy target
    // from the pre-chunked-shader-library era). UE auto-mounts any pak
    // under Content/Paks/** regardless of subdir, so a stale Hydro_* triple
    // in HydroMods/ collides with a fresh pakchunk<N>-Windows.* in
    // LogicMods/ at AssetRegistry merge time and trips a TArray-bounds
    // fatal in ContainerHelpers.cpp.
    fs::path paksRoot = logicMods.parent_path();
    fs::path legacyDirs[] = { logicMods, paksRoot / "HydroMods" };

    int removed = 0;
    for (const auto& dir : legacyDirs) {
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) continue;
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            if (!entry.is_regular_file() && !entry.is_symlink()) continue;
            std::string name = entry.path().filename().string();
            // Match three shapes:
            //   1. legacy `Hydro_*` (pre-chunked-shader-library era)
            //   2. transient `pakchunk<N>-Hydro_*` (one-day intermediate)
            //   3. current `pakchunk<N>-Windows.<ext>` where N in [10000,99999]
            //      - our reserved chunk-index range from the FNV-1a mod_id hash.
            //      Host-shipped paks use small N (typically 0..few-hundred);
            //      our high range avoids collision with host content.
            bool isOurs = (name.rfind("Hydro_", 0) == 0) ||
                          (name.find("-Hydro_") != std::string::npos);
            if (!isOurs && name.rfind("pakchunk", 0) == 0) {
                // Parse digits after "pakchunk" up to the next '-' or '.'
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
    }
    if (removed > 0) logInfo("Cleaned %d Hydro_* file(s) from LogicMods/+HydroMods/", removed);
    return true;
}

bool Core::deployPaks() {
    if (!m_manifest || m_manifest->getModCount() == 0) return false;
    if (m_gameDirectory.empty()) {
        logWarn("Pak deploy skipped - no game directory");
        return false;
    }
    // Manual-test gate: HYDRO_SKIP_DEPLOY=1 disables LogicMods/ deploy so a
    // hand-staged plugin tree under <game>/Plugins/<Mod>/ can be tested in
    // isolation. Also runs cleanupDeployedPaks() so any prior deploy is
    // removed before launch.
    char skipBuf[8] = {};
    DWORD skipLen = GetEnvironmentVariableA("HYDRO_SKIP_DEPLOY", skipBuf, sizeof(skipBuf));
    if (skipLen > 0 && skipLen < sizeof(skipBuf) && skipBuf[0] == '1') {
        logWarn("Pak deploy skipped (HYDRO_SKIP_DEPLOY=1) - running cleanup only");
        cleanupDeployedPaks();
        return false;
    }

    fs::path target = findHydroModsDir(m_gameDirectory);
    if (target.empty()) {
        logWarn("Pak deploy skipped - couldn't locate Content/Paks/LogicMods/ (or HydroMods/)");
        return false;
    }

    // Always start clean - orphan Hydro_* from a previous session would
    // collide with this run's symlinks and confuse load priority.
    cleanupDeployedPaks();

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

            // Deploy the .pak plus its IoStore companions (.utoc, .ucas)
            // plus the cook's .AssetRegistry.bin sibling - UE 5.6's
            // AssetRegistry merge needs this on startup-mount to know what
            // packages live in the chunk. Without it the pak mounts but
            // GetAsset queries return null because AR has no entries for
            // /Game/Mods/<modName>/*.
            //
            // The `_P` suffix is UE's "patch" marker - bumps load priority
            // so our content overrides anything the game's own pak baked.
            for (const char* ext : {"pak", "utoc", "ucas", "AssetRegistry.bin"}) {
                fs::path sibling = pakSrc;
                sibling.replace_extension(ext);
                if (!fs::exists(sibling)) continue;

                // Filename MUST start with "pakchunk<N>" so UE's PakGetPakchunkIndex
                // parses N as the pak's chunk index. UE then uses that index in
                // GetShaderLibraryNameForChunk("<HostProject>", N) → "<HostProject>_Chunk<N>",
                // which matches the in-pak shader archive name written by hydro-cli.
                uint32_t chunkIndex = stableChunkIndexForMod(mod.modId);

                std::string destName =
                    "pakchunk" + std::to_string(chunkIndex) + "-Windows." + std::string(ext);
                fs::path dest = target / destName;

                // Best-effort overwrite (cleanup above should have wiped these,
                // but a stray symlink can survive a crash).
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
