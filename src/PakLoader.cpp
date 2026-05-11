#include "PakLoader.h"
#include "HydroCore.h"
#include "EngineAPI.h"
#include "RawFunctions.h"
#include "SEH.h"
#include <Unreal/FString.hpp>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif

namespace fs = std::filesystem;

namespace Hydro {

// FPakPlatformFile vtable indices.
//   UE 5.5: GetName at slot 14
//   UE 5.6: GetName at slot 10  (IPlatformFile lost ~4 virtuals between 5.5 and 5.6)
// FPakPlatformFile uses multiple inheritance, so the exe contains multiple
// vtables - the main one + interface-shim vtables. Shims have trivial stubs at
// slot[0] (e.g. `C2 00 00` = `ret 0`). We pick the candidate whose slot[0]
// points to REAL function code.
constexpr int kVTable_DeletingDtor = 0;
constexpr int kVTable_GetName_UE55 = 14;
constexpr int kVTable_GetName_UE56 = 10;
static const int kGetNameCandidates[] = { kVTable_GetName_UE56, kVTable_GetName_UE55 };

// True if `addr` looks like the start of a real function (typical x64
// prologue) rather than a thunk that just returns. Used to reject
// interface-shim vtables.
static bool looksLikeRealFunction(const uint8_t* p, const uint8_t* base, size_t size) {
    if (p < base || p + 16 > base + size) return false;
    uint8_t b0 = p[0];
    if (b0 == 0x48 || b0 == 0x4C || b0 == 0x40 || b0 == 0x41 || b0 == 0x44 ||
        b0 == 0x55 || b0 == 0x53 || b0 == 0x56 || b0 == 0x57) {
        if (b0 == 0xC2 || b0 == 0xC3) return false;
        return true;
    }
    return false;
}

// SEH wrappers

static bool safeReadBytes(void* addr, uint8_t* out, size_t count) {
    HYDRO_SEH_TRY(memcpy(out, addr, count));
}

// FPakMountArgs - must match UE 5.6's `IPlatformFilePak.h:1962-1974` exactly.
// The engine-side struct is:
//   struct FPakMountArgs {
//       const TCHAR* PakFilename = nullptr;       // +0
//       uint32 PakOrder = 0;                      // +8
//       const TCHAR* Path = nullptr;              // +16 (after 4B padding)
//       FPakMountOptions MountOptions;            // +24 (uint32 EMountFlags inside)
//       bool bLoadIndex = true;                   // +28
//   };
// sizeof(FPakMountArgs) = 32 (8-byte struct alignment from pointer members).
struct FPakMountArgsShim {
    const wchar_t* PakFilename;
    uint32_t       PakOrder;
    uint32_t       _pad0;          // explicit padding so Path lands at +16
    const wchar_t* Path;
    uint32_t       MountFlags;     // FPakMountOptions::EMountFlags (uint32 enum)
    uint8_t        bLoadIndex;
    uint8_t        _pad1[3];       // tail padding to 32 bytes
};
static_assert(sizeof(FPakMountArgsShim) == 32, "FPakMountArgsShim must match UE 5.6 FPakMountArgs (32 bytes)");
static_assert(offsetof(FPakMountArgsShim, PakFilename) == 0,  "PakFilename at +0");
static_assert(offsetof(FPakMountArgsShim, PakOrder)    == 8,  "PakOrder at +8");
static_assert(offsetof(FPakMountArgsShim, Path)        == 16, "Path at +16");
static_assert(offsetof(FPakMountArgsShim, MountFlags)  == 24, "MountFlags at +24");
static_assert(offsetof(FPakMountArgsShim, bLoadIndex)  == 28, "bLoadIndex at +28");

// EMountFlags values (Core/Public/GenericPlatform/GenericPlatformFile.h:1007-1013)
constexpr uint32_t EMountFlags_None                    = 0;
constexpr uint32_t EMountFlags_WithSoftReferences      = 1u << 0;
constexpr uint32_t EMountFlags_ReportDecryptionFailure = 1u << 1;
constexpr uint32_t EMountFlags_SkipContainerFile       = 1u << 2;

// SEH-isolated call into FPakPlatformFile::Mount(FPakMountArgs&, FIoStatus*, FPakListEntry*).
// The signature uses Microsoft x64 calling convention; first arg is `this`.
//   bool Mount(FPakMountArgs& MountArgs, FIoStatus* OutIoMountStatus = nullptr, FPakListEntry* OutPakListEntry = nullptr)
// Lives in its own function so MSVC C2712 (no __try with C++ unwinding objects)
// doesn't bite us in the caller - only POD pointers cross the boundary.
static bool sehCallPakMount(void* mountFn, void* pakSelf, FPakMountArgsShim* args, bool* outResult) {
    using MountFn = bool(__fastcall*)(void* this_, FPakMountArgsShim* args, void* outIoStatus, void* outListEntry);
#ifdef _WIN32
    __try {
        *outResult = ((MountFn)mountFn)(pakSelf, args, nullptr, nullptr);
        return true;
    } __except(1) { return false; }
#else
    *outResult = ((MountFn)mountFn)(pakSelf, args, nullptr, nullptr);
    return true;
#endif
}

// Game module helper

struct GameModule { uint8_t* base; size_t size; };

static GameModule findGameModule() {
#ifdef _WIN32
    HMODULE modules[1024]; DWORD needed;
    if (!K32EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed))
        return {nullptr, 0};
    HMODULE best = nullptr; size_t largest = 0;
    for (DWORD i = 0; i < needed / sizeof(HMODULE); i++) {
        MODULEINFO mi; char name[MAX_PATH];
        if (!K32GetModuleInformation(GetCurrentProcess(), modules[i], &mi, sizeof(mi))) continue;
        K32GetModuleBaseNameA(GetCurrentProcess(), modules[i], name, MAX_PATH);
        if (strstr(name, "UE4SS") || strstr(name, "HydroCore") || strstr(name, "patternsleuth")) continue;
        if (mi.SizeOfImage > largest) { largest = mi.SizeOfImage; best = modules[i]; }
    }
    if (!best) return {nullptr, 0};
    MODULEINFO gi; K32GetModuleInformation(GetCurrentProcess(), best, &gi, sizeof(gi));
    return {(uint8_t*)gi.lpBaseOfDll, gi.SizeOfImage};
#else
    return {nullptr, 0};
#endif
}

// -- FPakPlatformFile vtable discovery ------------------------------------
//
// Anchor: L"PakFile" (the GetName() return for FPakPlatformFile).
// 1. Find every LEA to that string in .text.
// 2. For each LEA, scan all module QWORDs for one that points back into
//    the LEA's containing function - that QWORD is GetName's slot in some
//    vtable.
// 3. Try treating that QWORD's index as either the UE 5.6 (slot 10) or
//    UE 5.5 (slot 14) GetName position. The vtable whose slot[0] looks
//    like a real function is the canonical FPakPlatformFile vtable
//    (others are interface-shim copies with `ret 0` stubs at slot[0]).
//
// Returns the vtable's base address (pointer to slot[0]), or nullptr.
static void** findPakVtable(GameModule gm) {
    uint8_t* base = gm.base;
    size_t size = gm.size;

    static const uint8_t pakFileStr[] = {'P',0,'a',0,'k',0,'F',0,'i',0,'l',0,'e',0,0,0};
    uint8_t* strAddr = nullptr;
    for (size_t i = 0; i + sizeof(pakFileStr) <= size; i++) {
        if (memcmp(base + i, pakFileStr, sizeof(pakFileStr)) == 0) { strAddr = base + i; break; }
    }
    if (!strAddr) {
        logError("PakLoader: L\"PakFile\" string not found in module");
        return nullptr;
    }

    for (size_t i = 0; i + 7 < size; i++) {
        if ((base[i] != 0x48 && base[i] != 0x4C) || base[i+1] != 0x8D || (base[i+2] & 0xC7) != 0x05) continue;
        int32_t disp = *(int32_t*)(base + i + 3);
        if (base + i + 7 + disp != strAddr) continue;

        // Found a LEA to L"PakFile". Some QWORD in the module points back
        // into this function - that's the GetName slot of some vtable.
        for (size_t vi = 0; vi + 8 <= size; vi += 8) {
            void* entry = *(void**)(base + vi);
            if (!entry || (uint8_t*)entry < base || (uint8_t*)entry >= base + size) continue;
            ptrdiff_t diff = (uint8_t*)entry - (base + i);
            if (diff < -4 || diff > 0) continue;

            for (int slotIdx : kGetNameCandidates) {
                void** vtable = (void**)(base + vi) - slotIdx;
                if ((uint8_t*)vtable < base || (uint8_t*)vtable + (uintptr_t)slotIdx*8 + 8 > base + size) continue;

                uint8_t* dtor = (uint8_t*)vtable[kVTable_DeletingDtor];
                if (dtor < base || dtor >= base + size) continue;
                if (!looksLikeRealFunction(dtor, base, size)) continue;

                logInfo("PakLoader: Picked FPakPlatformFile vtable at exe+0x%zX "
                        "(GetName=slot%d, slot[0]@exe+0x%zX)",
                        (size_t)((uint8_t*)vtable - base), slotIdx,
                        (size_t)(dtor - base));
                return vtable;
            }
        }
    }
    logError("PakLoader: FPakPlatformFile vtable not found");
    return nullptr;
}

// -- FPakPlatformFile singleton discovery ---------------------------------
//
// The engine allocates an FPakPlatformFile on the heap and stores the
// pointer inside FPlatformFileManager's `TopmostPlatformFile` member, which
// lives in the static singleton in module .data. So scanning module memory
// for a QWORD whose deref's first 8 bytes equal our discovered vtable
// SHOULD find it - except in practice the host's data layout / page
// protection / atomic wrappers can hide it from a naive linear scan.
//
// More robust: walk every committed memory region in the process via
// VirtualQuery, scan each readable page's QWORDs, deref each candidate,
// match against the vtable. This catches both module-stored and any
// indirection-stored references. Cost: ~1-3 sec on a typical UE process.
static void* findPakSingleton(GameModule gm, void** vtable) {
    if (!vtable) return nullptr;
    uint64_t vtableAddr = (uint64_t)vtable;

    int regionsScanned = 0;
    int regionsSeen = 0;
    int candidates = 0;
    void* firstHit = nullptr;
    uint64_t startTick = GetTickCount64();
    uint64_t lastProgressTick = startTick;
    constexpr uint64_t kMaxScanMs = 10000;  // hard cap - bail after 10 sec

#ifdef _WIN32
    SYSTEM_INFO sysInfo;
    GetSystemInfo(&sysInfo);
    uint8_t* addr = (uint8_t*)sysInfo.lpMinimumApplicationAddress;
    uint8_t* maxAddr = (uint8_t*)sysInfo.lpMaximumApplicationAddress;

    while (addr < maxAddr) {
        // Hard timeout - never let init hang.
        uint64_t now = GetTickCount64();
        if (now - startTick > kMaxScanMs) {
            logWarn("PakLoader: Singleton scan TIMED OUT after %llu ms "
                    "(regions seen=%d scanned=%d candidates=%d)",
                    (unsigned long long)(now - startTick),
                    regionsSeen, regionsScanned, candidates);
            break;
        }
        // Progress log every ~500ms so we can diagnose hangs.
        if (now - lastProgressTick > 500) {
            logInfo("PakLoader: Singleton scan progress @ %p "
                    "(seen=%d scanned=%d candidates=%d %llu ms in)",
                    addr, regionsSeen, regionsScanned, candidates,
                    (unsigned long long)(now - startTick));
            lastProgressTick = now;
        }

        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(addr, &mbi, sizeof(mbi)) == 0) break;
        regionsSeen++;
        if (mbi.State == MEM_COMMIT) {
            // Only scan readable pages. Skip guard / no-access pages.
            DWORD prot = mbi.Protect & 0xFF;
            bool readable =
                (prot == PAGE_READONLY) || (prot == PAGE_READWRITE) ||
                (prot == PAGE_WRITECOPY) ||
                (prot == PAGE_EXECUTE_READ) || (prot == PAGE_EXECUTE_READWRITE) ||
                (prot == PAGE_EXECUTE_WRITECOPY);
            // Restrict to writable pages only - FPakPlatformFile instance
            // pointer lives in writable memory (heap or .data), never in
            // executable .text or read-only constant pools. Cuts scan cost
            // by ~10x on a typical UE process.
            bool isWritable =
                (prot == PAGE_READWRITE) || (prot == PAGE_WRITECOPY) ||
                (prot == PAGE_EXECUTE_READWRITE) || (prot == PAGE_EXECUTE_WRITECOPY);
            // No region size cap - the instance pointer could live in a
            // large heap arena. The kMaxScanMs budget at the top of the
            // loop bounds total time instead.
            if (readable && isWritable && !(mbi.Protect & PAGE_GUARD)) {
                regionsScanned++;
                uint8_t* regionEnd = (uint8_t*)mbi.BaseAddress + mbi.RegionSize;
                // Inner-loop budget check every 64KB. Without this, a
                // single multi-GB region would burn the entire timeout
                // before the outer loop's check runs.
                uint64_t innerLastCheck = GetTickCount64();
                for (uint8_t* p = (uint8_t*)mbi.BaseAddress; p + 8 <= regionEnd; p += 8) {
                    if (((uintptr_t)p & 0xFFFF) == 0) {
                        uint64_t innerNow = GetTickCount64();
                        if (innerNow - startTick > kMaxScanMs) goto done;
                        innerLastCheck = innerNow;
                    }
                    // Read QWORD at this position. If equal to vtableAddr,
                    // this position is the start of an FPakPlatformFile
                    // instance - because in MSVC C++ ABI, an object's first
                    // 8 bytes are its vtable pointer, which equals the
                    // address of slot[0] of the vtable. That's exactly
                    // vtableAddr.
                    //
                    // SEH-wrap the read since VirtualQuery's protection
                    // snapshot can diverge from runtime page state.
                    uint64_t qword = 0;
                    if (!safeReadBytes(p, (uint8_t*)&qword, 8)) {
                        // Page faulted - skip rest of region.
                        break;
                    }
                    if (qword != vtableAddr) continue;

                    // Found a position whose first 8 bytes equal the
                    // vtable. p is an instance. Skip the vtable's own
                    // location (.rdata): a slot inside the vtable could
                    // theoretically hold its own address but in practice
                    // doesn't - vtable slots are function pointers, not
                    // self-references. Defensive skip just in case.
                    if (p == (uint8_t*)vtableAddr) continue;

                    candidates++;
                    if (!firstHit) firstHit = (void*)p;
                    if (candidates <= 4) {
                        logInfo("PakLoader: FPakPlatformFile candidate #%d "
                                "instance @ %p (region base=%p sz=0x%zX prot=0x%X)",
                                candidates, p,
                                mbi.BaseAddress, mbi.RegionSize, mbi.Protect);
                    }
                    if (candidates >= 8) goto done;
                }
            }
        }
        // Always advance: even if VirtualQuery returns RegionSize=0 in
        // some edge case, step at least one page forward to guarantee
        // termination. If the post-update addr <= old addr (wrap, or
        // VirtualQuery bug), bail out - better to under-scan than hang.
        uint8_t* prevAddr = addr;
        size_t advance = mbi.RegionSize ? mbi.RegionSize : 0x1000;
        addr = (uint8_t*)mbi.BaseAddress + advance;
        if (addr <= prevAddr) break;
    }
done:
    logInfo("PakLoader: Singleton scan: %d candidate(s) across %d region(s), vtableAddr=%p",
            candidates, regionsScanned, (void*)vtableAddr);
#else
    (void)gm;
#endif

    if (!firstHit) {
        logError("PakLoader: No FPakPlatformFile instance found anywhere in process");
        return nullptr;
    }
    logInfo("PakLoader: Using FPakPlatformFile @ %p", firstHit);
    return firstHit;
}

// -- FPakPlatformFile::Mount(FPakMountArgs&) discovery --------------------
//
// Anchor: L"Mounted IoStore container \"" - appears exactly once in shipping
// binaries inside FPakPlatformFile::Mount(FPakMountArgs&) at line 6053 of
// IPlatformFilePak.cpp. The .pdata exception directory gives us the
// containing function start regardless of any prologue shape (movaps SSE
// saves etc. that would defeat a byte-walk-back heuristic).
static void* findPakMountFn(GameModule gm) {
    uint8_t* base = gm.base;
    size_t size = gm.size;

    // Anchor candidates - strings that uniquely live INSIDE
    // `FPakPlatformFile::Mount(FPakMountArgs&)` (IPlatformFilePak.cpp:5928).
    //
    // Earlier "Mounting pak file" / "Mounted IoStore container" / "Failed to
    // mount pak" candidates all lead to the WRONG function:
    //   - "Mounting pak file" -> FHandleMountPaksExDelegate::HandleDelegate
    //     (line 6477) which has TArrayView<FMountPaksExArgs> args, not
    //     FPakMountArgs - calling it with our struct crashes.
    //   - "Mounted IoStore container" / "Failed to mount pak" - Display-level
    //     UE_LOGs stripped from PGO/LTO shipping builds (verified absent).
    //
    // "utoc not found" lives at line 6085 as the FIoStatus error literal:
    //   `IoStoreSuccess = FIoStatus(EIoErrorCode::NotFound, TEXT("utoc not found"));`
    // It is a wide TCHAR const in .rdata referenced via LEA from inside Mount.
    // Single occurrence in shipping binaries; unambiguously identifies Mount.
    //
    // "IoStore container" (line 6086) is the fallback - also single-occurrence,
    // also inside Mount, also a TEXT() literal that survives stripping.
    static const wchar_t* candidates[] = {
        L"utoc not found",         // FIoStatus error string inside Mount @ 6085
        L"IoStore container",      // UE_LOG warning inside Mount @ 6086
    };
    uint8_t* strAddr = nullptr;
    const wchar_t* matched = nullptr;
    for (const wchar_t* needle : candidates) {
        size_t needleBytes = wcslen(needle) * sizeof(wchar_t);
        for (size_t i = 0; i + needleBytes <= size; i++) {
            if (memcmp(base + i, needle, needleBytes) == 0) {
                strAddr = base + i;
                matched = needle;
                break;
            }
        }
        if (strAddr) break;
    }
    if (!strAddr) {
        logError("PakLoader: No FPakPlatformFile::Mount anchor string found "
                 "(tried 'utoc not found', 'IoStore container')");
        return nullptr;
    }
    logInfo("PakLoader: anchor '%ls' at exe+0x%zX",
            matched, (size_t)(strAddr - base));

    // Find the LEA to that string in .text.
    uint8_t* leaAddr = nullptr;
    for (size_t i = 0; i + 7 < size; i++) {
        if ((base[i] != 0x48 && base[i] != 0x4C) || base[i+1] != 0x8D ||
            (base[i+2] & 0xC7) != 0x05) continue;
        int32_t disp = *(int32_t*)(base + i + 3);
        if (base + i + 7 + disp == strAddr) { leaAddr = base + i; break; }
    }
    if (!leaAddr) {
        logError("PakLoader: No LEA ref to anchor string '%ls'", matched);
        return nullptr;
    }
    logInfo("PakLoader: LEA at exe+0x%zX", (size_t)(leaAddr - base));

    // Resolve the containing function via PE exception directory (.pdata).
    auto* dosHeader = (IMAGE_DOS_HEADER*)base;
    auto* ntHeaders = (IMAGE_NT_HEADERS*)(base + dosHeader->e_lfanew);
    auto& excDir = ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (excDir.VirtualAddress == 0 || excDir.Size == 0) {
        logError("PakLoader: PE has no exception directory");
        return nullptr;
    }
    auto* funcs = (RUNTIME_FUNCTION*)(base + excDir.VirtualAddress);
    size_t numFuncs = excDir.Size / sizeof(RUNTIME_FUNCTION);
    uint32_t leaRva = (uint32_t)(leaAddr - base);

    size_t lo = 0, hi = numFuncs;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (leaRva < funcs[mid].BeginAddress) hi = mid;
        else if (leaRva >= funcs[mid].EndAddress) lo = mid + 1;
        else {
            uint32_t beginRva = funcs[mid].BeginAddress;
            uint32_t endRva = funcs[mid].EndAddress;
            void* fnStart = base + beginRva;
            logInfo("PakLoader: FPakPlatformFile::Mount at exe+0x%X "
                    "(.pdata fn 0x%X-0x%X, %u bytes)",
                    beginRva, beginRva, endRva, endRva - beginRva);
            return fnStart;
        }
    }

    logError("PakLoader: LEA at exe+0x%X has no containing .pdata function", leaRva);
    return nullptr;
}

// -- PakLoader public API --------------------------------------------------

bool PakLoader::initializeWithInstance(void* fpakPlatformFile) {
    if (!fpakPlatformFile) {
        logError("PakLoader: initializeWithInstance called with null pointer");
        return false;
    }
    GameModule gm = findGameModule();
    if (!gm.base) { logError("PakLoader: Game module not found"); return false; }

    m_pakPlatformFile = fpakPlatformFile;

    // Read the instance's first qword as its actual vtable. The ANCHOR-
    // based findPakVtable picked exe+0x6DB8918 (wrong) on UE 5.6 DMG; the
    // real vtable on this build is at exe+0x6CF9010 - recoverable only via
    // the instance pointer itself.
    void* vt = nullptr;
    if (!safeReadBytes(m_pakPlatformFile, (uint8_t*)&vt, sizeof(vt)) || !vt) {
        logError("PakLoader: Failed to read vtable from instance %p", m_pakPlatformFile);
        return false;
    }
    m_vtable = (void**)vt;
    logInfo("PakLoader: seeded instance=%p, vtable=%p (from EngineAPI reverse-discovery)",
            m_pakPlatformFile, m_vtable);

    void* mountFn = findPakMountFn(gm);
    if (!mountFn) {
        logError("PakLoader: findPakMountFn failed");
        return false;
    }
    m_mountFunc = mountFn;

    logInfo("PakLoader: Ready (via instance) - pak=%p, mount=%p", m_pakPlatformFile, m_mountFunc);
    return true;
}

bool PakLoader::initialize() {
    logInfo("PakLoader: Initializing...");

    GameModule gm = findGameModule();
    if (!gm.base) { logError("PakLoader: Game module not found"); return false; }
    logInfo("PakLoader: Game exe %zu MB", gm.size / (1024*1024));

    void** vtable = findPakVtable(gm);
    if (!vtable) return false;
    m_vtable = vtable;

    void* singleton = findPakSingleton(gm, vtable);
    if (!singleton) return false;
    m_pakPlatformFile = singleton;

    void* mountFn = findPakMountFn(gm);
    if (!mountFn) return false;
    m_mountFunc = mountFn;

    logInfo("PakLoader: Ready - pak=%p, mount=%p", m_pakPlatformFile, m_mountFunc);
    return true;
}

PakMountResult PakLoader::mountPak(const std::string& pakPath, const std::string& modId, int priority) {
    PakMountResult result{pakPath, modId, false, ""};

    if (!m_pakPlatformFile || !m_mountFunc) {
        result.error = "Not initialized";
        return result;
    }

    // Build wide path. UE wants forward-slash-separated paths internally;
    // FPakPlatformFile normalizes them but we'll feed it normalized form.
    std::wstring widePath(pakPath.begin(), pakPath.end());
    for (auto& ch : widePath) if (ch == L'\\') ch = L'/';

    // Build FPakMountArgs on the stack. WithSoftReferences mirrors the
    // engine's startup-mount path on a 5.6 cook (the .utoc may carry the
    // soft-references segment per V=5 container header layout).
    FPakMountArgsShim args = {};
    args.PakFilename = widePath.c_str();
    args.PakOrder    = (uint32_t)priority;
    args.Path        = nullptr;
    args.MountFlags  = EMountFlags_WithSoftReferences;
    args.bLoadIndex  = 1;

    logInfo("PakLoader: Mount('%s', order=%d, flags=0x%X)",
            pakPath.c_str(), priority, args.MountFlags);

    // Snapshot first 320 bytes of FPakPlatformFile to detect MountedPaks
    // TArray increments - useful diagnostic regardless of return value.
    uint8_t snapBefore[320] = {};
    safeReadBytes(m_pakPlatformFile, snapBefore, 320);

    bool mountReturn = false;
    bool ok = sehCallPakMount(m_mountFunc, m_pakPlatformFile, &args, &mountReturn);

    logInfo("PakLoader: Mount returned ok=%d, ret=%d", ok ? 1 : 0, mountReturn ? 1 : 0);

    {
        uint8_t snapAfter[320] = {};
        if (safeReadBytes(m_pakPlatformFile, snapAfter, 320)) {
            for (int off = 0; off + 4 <= 320; off += 4) {
                int32_t bef = *(int32_t*)(snapBefore + off);
                int32_t aft = *(int32_t*)(snapAfter + off);
                if (aft == bef + 1 && bef >= 0 && bef < 1000) {
                    logInfo("PakLoader: FPakPlatformFile+0x%02X: %d->%d "
                            "(TArray count++ - pak entered an internal list)", off, bef, aft);
                }
            }
        }
    }

    if (!ok) { result.error = "Mount crashed"; return result; }
    if (!mountReturn) {
        // Mount returned false. On 5.6 this commonly means the .pak file
        // was found but Pak->IsValid() failed, or the IoStore companion
        // mount didn't succeed. Either is informative diagnostic; surface
        // as a soft failure but keep going so subsequent mods still run.
        result.error = "Mount returned false";
        return result;
    }
    result.success = true;
    m_mountedCount++;
    return result;
}

std::vector<PakMountResult> PakLoader::mountAll(const std::vector<std::tuple<std::string, std::string, int>>& paks) {
    std::vector<PakMountResult> results;
    for (const auto& [path, modId, priority] : paks)
        results.push_back(mountPak(path, modId, priority));
    return results;
}

// -- Verification ---------------------------------------------------------

fs::path PakLoader::findContentPaksDir(const std::string& gameDir) const {
    fs::path dir(gameDir);
    for (int i = 0; i < 5; i++) {
        fs::path c = dir / "Content" / "Paks";
        if (fs::is_directory(c)) return c;
        fs::path p = dir.parent_path();
        if (p == dir) break;
        dir = p;
    }
    return {};
}

size_t PakLoader::verifyDeployedPaks(const std::string& gameDir) const {
    fs::path cp = findContentPaksDir(gameDir);
    if (cp.empty()) return 0;
    size_t found = 0;
    fs::path dirs[] = { cp / "HydroMods", cp / "LogicMods", cp };
    for (const auto& dir : dirs) {
        if (!fs::is_directory(dir)) continue;
        for (const auto& e : fs::directory_iterator(dir)) {
            if (e.is_regular_file() && e.path().filename().string().rfind("pakchunk", 0) == 0) {
                found++;
                logInfo("  Pak: %s", e.path().filename().string().c_str());
            }
        }
    }
    return found;
}

} // namespace Hydro
