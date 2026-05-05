#include "EngineAPI.h"
#include "HydroCore.h"
#include "RawFunctions.h"
#include "SEH.h"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <algorithm>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif

/*
 * EngineAPI: pure C++ engine reflection layer. No UE4SS headers touched here.
 *
 * Bootstrap sequence:
 *   1. Find game module (largest loaded DLL/exe)
 *   2. Pattern scan for GUObjectArray via the "Unable to add more objects" string
 *   3. Pattern scan for StaticLoadObject via the "Failed to find object" string
 *   4. Pattern scan for FName::ToString via the "None" string
 *   5. Get ProcessEvent from the first UObject's vtable at offset 0x278
 *   6. Iterate GUObjectArray to find UWorld, GameplayStatics CDO, spawn UFunctions
 *   7. Ready to spawn actors via ProcessEvent
 */

namespace Hydro::Engine {

// Game module discovery (reused from PakLoader pattern)

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

// SEH wrappers

static bool safeReadPtr(void* addr, void** out) {
    HYDRO_SEH_TRY(*out = *(void**)addr);
}

static bool safeReadInt32(void* addr, int32_t* out) {
    HYDRO_SEH_TRY(*out = *(int32_t*)addr);
}

// Wide string search helper

static uint8_t* findWideString(uint8_t* base, size_t size, const wchar_t* str) {
    size_t strBytes = wcslen(str) * 2;
    for (size_t i = 0; i + strBytes <= size; i++) {
        if (memcmp(base + i, str, strBytes) == 0) return base + i;
    }
    return nullptr;
}

// Find all LEA instructions referencing a target address
static uint8_t* findLeaRef(uint8_t* base, size_t size, uint8_t* target) {
    for (size_t i = 0; i + 7 < size; i++) {
        if ((base[i] == 0x48 || base[i] == 0x4C) && base[i+1] == 0x8D) {
            uint8_t modrm = base[i+2];
            if ((modrm & 0xC7) != 0x05 && (modrm & 0xC7) != 0x0D) continue;
            int32_t disp = *(int32_t*)(base + i + 3);
            if (base + i + 7 + disp == target) return base + i;
        }
    }
    return nullptr;
}

// Walk backwards from an instruction to find function start (CC padding)
static uint8_t* walkToFuncStart(uint8_t* addr, int maxBack = 16384) {
    uint8_t* p = addr;
    for (int i = 0; i < maxBack; i++) {
        if (p[-1] == 0xCC) return p;
        p--;
    }
    return addr; // fallback: use the lea location
}

// Forward declarations
static void seedAndResolveRawFunctions();
static bool findFNameConstructor();
static bool discoverAssetRegistry();
static bool discoverConvNameToString();
static bool discoverFNamePool();
static bool findOpenShaderLibrary();
static bool callProcessEvent(void* obj, void* func, void* params);
static void* findObjectViaScan(const wchar_t* path);
static int readPoolEntryRaw(uint32_t comparisonIndex, char* buf, int bufSize, bool* isWide);
static void staticLoadObjectCrashed(bool wasFromCache);
// Set to true after a fatal init failure (SLO sanity-test crash). Stops
// init from re-running in the same process - cached pointer is bad and
// retrying would crash again. Cache file is invalidated for next launch.
static bool s_initFatallyFailed = false;

// Cached state

static bool s_ready = false;
static GameModule s_gm = {};

// FNamePool state (declared early - used by cache load/save and pool discovery)
static bool s_poolReady = false;
static void* s_fnamePool = nullptr;
static int s_poolBlocksOffset = -1;

// Discovered function pointers
static void* s_guObjectArray = nullptr;     // FUObjectArray*
static void* s_staticLoadObject = nullptr;  // StaticLoadObject function

// Discovered via ProcessEvent (vtable offset)
using ProcessEventFn = void(__fastcall*)(void*, void*, void*);
static ProcessEventFn s_processEvent = nullptr;

// Discovered engine objects
static void* s_world = nullptr;               // UWorld*
static void* s_gameplayStaticsCDO = nullptr;   // Default__GameplayStatics
static void* s_spawnFunc = nullptr;            // BeginDeferredActorSpawnFromClass UFunction*
static void* s_finishSpawnFunc = nullptr;      // FinishSpawningActor UFunction*
static void* s_getPlayerCharacterFunc = nullptr; // GameplayStatics.GetPlayerCharacter UFunction*
static void* s_getPlayerPawnFunc = nullptr;      // GameplayStatics.GetPlayerPawn UFunction*
static void* s_getAllActorsOfClassFunc = nullptr; // GameplayStatics.GetAllActorsOfClass UFunction*

// UStruct::SuperStruct offset, discovered at init via an AActor->Object probe.
// -1 until discovery runs; callers fall back to the 0x40 UE 5.5 default.
static int s_superOffset = -1;

// AssetRegistry loading (same dispatch path BPModLoaderMod uses)
static void* s_fnameConstructor = nullptr;     // FName(const TCHAR*, EFindName) function
static void* s_assetRegHelpersCDO = nullptr;   // Default__AssetRegistryHelpers
static void* s_getAssetFunc = nullptr;         // AssetRegistryHelpers:GetAsset UFunction
static void* s_assetRegImpl = nullptr;         // AssetRegistry implementation object
static void* s_getByPathFunc = nullptr;        // AssetRegistry:GetAssetByObjectPath UFunction

// Variables used by both cache code and pattern scans
static void* s_realStaticFindObject = nullptr;
static void* s_loadPackage = nullptr;
static void* s_staticFindObject = nullptr;
static void* s_staticFindObjectFast = nullptr;   // StaticFindObjectFast (useful reflection anchor)
static void* s_openShaderLibrary = nullptr;      // FShaderCodeLibrary::OpenLibrary - register a mounted pak's shader archive
static void* s_gmalloc = nullptr;                // UE's global FMalloc* (GMalloc) - needed to allocate UE-owned buffers

// Forward decl: drops every cached UFunction parameter layout so subsequent
// calls re-derive via the (now-correct) Layer 2 offsets. Defined later
// alongside the layout cache itself.
static void clearUFuncLayoutCache();

// Layer 2 - discovered struct layout. Defined early so the scan cache
// load/save can persist it. Real probing happens in discoverPropertyLayout()
// further down. -1 fields = not yet discovered, fall back to hardcoded.
// Includes the FField/UStruct chain-walk offsets too: Palworld's UE 5.1.x
// fork shifts these by +8 bytes vs. stock UE 5.5, so even findProperty
// needs the discovered values to walk an FProperty chain at all.
struct PropertyLayoutCache {
    // FField/UStruct chain-walk offsets (probed first; needed to do anything else)
    int32_t childPropsOffset = -1;   // UStruct::ChildProperties (FField*)
    int32_t fieldNextOffset  = -1;   // FField::Next (FField*)
    int32_t fieldNameOffset  = -1;   // FField::NamePrivate (FName)
    // FProperty internal offsets (probed after chain walk works)
    int32_t offsetInternal   = -1;
    int32_t elementSize      = -1;
    int32_t flags            = -1;
    bool    initialized      = false;
    bool    succeeded        = false;
};
static PropertyLayoutCache s_layout;

// Pattern scan cache
// Caches function offsets (relative to game module base) so we skip the
// expensive 6+ second pattern scan on subsequent launches.
// Cache is invalidated when the game binary size changes (i.e., game update).

struct ScanCache {
    uint32_t magic;                  // bumped whenever layout changes
    size_t moduleSize;               // Game module size - cache key
    ptrdiff_t staticLoadObject;      // StaticLoadObject offset from base
    ptrdiff_t fnameConstructor;      // FName constructor offset from base
    ptrdiff_t guObjectArray;         // GUObjectArray offset from base
    ptrdiff_t realStaticFindObject;  // Real StaticFindObject offset
    ptrdiff_t loadPackage;           // LoadPackageInternal offset
    ptrdiff_t fnamePool;             // FNamePool offset from base (0 = not cached)
    int32_t   poolBlocksOffset;      // Blocks[] offset within FNamePool struct
    ptrdiff_t staticFindObjectFast;  // StaticFindObjectFast offset
    ptrdiff_t openShaderLibrary;     // FShaderCodeLibrary::OpenLibrary offset
    // Layer 2/4 discovered struct field offsets (-1 = not yet discovered)
    int32_t childPropsOffset;
    int32_t fieldNextOffset;
    int32_t fieldNameOffset;
    int32_t fpropOffsetInternal;
    int32_t fpropElementSize;
    int32_t fpropFlags;
};

// Bumped to 0x4859445C - invalidates any caches written by prior in-progress
// experiments so loadScanCache always pattern-scans fresh on first launch.
static const uint32_t CACHE_MAGIC = 0x4859445C;

static std::string getCachePath() {
    char path[MAX_PATH];
    HMODULE hm = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        (LPCSTR)&getCachePath, &hm);
    GetModuleFileNameA(hm, path, MAX_PATH);
    std::string dir(path);
    auto pos = dir.find_last_of("\\/");
    if (pos != std::string::npos) dir = dir.substr(0, pos);
    return dir + "\\hydro_scan_cache.bin";
}

// Invoked when StaticLoadObject's sanity-test crash happens. Lives outside
// initialize() so std::filesystem::path destructors don't conflict with the
// __try block (C2712) inside initialize. Deletes the scan cache file so the
// next launch re-discovers offsets fresh.
static void staticLoadObjectCrashed(bool wasFromCache) {
    Hydro::logError("EngineAPI: StaticLoadObject CRASHED - %s",
        wasFromCache ? "stale cached pointer; will pattern-scan fresh next tick"
                     : "wrong function from fresh scan; giving up to avoid retry storm");
    s_staticLoadObject = nullptr;
    s_staticFindObject = nullptr;
    if (wasFromCache) {
        // Cache was loaded from a prior run with a different ASLR base.
        // The crash means cached pointers are off by the ASLR delta - and
        // SLO wasn't the only one cached. EVERY pointer restored from the
        // scan cache (loadScanCache, ~line 277) carries the stale base.
        // We must zero them all *and* clear the readiness flags that gate
        // re-discovery, otherwise the next initialize() call sees
        // s_poolReady=true and skips FNamePool re-scan, leaving every
        // subsequent getObjectName / findObject reading garbage.
        s_fnamePool            = nullptr;
        s_guObjectArray        = nullptr;
        s_fnameConstructor     = nullptr;
        s_realStaticFindObject = nullptr;
        s_loadPackage          = nullptr;
        s_staticFindObjectFast = nullptr;
        s_openShaderLibrary    = nullptr;
        s_poolReady            = false;  // <- critical: gates discoverFNamePool
        // s_layout.* is intentionally kept - those are struct offsets
        // (e.g. 0x4C for FPROP_OFFSET_INTERNAL), not pointers. They don't
        // shift with ASLR and stay valid across launches.
        Hydro::logInfo("EngineAPI: cleared all cached pointers + s_poolReady - full re-discovery next tick");
    }
    std::error_code ec;
    std::filesystem::remove(getCachePath(), ec);
    // Only mark fatal if we already pattern-scanned from scratch and STILL
    // crashed - that means our discovery is wrong, not just stale.
    // Cache-load crashes are recoverable: clearing the file lets the next
    // initialize() attempt run a clean pattern scan.
    if (!wasFromCache) s_initFatallyFailed = true;
}

static bool loadScanCache(const GameModule& gm) {
    auto path = getCachePath();
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;

    ScanCache cache = {};
    bool ok = fread(&cache, sizeof(cache), 1, f) == 1;
    fclose(f);

    if (!ok || cache.magic != CACHE_MAGIC || cache.moduleSize != gm.size) {
        Hydro::logInfo("EngineAPI: Cache invalid - full scan needed");
        return false;
    }

    // Restore offsets
    s_staticLoadObject = cache.staticLoadObject ? (gm.base + cache.staticLoadObject) : nullptr;
    s_fnameConstructor = cache.fnameConstructor ? (gm.base + cache.fnameConstructor) : nullptr;
    s_guObjectArray = cache.guObjectArray ? (void*)(gm.base + cache.guObjectArray) : nullptr;
    s_realStaticFindObject = cache.realStaticFindObject ? (gm.base + cache.realStaticFindObject) : nullptr;
    s_loadPackage = cache.loadPackage ? (gm.base + cache.loadPackage) : nullptr;
    s_staticFindObjectFast = cache.staticFindObjectFast ? (gm.base + cache.staticFindObjectFast) : nullptr;
    s_openShaderLibrary    = cache.openShaderLibrary    ? (gm.base + cache.openShaderLibrary)    : nullptr;
    if (cache.fnamePool && cache.poolBlocksOffset > 0) {
        s_fnamePool = gm.base + cache.fnamePool;
        s_poolBlocksOffset = cache.poolBlocksOffset;
        s_poolReady = true;
        Hydro::logInfo("EngineAPI: FNamePool restored from cache at %p (blocksOffset=0x%X)",
            s_fnamePool, s_poolBlocksOffset);
    }

    // Restore discovered struct layout (Layer 2 / chain-walk + FProperty).
    // Treat any negative value as "not cached" - discovery re-runs lazily.
    if (cache.childPropsOffset >= 0 && cache.fieldNextOffset >= 0 && cache.fieldNameOffset >= 0 &&
        cache.fpropOffsetInternal >= 0 && cache.fpropElementSize >= 0 && cache.fpropFlags >= 0) {
        s_layout.childPropsOffset = cache.childPropsOffset;
        s_layout.fieldNextOffset  = cache.fieldNextOffset;
        s_layout.fieldNameOffset  = cache.fieldNameOffset;
        s_layout.offsetInternal   = cache.fpropOffsetInternal;
        s_layout.elementSize      = cache.fpropElementSize;
        s_layout.flags            = cache.fpropFlags;
        s_layout.initialized      = true;
        s_layout.succeeded        = true;
        Hydro::logInfo("EngineAPI: struct layout restored from cache "
                       "(CHILD_PROPS=0x%X FF_NEXT=0x%X FF_NAME=0x%X "
                       "OFFSET_INTERNAL=0x%X ELEMENT_SIZE=0x%X FLAGS=0x%X)",
                       s_layout.childPropsOffset, s_layout.fieldNextOffset, s_layout.fieldNameOffset,
                       s_layout.offsetInternal, s_layout.elementSize, s_layout.flags);
    }

    // Validate: first bytes of StaticLoadObject should look like code
    if (s_staticLoadObject) {
        uint8_t* fn = (uint8_t*)s_staticLoadObject;
        bool looksLikeCode = (fn[0] == 0x48 || fn[0] == 0x40 || fn[0] == 0x41 ||
                               fn[0] == 0x55 || fn[0] == 0x53 || fn[0] == 0x56 ||
                               fn[0] == 0x4C || fn[0] == 0xE9);
        if (!looksLikeCode) {
            Hydro::logWarn("EngineAPI: Cached StaticLoadObject looks wrong - re-scanning");
            s_staticLoadObject = nullptr;
            s_fnameConstructor = nullptr;
            s_guObjectArray = nullptr;
            return false;
        }
    }

    // Also set s_staticFindObject (alias used by discoverAssetRegistry fallback)
    s_staticFindObject = s_staticLoadObject;

    Hydro::logInfo("EngineAPI: Loaded scan cache - skipping pattern scan!");
    return true;
}

static void saveScanCache(const GameModule& gm) {
    ScanCache cache = {};
    cache.magic = CACHE_MAGIC;
    cache.moduleSize = gm.size;
    cache.staticLoadObject = s_staticLoadObject ? ((uint8_t*)s_staticLoadObject - gm.base) : 0;
    cache.fnameConstructor = s_fnameConstructor ? ((uint8_t*)s_fnameConstructor - gm.base) : 0;
    cache.guObjectArray = s_guObjectArray ? ((uint8_t*)s_guObjectArray - gm.base) : 0;
    cache.realStaticFindObject = s_realStaticFindObject ? ((uint8_t*)s_realStaticFindObject - gm.base) : 0;
    cache.loadPackage = s_loadPackage ? ((uint8_t*)s_loadPackage - gm.base) : 0;
    cache.fnamePool = (s_poolReady && s_fnamePool) ? ((uint8_t*)s_fnamePool - gm.base) : 0;
    cache.poolBlocksOffset = s_poolReady ? s_poolBlocksOffset : 0;
    cache.staticFindObjectFast = s_staticFindObjectFast ? ((uint8_t*)s_staticFindObjectFast - gm.base) : 0;
    cache.openShaderLibrary    = s_openShaderLibrary    ? ((uint8_t*)s_openShaderLibrary    - gm.base) : 0;
    cache.childPropsOffset     = (s_layout.succeeded ? s_layout.childPropsOffset : -1);
    cache.fieldNextOffset      = (s_layout.succeeded ? s_layout.fieldNextOffset  : -1);
    cache.fieldNameOffset      = (s_layout.succeeded ? s_layout.fieldNameOffset  : -1);
    cache.fpropOffsetInternal  = (s_layout.succeeded ? s_layout.offsetInternal   : -1);
    cache.fpropElementSize     = (s_layout.succeeded ? s_layout.elementSize      : -1);
    cache.fpropFlags           = (s_layout.succeeded ? s_layout.flags            : -1);

    auto path = getCachePath();
    FILE* f = fopen(path.c_str(), "wb");
    if (f) {
        fwrite(&cache, sizeof(cache), 1, f);
        fclose(f);
        Hydro::logInfo("EngineAPI: Scan cache saved to %s", path.c_str());
    }
}

// GUObjectArray discovery

static bool findGUObjectArray() {
    // Scan for L"Unable to add more objects to disregard for GC pool"
    uint8_t* strAddr = findWideString(s_gm.base, s_gm.size,
        L"Unable to add more objects to disregard for GC pool");

    if (!strAddr) {
        Hydro::logWarn("EngineAPI: GC pool string not found - trying alternate");
        // Try shorter unique substring
        strAddr = findWideString(s_gm.base, s_gm.size,
            L"MaxObjectsNotConsideredByGC");
        if (!strAddr) {
            Hydro::logError("EngineAPI: GUObjectArray strings not found");
            return false;
        }
    }

    Hydro::logInfo("EngineAPI: GC string at exe+0x%zX", (size_t)(strAddr - s_gm.base));

    // Find LEA referencing this string
    uint8_t* leaAddr = findLeaRef(s_gm.base, s_gm.size, strAddr);
    if (!leaAddr) {
        Hydro::logError("EngineAPI: No LEA ref to GC string");
        return false;
    }

    // Walk to function start
    uint8_t* funcStart = walkToFuncStart(leaAddr);
    Hydro::logInfo("EngineAPI: GC function at exe+0x%zX", (size_t)(funcStart - s_gm.base));

    // AllocateObjectPool is a member function of FUObjectArray - 'this' (rcx) is
    // set by the caller, not loaded internally. Find callers and read the
    // 'lea rcx, [rip+...]' that precedes each call to recover GUObjectArray.
    Hydro::logInfo("EngineAPI: Searching for callers of AllocateObjectPool...");

    // Two possible global layouts in UE 5.x:
    //   (a) TUObjectArray-direct: global IS the FChunkedFixedUObjectArray
    //       (NumElements @ +0x14, Objects @ +0x00).
    //   (b) FUObjectArray-wrapper: global is FUObjectArray with inner
    //       TUObjectArray ObjObjects at +0x10. NumElements @ +0x24.
    // UE 5.1 / Palworld observed (a); UE 5.5 / DummyModdableGame observed (b).
    // Probe both - accept the first that has a plausible NumElements.
    constexpr int FARRAY_OBJOBJECTS_OFFSET = 0x10;  // FUObjectArray::ObjObjects
    auto tryAcceptCandidate = [&](void* resolved) -> bool {
        if ((uint8_t*)resolved < s_gm.base || (uint8_t*)resolved >= s_gm.base + s_gm.size)
            return false;
        // Try TUObjectArray-direct first
        int32_t numElems = 0;
        if (safeReadInt32((uint8_t*)resolved + FARRAY_NUM_ELEMS, &numElems) &&
            numElems > 1000 && numElems < 2000000) {
            s_guObjectArray = resolved;
            Hydro::logInfo("EngineAPI: GUObjectArray at %p (%d objects, TUObjectArray-direct shape)",
                resolved, numElems);
            return true;
        }
        // Try FUObjectArray-wrapper: inner TUObjectArray at +0x10
        uint8_t* inner = (uint8_t*)resolved + FARRAY_OBJOBJECTS_OFFSET;
        if (safeReadInt32(inner + FARRAY_NUM_ELEMS, &numElems) &&
            numElems > 1000 && numElems < 2000000) {
            s_guObjectArray = inner;
            Hydro::logInfo("EngineAPI: GUObjectArray at %p (%d objects, FUObjectArray-wrapper, inner TUObjectArray @ %p)",
                resolved, numElems, inner);
            return true;
        }
        return false;
    };

    for (size_t i = 0; i + 5 < s_gm.size; i++) {
        if (s_gm.base[i] != 0xE8) continue; // CALL rel32
        int32_t rel = *(int32_t*)(s_gm.base + i + 1);
        uint8_t* target = s_gm.base + i + 5 + rel;
        if (target != funcStart) continue;

        // Found a call to AllocateObjectPool at exe+i
        // Search backwards (up to 64 bytes) for 'lea rcx, [rip+xx]' (48 8D 0D xx xx xx xx)
        for (int back = 5; back < 64; back++) {
            uint8_t* p = s_gm.base + i - back;
            if (p < s_gm.base) break;
            // lea rcx, [rip+disp32] - sets 'this' pointer for the call
            if (p[0] == 0x48 && p[1] == 0x8D && p[2] == 0x0D) {
                int32_t disp = *(int32_t*)(p + 3);
                void* resolved = p + 7 + disp;
                if (tryAcceptCandidate(resolved)) return true;
            }
        }
    }

    Hydro::logError("EngineAPI: GUObjectArray caller not found - trying data scan");

    // Fallback: scan the module's data section directly for a plausible
    // FChunkedFixedUObjectArray. Instead of searching code for LEA instructions
    // (millions of matches, very slow), scan writable memory at pointer-aligned
    // stride. The layout is:
    //   +0x00: void** Objects (chunk table pointer - heap address)
    //   +0x08: void*  PreAllocated (may be null)
    //   +0x10: int32  MaxElements
    //   +0x14: int32  NumElements
    // We look for locations where NumElements is large (>50k) and MaxElements
    // >= NumElements. The real GUObjectArray is the largest.

    // Find the data sections via PE header
    auto* dosHeader = (IMAGE_DOS_HEADER*)s_gm.base;
    auto* ntHeaders = (IMAGE_NT_HEADERS*)(s_gm.base + dosHeader->e_lfanew);
    auto* section = IMAGE_FIRST_SECTION(ntHeaders);
    int numSections = ntHeaders->FileHeader.NumberOfSections;

    void* bestCandidate = nullptr;
    int32_t bestCount = 0;
    const char* bestShape = nullptr;

    uint8_t* moduleStart = s_gm.base;
    uint8_t* moduleEnd   = s_gm.base + s_gm.size;
    auto isHeapPtr = [&](void* p) {
        if (!p) return false;
        uint8_t* pb = (uint8_t*)p;
        if (pb >= moduleStart && pb < moduleEnd) return false;
        if ((uintptr_t)p < 0x10000) return false;
        if (((uintptr_t)p & 0x7) != 0) return false;
        return true;
    };

    // Probe one address as a candidate TUObjectArray (Objects @ +0x00,
    // MaxElements @ +0x10, NumElements @ +0x14). Returns NumElements if
    // the candidate validates (chunk table dereferences, first 5 entries
    // contain heap-resident UObjects with heap-resident class pointers),
    // else 0. The threshold is intentionally low (1000) so tiny test
    // games like DummyModdableGame pass; small enough random ints in
    // .data are filtered out by the structural validation that follows.
    auto probeShape = [&](uint8_t* addr) -> int32_t {
        int32_t numElems = *(int32_t*)(addr + FARRAY_NUM_ELEMS);
        if (numElems < 1000 || numElems > 2000000) return 0;
        int32_t maxElems = *(int32_t*)(addr + FARRAY_MAX_ELEMS);
        if (maxElems < numElems) return 0;
        void* objects = *(void**)addr;
        if (!isHeapPtr(objects)) return 0;
        void* chunk0 = nullptr;
        if (!safeReadPtr(objects, &chunk0) || !chunk0) return 0;

        int validEntries = 0;
        int probedEntries = 0;
        for (int e = 0; e < 5; e++) {
            void* obj = nullptr;
            if (!safeReadPtr((uint8_t*)chunk0 + (e * FUOBJ_SIZE) + FUOBJ_OBJECT, &obj)) continue;
            if (!obj) continue;
            probedEntries++;
            if (!isHeapPtr(obj)) break;
            void* cls = nullptr;
            if (!safeReadPtr((uint8_t*)obj + UOBJ_CLASS, &cls)) break;
            if (!cls) break;
            if (!isHeapPtr(cls)) break;
            validEntries++;
        }
        if (validEntries < 3) return 0;
        return numElems;
    };

    for (int sec = 0; sec < numSections; sec++, section++) {
        // Only scan writable sections (.data, .rdata won't have mutable counts)
        if (!(section->Characteristics & IMAGE_SCN_MEM_WRITE)) continue;

        uint8_t* secStart = s_gm.base + section->VirtualAddress;
        size_t secSize = section->Misc.VirtualSize;
        if (secSize < 0x30) continue;

        // Scan at 8-byte alignment (struct is pointer-aligned). Probe both
        // global layouts at each address:
        //   (a) addr is TUObjectArray-direct - UE 5.1 / Palworld
        //   (b) addr is FUObjectArray, inner TUObjectArray @ +0x10 - UE 5.5
        for (size_t off = 0; off + 0x30 <= secSize; off += 8) {
            uint8_t* addr = secStart + off;

            // (a) Direct shape
            int32_t n = probeShape(addr);
            if (n > 0) {
                Hydro::logInfo("EngineAPI: GUObjectArray candidate at %p (%d objects, exe+0x%zX, TUObjectArray-direct)",
                    addr, n, (size_t)(addr - s_gm.base));
                if (n > bestCount) {
                    bestCandidate = addr;
                    bestCount = n;
                    bestShape = "TUObjectArray-direct";
                }
                continue;  // don't probe wrapper shape at same offset
            }

            // (b) FUObjectArray-wrapper shape: inner TUObjectArray at +0x10
            uint8_t* inner = addr + 0x10;
            n = probeShape(inner);
            if (n > 0) {
                Hydro::logInfo("EngineAPI: GUObjectArray candidate at %p (FUObjectArray, inner @ %p, %d objects, exe+0x%zX)",
                    addr, inner, n, (size_t)(addr - s_gm.base));
                if (n > bestCount) {
                    // Store inner pointer - iteration code uses
                    // TUObjectArray-direct offsets unchanged.
                    bestCandidate = inner;
                    bestCount = n;
                    bestShape = "FUObjectArray-wrapper";
                }
            }
        }
    }

    if (bestCandidate) {
        s_guObjectArray = bestCandidate;
        Hydro::logInfo("EngineAPI: GUObjectArray at %p (%d objects, %s) - found via data section scan",
            bestCandidate, bestCount, bestShape ? bestShape : "?");
        return true;
    }

    Hydro::logError("EngineAPI: GUObjectArray not found by any method");
    return false;
}

// GUObjectArray iteration

// Read a UObject* from GUObjectArray at the given index
void* getObjectAt(int32_t index) {
    if (!s_guObjectArray || index < 0) return nullptr;

    // Read chunk table pointer
    void** chunkTable = nullptr;
    if (!safeReadPtr((uint8_t*)s_guObjectArray + FARRAY_OBJECTS, (void**)&chunkTable) || !chunkTable)
        return nullptr;

    int32_t chunkIndex = index / CHUNK_SIZE;
    int32_t withinChunk = index % CHUNK_SIZE;

    // Read chunk pointer
    void* chunk = nullptr;
    if (!safeReadPtr(&chunkTable[chunkIndex], &chunk) || !chunk)
        return nullptr;

    // Each FUObjectItem is FUOBJ_SIZE bytes. Object pointer is at offset 0.
    uint8_t* item = (uint8_t*)chunk + (withinChunk * FUOBJ_SIZE);
    void* obj = nullptr;
    if (!safeReadPtr(item + FUOBJ_OBJECT, &obj))
        return nullptr;

    return obj;
}

int32_t getObjectCount() {
    if (!s_guObjectArray) return 0;
    int32_t count = 0;
    safeReadInt32((uint8_t*)s_guObjectArray + FARRAY_NUM_ELEMS, &count);
    return count;
}

// Read the FName ComparisonIndex from a UObject (offset 0x18, first 4 bytes)
uint32_t getNameIndex(void* obj) {
    if (!obj) return 0;
    uint32_t idx = 0;
#ifdef _WIN32
    __try { idx = *(uint32_t*)((uint8_t*)obj + UOBJ_NAME); }
    __except(1) { return 0; }
#else
    idx = *(uint32_t*)((uint8_t*)obj + UOBJ_NAME);
#endif
    return idx;
}

// Read ClassPrivate from a UObject (offset 0x10)
// Note: public getClass() is defined at bottom of file. This is the early
// static version used by internal discovery code before public API section.
void* getObjClass(void* obj) {
    if (!obj) return nullptr;
    void* cls = nullptr;
    safeReadPtr((uint8_t*)obj + UOBJ_CLASS, &cls);
    return cls;
}

// Name resolution uses FName index comparisons instead of FName::ToString:
// we construct an FName for the known target string and compare its
// ComparisonIndex against candidates. This avoids needing the FName pool.

// StaticLoadObject discovery

using StaticLoadObjectFn_t = void*(__fastcall*)(
    void* Class, void* InOuter, const wchar_t* Name, const wchar_t* Filename,
    uint32_t LoadFlags, void* Sandbox, bool bAllowReconciliation, void* InstancingContext);

// s_staticFindObject is forward-declared before the cache section
// s_realStaticFindObject and s_loadPackage are forward-declared before the cache section

// SEH-wrapped test call for candidate load functions
static void* safeCallLoadObject(void* funcAddr, const wchar_t* path, uint32_t flags = 0) {
#ifdef _WIN32
    __try {
        auto fn = (StaticLoadObjectFn_t)funcAddr;
        return fn(nullptr, nullptr, path, nullptr, flags, nullptr, true, nullptr);
    } __except(1) { return nullptr; }
#else
    auto fn = (StaticLoadObjectFn_t)funcAddr;
    return fn(nullptr, nullptr, path, nullptr, flags, nullptr, true, nullptr);
#endif
}

// SEH-wrapped 4-arg StaticFindObject: UObject*(UClass*, UObject*, const TCHAR*, bool).
// Used to behaviorally validate candidate StaticFindObject functions.
using StaticFindObjectFn_t = void*(__fastcall*)(void*, void*, const wchar_t*, bool);
static void* safeCallFindObject(void* funcAddr, const wchar_t* path) {
#ifdef _WIN32
    __try {
        auto fn = (StaticFindObjectFn_t)funcAddr;
        return fn(nullptr, nullptr, path, false);
    } __except(1) { return nullptr; }
#else
    auto fn = (StaticFindObjectFn_t)funcAddr;
    return fn(nullptr, nullptr, path, false);
#endif
}

// Anchor cache: the Object UClass pointer, resolved once via the GUObjectArray
// walk. Used to behaviorally validate candidate StaticFindObject /
// StaticLoadObject functions by pointer equality - bypasses getNameString
// which depends on the FName resolver (not yet up during initial discovery).
static void* getObjectClassAnchor() {
    static void* s_objectClass = nullptr;
    if (s_objectClass) return s_objectClass;
    if (!s_guObjectArray || !s_fnameConstructor) return nullptr;
    s_objectClass = findObjectViaScan(L"/Script/CoreUObject.Object");
    if (s_objectClass) {
        Hydro::logInfo("EngineAPI: Object UClass anchor resolved at %p", s_objectClass);
    }
    return s_objectClass;
}

// Behaviorally validate a candidate StaticFindObject. Two-tier check:
//   Tier A (best): pointer-equality against the Object UClass anchor we
//     found via GUObjectArray walk. Definitive when GUObjectArray + FName
//     ctor are up.
//   Tier B (fallback): diversity check. Call with two different paths;
//     results must (a) both be non-null heap pointers and (b) differ.
//     The buggy Palworld function returns the same /Script/AssetRegistry
//     UPackage for every path, which fails this check.
static bool validateStaticFindObjectCandidate(void* fn, std::string* outName) {
    if (outName) *outName = "<null>";
    if (!fn) return false;
    void* a = safeCallFindObject(fn, L"/Script/CoreUObject.Object");
    if (!a) return false;

    void* anchor = getObjectClassAnchor();
    if (anchor) {
        if (outName) *outName = (a == anchor) ? "<MATCH>" : "<wrong-ptr>";
        return a == anchor;
    }

    // Tier B fallback: diversity vs. /Script/Engine.Actor.
    void* b = safeCallFindObject(fn, L"/Script/Engine.Actor");
    if (!b) {
        if (outName) *outName = "<no-2nd>";
        return false;
    }
    bool ok = (a != b);
    // Also reject pointers in the game module (those are code, not heap UObjects).
    auto inMod = [](void* p) {
        return (uint8_t*)p >= s_gm.base && (uint8_t*)p < s_gm.base + s_gm.size;
    };
    if (inMod(a) || inMod(b)) ok = false;
    if (outName) *outName = ok ? "<diversity-pass>" : "<same-or-mod>";
    return ok;
}

// Behavioral validation for StaticLoadObject. Pointer-equality against the
// same Object UClass anchor - load path returns the same already-resident
// /Script/CoreUObject.Object UClass.
static bool validateStaticLoadObjectResult(void* anyObj, std::string* outName) {
    if (outName) *outName = "<null>";
    if (!anyObj) return false;
    void* anchor = getObjectClassAnchor();
    if (!anchor) return false;
    if (outName) *outName = "<got-ptr>";
    return anyObj == anchor;
}

static bool validateStaticLoadObjectCandidate(void* fn) {
    if (!fn) return false;
    void* ret = safeCallLoadObject(fn, L"/Script/CoreUObject.Object", 0);
    return validateStaticLoadObjectResult(ret, nullptr);
}

static bool findStaticLoadObject() {
    // "Failed to find object" is in StaticFindObject (find-only, no disk loading).
    // Save it as a useful bonus, but we still need the actual loader.
    uint8_t* findStr = findWideString(s_gm.base, s_gm.size, L"Failed to find object");
    if (!findStr) {
        Hydro::logWarn("EngineAPI: 'Failed to find object' string not found");
        return false;
    }

    Hydro::logInfo("EngineAPI: 'Failed to find object' at exe+0x%zX", (size_t)(findStr - s_gm.base));

    // Find ALL LEA refs to this string - there may be multiple
    // The one we want is inside StaticLoadObjectInternal
    for (size_t i = 0; i + 7 < s_gm.size; i++) {
        uint8_t* p = s_gm.base + i;
        if ((p[0] != 0x48 && p[0] != 0x4C) || p[1] != 0x8D) continue;
        uint8_t modrm = p[2];
        if ((modrm & 0xC7) != 0x05 && (modrm & 0xC7) != 0x0D &&
            (modrm & 0xC7) != 0x15 && (modrm & 0xC7) != 0x35) continue;
        int32_t disp = *(int32_t*)(p + 3);
        if (p + 7 + disp != findStr) continue;

        Hydro::logInfo("EngineAPI: LEA ref at exe+0x%zX", i);

        // Walk back looking for a proper function prologue
        // Common prologues: 48 89 5C (mov [rsp+x],rbx), 48 8B C4 (mov rax,rsp),
        // 40 53 (push rbx), 48 89 4C (mov [rsp+x],rcx), 4C 89 44 (mov [rsp+x],r8)
        // Also: sub rsp,XX after push instructions
        for (int back = 1; back < 4096; back++) {
            uint8_t* candidate = p - back;
            if (candidate < s_gm.base) break;

            // Check for CC CC padding followed by a function start
            if (candidate[-1] == 0xCC || candidate[-1] == 0xC3) {
                uint8_t b0 = candidate[0], b1 = candidate[1];
                // Verify this looks like a real function prologue
                bool prologue = (b0 == 0x48 && (b1 == 0x89 || b1 == 0x8B || b1 == 0x83)) ||
                                (b0 == 0x40 && (b1 == 0x53 || b1 == 0x55 || b1 == 0x56 || b1 == 0x57)) ||
                                (b0 == 0x41 && (b1 == 0x54 || b1 == 0x55 || b1 == 0x56 || b1 == 0x57)) ||
                                (b0 == 0x4C && b1 == 0x89) ||
                                (b0 == 0x55) || (b0 == 0x53) || (b0 == 0x56);
                if (prologue) {
                    s_staticLoadObject = candidate;
                    Hydro::logInfo("EngineAPI: StaticFindObject at exe+0x%zX (prologue: %02X %02X %02X %02X)",
                        (size_t)(candidate - s_gm.base), b0, b1, candidate[2], candidate[3]);
                    break; // Don't return - continue to find the REAL loader
                }
            }
        }

        Hydro::logWarn("EngineAPI: Could not find prologue for LEA at exe+0x%zX", i);
    }

    // What we found above is StaticFindObject (find-only, no loading).
    // Save it - useful for finding in-memory objects.
    if (s_staticLoadObject) {
        s_staticFindObject = s_staticLoadObject;
        Hydro::logInfo("EngineAPI: StaticFindObject confirmed at %p", s_staticFindObject);
    }

    // Now find the actual StaticLoadObject, which calls LoadPackage. That
    // path references "Can't find file for package" or "CreateLinker".
    // StaticLoadObject itself has "Attempting to load object" or is near "LoadObject".
    const wchar_t* loadStrings[] = {
        L"Attempting to find",
        L"LoadPackageInternal",
        L"Can't find file for package",
        L"CreateLinker",
    };

    for (auto* searchStr : loadStrings) {
        uint8_t* loadStr = findWideString(s_gm.base, s_gm.size, searchStr);
        if (!loadStr) continue;

        Hydro::logInfo("EngineAPI: Found loading string at exe+0x%zX", (size_t)(loadStr - s_gm.base));

        uint8_t* leaAddr = findLeaRef(s_gm.base, s_gm.size, loadStr);
        if (!leaAddr) continue;

        // Walk back to function prologue
        for (int back = 1; back < 8192; back++) {
            uint8_t* candidate = leaAddr - back;
            if (candidate < s_gm.base) break;
            if (candidate[-1] == 0xCC || candidate[-1] == 0xC3) {
                uint8_t b0 = candidate[0], b1 = candidate[1];
                bool prologue = (b0 == 0x48 && (b1 == 0x89 || b1 == 0x8B || b1 == 0x83)) ||
                                (b0 == 0x40 && (b1 >= 0x53 && b1 <= 0x57)) ||
                                (b0 == 0x41) || (b0 == 0x4C && b1 == 0x89) ||
                                (b0 == 0x55) || (b0 == 0x53) || (b0 == 0x56);
                if (prologue) {
                    s_staticLoadObject = candidate;
                    Hydro::logInfo("EngineAPI: LoadPackage/LoadObject at exe+0x%zX (%02X %02X %02X %02X)",
                        (size_t)(candidate - s_gm.base), b0, b1, candidate[2], candidate[3]);
                    return true;
                }
            }
        }
    }

    Hydro::logError("EngineAPI: No loading function found");
    return s_staticFindObject != nullptr;
}

// ProcessEvent discovery

static bool findProcessEvent() {
    // Get the first valid UObject from GUObjectArray
    int32_t count = getObjectCount();
    for (int32_t i = 0; i < count && i < 100; i++) {
        void* obj = getObjectAt(i);
        if (!obj) continue;

        // Read vtable pointer
        void** vtable = nullptr;
        if (!safeReadPtr(obj, (void**)&vtable) || !vtable) continue;

        // ProcessEvent is at vtable index 79 (offset 0x278)
        void* pe = nullptr;
        if (!safeReadPtr(&vtable[VTABLE_PROCESS_EVENT], &pe) || !pe) continue;

        // Verify it looks like code (in the game module range)
        if ((uint8_t*)pe >= s_gm.base && (uint8_t*)pe < s_gm.base + s_gm.size) {
            s_processEvent = (ProcessEventFn)pe;
            Hydro::logInfo("EngineAPI: ProcessEvent at %p (from obj[%d])", pe, i);
            return true;
        }
    }

    Hydro::logError("EngineAPI: ProcessEvent not found in vtables");
    return false;
}

// Engine object discovery: load known classes/UFunctions directly via
// StaticLoadObject, then locate instances by scanning GUObjectArray.

static bool discoverEngineObjects() {
    // Don't gate on s_staticLoadObject: on forked engines its pattern scan
    // may produce a wrong function that we cleared. findObject() is now
    // GUObjectArray-walk-based and resolves /Script/ classes reliably without
    // depending on any pattern-scanned C++ load primitive.
    if (!s_guObjectArray && !s_staticLoadObject) return false;

    // Resolve UWorld class via findObject (which is now resilient to broken
    // StaticLoadObject); fall back to s_staticLoadObject if findObject misses.
    void* worldClass = findObject(L"/Script/Engine.World");
    if (!worldClass && s_staticLoadObject) {
        auto loadObj = (StaticLoadObjectFn_t)s_staticLoadObject;
#ifdef _WIN32
        __try {
            worldClass = loadObj(nullptr, nullptr, L"/Script/Engine.World", nullptr, 0, nullptr, true, nullptr);
        } __except(1) { worldClass = nullptr; }
#endif
    }

    if (worldClass && s_guObjectArray) {
        Hydro::logInfo("EngineAPI: UWorld class at %p, scanning for instance...", worldClass);
        int32_t count = getObjectCount();
        for (int32_t i = 0; i < count; i++) {
            void* obj = getObjectAt(i);
            if (!obj) continue;
            if (getObjClass(obj) == worldClass) {
                s_world = obj;
                Hydro::logInfo("EngineAPI: UWorld instance at %p (index %d)", obj, i);
                break;
            }
        }
    }

    if (!s_world && worldClass) {
        // Fallback: scan 'mov rax,[rip+disp]' instructions, resolve the global
        // they reference, and check whether it points at a UWorld instance.
        // GWorld is read heavily in .text, so this converges quickly.
        Hydro::logInfo("EngineAPI: Scanning for GWorld via mov rax,[rip+xx]...");

        int checked = 0;
        for (size_t i = 0; i + 7 < s_gm.size; i++) {
            uint8_t* p = s_gm.base + i;
            // mov rax, [rip+disp32]: 48 8B 05 xx xx xx xx
            if (p[0] != 0x48 || p[1] != 0x8B || p[2] != 0x05) continue;

            int32_t disp = *(int32_t*)(p + 3);
            void** globalPtr = (void**)(p + 7 + disp);

            // Must point within the module's memory
            if ((uint8_t*)globalPtr < s_gm.base || (uint8_t*)globalPtr >= s_gm.base + s_gm.size)
                continue;

            void* candidate = nullptr;
            if (!safeReadPtr(globalPtr, &candidate) || !candidate) continue;
            if ((uintptr_t)candidate < 0x10000) continue;
            // Skip if candidate is in exe range (it should be a heap object)
            if ((uint8_t*)candidate >= s_gm.base && (uint8_t*)candidate < s_gm.base + s_gm.size)
                continue;

            void* cls = nullptr;
            if (!safeReadPtr((uint8_t*)candidate + UOBJ_CLASS, &cls)) continue;
            if (cls == worldClass) {
                s_world = candidate;
                Hydro::logInfo("EngineAPI: UWorld at %p (GWorld at %p, exe+0x%zX)",
                    candidate, globalPtr, (size_t)((uint8_t*)globalPtr - s_gm.base));
                break;
            }
            checked++;
        }
        if (!s_world)
            Hydro::logInfo("EngineAPI: Checked %d global pointers, no UWorld", checked);
    }

    if (!s_world) {
        Hydro::logWarn("EngineAPI: UWorld not found");
    }

    // Find GameplayStatics CDO + spawn UFunctions. Prefer findObject
    // (GUObjectArray walk) - it's reliable on every UE5 host. Fall back
    // to s_staticLoadObject if available for assets not yet in memory.
#ifdef _WIN32
    __try {
        void* gsClass = findObject(L"/Script/Engine.GameplayStatics");
        if (!gsClass && s_staticLoadObject) {
            auto loadObj2 = (StaticLoadObjectFn_t)s_staticLoadObject;
            gsClass = loadObj2(nullptr, nullptr, L"/Script/Engine.GameplayStatics",
                               nullptr, 0, nullptr, true, nullptr);
        }
        if (gsClass) {
            Hydro::logInfo("EngineAPI: GameplayStatics class at %p", gsClass);
            // CDO: scan GUObjectArray if available
            if (s_guObjectArray) {
                int32_t count = getObjectCount();
                for (int32_t i = 0; i < count; i++) {
                    void* obj = getObjectAt(i);
                    if (!obj) continue;
                    if (getObjClass(obj) == gsClass) {
                        s_gameplayStaticsCDO = obj;
                        Hydro::logInfo("EngineAPI: GameplayStatics CDO at %p", obj);
                        break;
                    }
                }
            }
            if (!s_gameplayStaticsCDO) {
                // ClassDefaultObject lives somewhere inside UClass; its exact offset
                // varies by build. Scan UClass+0x100..+0x200 for a pointer whose
                // ClassPrivate is gsClass (a CDO is an instance of its own class).
                Hydro::logInfo("EngineAPI: Scanning UClass for CDO pointer...");
                for (int off = 0x100; off <= 0x200; off += 8) {
                    void* candidate = nullptr;
                    if (!safeReadPtr((uint8_t*)gsClass + off, &candidate) || !candidate) continue;
                    // Check if candidate's ClassPrivate == gsClass
                    void* candidateClass = nullptr;
                    if (!safeReadPtr((uint8_t*)candidate + UOBJ_CLASS, &candidateClass)) continue;
                    if (candidateClass == gsClass) {
                        s_gameplayStaticsCDO = candidate;
                        Hydro::logInfo("EngineAPI: GameplayStatics CDO at %p (UClass+0x%X)", candidate, off);
                        break;
                    }
                }
            }
        }
    } __except(1) { Hydro::logWarn("EngineAPI: GameplayStatics discovery crashed"); }

    // Load spawn UFunctions. Prefer findObject (always works once
    // GameplayStatics class is loaded - its UFunctions are in GUObjectArray);
    // fall back to s_staticLoadObject only if findObject misses.
    auto findFn = [](const wchar_t* path) -> void* {
        return findObject(path);
    };
    s_spawnFunc       = findFn(L"/Script/Engine.GameplayStatics:BeginDeferredActorSpawnFromClass");
    s_finishSpawnFunc = findFn(L"/Script/Engine.GameplayStatics:FinishSpawningActor");
    s_getPlayerCharacterFunc = findFn(L"/Script/Engine.GameplayStatics:GetPlayerCharacter");

    s_getPlayerPawnFunc       = findFn(L"/Script/Engine.GameplayStatics:GetPlayerPawn");
    s_getAllActorsOfClassFunc = findFn(L"/Script/Engine.GameplayStatics:GetAllActorsOfClass");
#endif

    if (s_spawnFunc)
        Hydro::logInfo("EngineAPI: SpawnFunc at %p", s_spawnFunc);
    if (s_finishSpawnFunc)
        Hydro::logInfo("EngineAPI: FinishSpawnFunc at %p", s_finishSpawnFunc);
    if (s_getPlayerCharacterFunc)
        Hydro::logInfo("EngineAPI: GetPlayerCharacter at %p", s_getPlayerCharacterFunc);
    if (s_getPlayerPawnFunc)
        Hydro::logInfo("EngineAPI: GetPlayerPawn at %p", s_getPlayerPawnFunc);
    if (s_getAllActorsOfClassFunc)
        Hydro::logInfo("EngineAPI: GetAllActorsOfClass at %p", s_getAllActorsOfClassFunc);

    bool ready = s_world && s_gameplayStaticsCDO && s_spawnFunc;
    if (!ready)
        Hydro::logWarn("EngineAPI: Missing: world=%p gs=%p spawn=%p",
            s_world, s_gameplayStaticsCDO, s_spawnFunc);
    return ready;
}

// Public API: initialize

bool initialize() {
    if (s_initFatallyFailed) return false;
    Hydro::logInfo("EngineAPI: Initializing (pure C++ - no UE4SS)...");

    s_gm = findGameModule();
    if (!s_gm.base) {
        Hydro::logError("EngineAPI: Game module not found");
        return false;
    }
    Hydro::logInfo("EngineAPI: Game module %zu MB at %p", s_gm.size / (1024*1024), s_gm.base);

    // Try loading cached scan results first
    bool fromCache = loadScanCache(s_gm);

    if (!fromCache) {
        // Step 1: Find StaticLoadObject (full pattern scan)
        if (!findStaticLoadObject()) {
            Hydro::logError("EngineAPI: StaticLoadObject not found");
            return false;
        }

        // If the exe has a "StaticLoadObject" literal, prefer that function
        // over whatever findStaticLoadObject() picked above.
        uint8_t* loadObjStr = findWideString(s_gm.base, s_gm.size, L"StaticLoadObject");
        if (loadObjStr) {
            Hydro::logInfo("EngineAPI: Found 'StaticLoadObject' string at exe+0x%zX", (size_t)(loadObjStr - s_gm.base));
            uint8_t* leaRef = findLeaRef(s_gm.base, s_gm.size, loadObjStr);
            if (leaRef) {
                for (int back = 1; back < 4096; back++) {
                    uint8_t* candidate = leaRef - back;
                    if (candidate < s_gm.base) break;
                    if (candidate[-1] == 0xCC || candidate[-1] == 0xC3) {
                        uint8_t b0 = candidate[0], b1 = candidate[1];
                        bool prologue = (b0 == 0x48 && (b1 == 0x89 || b1 == 0x8B || b1 == 0x83)) ||
                                        (b0 == 0x40 && (b1 >= 0x53 && b1 <= 0x57)) ||
                                        (b0 == 0x41) || (b0 == 0x4C && b1 == 0x89) ||
                                        (b0 == 0x55) || (b0 == 0x53) || (b0 == 0x56);
                        if (prologue) {
                            Hydro::logInfo("EngineAPI: ALTERNATE StaticLoadObject at exe+0x%zX (%02X %02X %02X %02X)",
                                (size_t)(candidate - s_gm.base), b0, b1, candidate[2], candidate[3]);
                            s_staticLoadObject = candidate;
                            break;
                        }
                    }
                }
            }
        }

        // Validate the found function
        if (s_staticLoadObject) {
            uint8_t* fn = (uint8_t*)s_staticLoadObject;
            Hydro::logInfo("EngineAPI: StaticLoadObject first bytes: %02X %02X %02X %02X %02X %02X %02X %02X",
                fn[0], fn[1], fn[2], fn[3], fn[4], fn[5], fn[6], fn[7]);
            bool looksLikeCode = (fn[0] == 0x48 || fn[0] == 0x40 || fn[0] == 0x41 ||
                                   fn[0] == 0x55 || fn[0] == 0x53 || fn[0] == 0x56 ||
                                   fn[0] == 0x4C || fn[0] == 0xE9);
            if (!looksLikeCode) {
                Hydro::logError("EngineAPI: StaticLoadObject doesn't look like a function - SKIPPING calls");
                s_staticLoadObject = nullptr;
                return false;
            }
        }
    } // end !fromCache

    // Step 2: sanity-check the discovered StaticLoadObject with a known path
    Hydro::logInfo("EngineAPI: Testing StaticLoadObject call...");
    {
        auto fn = (StaticLoadObjectFn_t)s_staticLoadObject;
        void* anyObj = nullptr;
#ifdef _WIN32
        __try {
            anyObj = fn(nullptr, nullptr, L"/Script/CoreUObject.Object", nullptr, 0, nullptr, true, nullptr);
        } __except(1) {
            // Use a helper to invalidate the cache file because std::filesystem
            // destructors are incompatible with __try in this function (C2712).
            staticLoadObjectCrashed(fromCache);
            return false;
        }
#endif
        if (anyObj) {
            // Always try ProcessEvent extraction first - every UObject's
            // vtable has ProcessEvent at the same slot, so the wrong
            // function still gives us a usable PE pointer.
            void** vtable = nullptr;
            if (safeReadPtr(anyObj, (void**)&vtable) && vtable) {
                void* pe = nullptr;
                if (safeReadPtr(&vtable[VTABLE_PROCESS_EVENT], &pe) && pe &&
                    (uint8_t*)pe >= s_gm.base && (uint8_t*)pe < s_gm.base + s_gm.size) {
                    s_processEvent = (ProcessEventFn)pe;
                    Hydro::logInfo("EngineAPI: ProcessEvent at %p", pe);
                }
            }
            // Behavioral validation lives in a separate function (C2712:
            // can't combine __try with C++ unwinding objects in one func).
            bool isObject = validateStaticLoadObjectResult(anyObj, nullptr);
            if (isObject) {
                Hydro::logInfo("EngineAPI: StaticLoadObject validated (returned 'Object')");
            } else {
                Hydro::logWarn("EngineAPI: StaticLoadObject FAILED validation - clearing s_staticLoadObject and s_staticFindObject (both aliased to the same broken pointer).");
                s_staticLoadObject = nullptr;
                // s_staticFindObject is aliased to the same broken function
                // during discovery; without clearing it, loadObject's tier-3
                // fallback would call the wrong function and return garbage
                // pointers that look "valid" but aren't real UClasses.
                s_staticFindObject = nullptr;
            }
        } else {
            Hydro::logWarn("EngineAPI: StaticLoadObject returned null (not fatal)");
        }
        if (!s_processEvent) {
            Hydro::logError("EngineAPI: ProcessEvent not found");
            return false;
        }
    }

    if (!fromCache) {
    // Step 3: Find GUObjectArray (needed for finding UWorld instance)
    if (!findGUObjectArray()) {
        Hydro::logWarn("EngineAPI: GUObjectArray not found - will try alternate world discovery");
    }

    // Step 4: Find LoadPackage via "Attempted to LoadPackage" string
    {
        uint8_t* lpStr = findWideString(s_gm.base, s_gm.size,
            L"Attempted to LoadPackage from empty PackagePath");
        if (lpStr) {
            Hydro::logInfo("EngineAPI: 'Attempted to LoadPackage' at exe+0x%zX", (size_t)(lpStr - s_gm.base));
            // This string is in LoadPackageInternal. Find the LEA, walk to function start.
            uint8_t* leaAddr = findLeaRef(s_gm.base, s_gm.size, lpStr);
            if (leaAddr) {
                // Walk back to prologue
                for (int back = 1; back < 8192; back++) {
                    uint8_t* c = leaAddr - back;
                    if (c < s_gm.base) break;
                    if (c[-1] == 0xCC || c[-1] == 0xC3) {
                        uint8_t b0 = c[0], b1 = c[1];
                        bool prologue = (b0 == 0x48 && (b1 == 0x89 || b1 == 0x8B || b1 == 0x83)) ||
                                        (b0 == 0x40 && (b1 >= 0x53 && b1 <= 0x57)) ||
                                        (b0 == 0x41) || (b0 == 0x4C && b1 == 0x89) ||
                                        (b0 == 0x55) || (b0 == 0x53) || (b0 == 0x56);
                        if (prologue) {
                            s_loadPackage = c;
                            Hydro::logInfo("EngineAPI: LoadPackageInternal at exe+0x%zX (%02X %02X %02X %02X)",
                                (size_t)(c - s_gm.base), b0, b1, c[2], c[3]);
                            break;
                        }
                    }
                }
            }
        }
        if (!s_loadPackage)
            Hydro::logWarn("EngineAPI: LoadPackage not found");
    }

    // Step 5: Find the real StaticFindObject. The function reached from
    // "Failed to find object" only resolves /Script/ paths; the full-path
    // variant calls StaticFindObjectFast (which uniquely contains the
    // "Illegal call to StaticFindObjectFast" string), so we locate that and
    // pick its largest caller as StaticFindObject.
    // Skip when env HYDRO_SKIP_REAL_SFO=1: the validation loop probe-calls up
    // to 32 candidate functions with a wide-string arg. On UE 5.5 / DMG, one
    // candidate hangs the init thread (probe is structurally unsafe - we are
    // calling unknown functions with hopeful signatures). The /Script/-only
    // StaticFindObject from Step 4 is enough for reflection dumping; the
    // wider variant is only needed for cooked-asset path resolution. Set
    // HYDRO_SKIP_REAL_SFO=0 to force step 5 on UE 5.1 / Palworld where the
    // probe doesn't hang.
    bool skipRealSFO = false;
    if (char* envSkip = std::getenv("HYDRO_SKIP_REAL_SFO")) {
        skipRealSFO = (envSkip[0] == '1');
    } else {
        // Default: skip on UE 5.5+ (heuristic via game module size - DMG is
        // ~143 MB, Palworld ~600 MB; safer is to bias-skip and let UE 5.1
        // hosts opt back in via env).
        skipRealSFO = true;
    }
    if (skipRealSFO) {
        Hydro::logInfo("EngineAPI: Step 5 (real StaticFindObject probe) skipped - using Step 4 anchor for /Script/ lookups (set HYDRO_SKIP_REAL_SFO=0 to force)");
    }
    if (!skipRealSFO) {
        uint8_t* illegalStr = findWideString(s_gm.base, s_gm.size,
            L"Illegal call to StaticFindObjectFast");
        if (illegalStr) {
            Hydro::logInfo("EngineAPI: 'Illegal call to StaticFindObjectFast' at exe+0x%zX",
                (size_t)(illegalStr - s_gm.base));

            uint8_t* leaAddr = findLeaRef(s_gm.base, s_gm.size, illegalStr);
            if (leaAddr) {
                // Walk back to StaticFindObjectFast function start
                uint8_t* findObjFast = nullptr;
                for (int back = 1; back < 4096; back++) {
                    uint8_t* c = leaAddr - back;
                    if (c < s_gm.base) break;
                    if (c[-1] == 0xCC || c[-1] == 0xC3) {
                        uint8_t b0 = c[0], b1 = c[1];
                        bool prologue = (b0 == 0x48 && (b1 == 0x89 || b1 == 0x8B || b1 == 0x83)) ||
                                        (b0 == 0x40 && (b1 >= 0x53 && b1 <= 0x57)) ||
                                        (b0 == 0x41) || (b0 == 0x4C && b1 == 0x89) ||
                                        (b0 == 0x55) || (b0 == 0x53) || (b0 == 0x56);
                        if (prologue) { findObjFast = c; break; }
                    }
                }

                if (findObjFast) {
                    s_staticFindObjectFast = findObjFast;
                    Hydro::logInfo("EngineAPI: StaticFindObjectFast at exe+0x%zX",
                        (size_t)(findObjFast - s_gm.base));

                    // Collect every distinct moderate-sized caller of
                    // StaticFindObjectFast as a candidate. Picking by size
                    // alone is unreliable on forked engines (Palworld's
                    // 217 callers contain multiple ~800-byte functions; the
                    // largest happens to be a wrong one that always returns
                    // /Script/AssetRegistry). Instead we collect all viable
                    // candidates and validate each by behavior below.
                    int callerCount = 0;
                    struct Candidate { uint8_t* addr; size_t size; };
                    constexpr int MAX_CANDIDATES = 32;
                    Candidate candidates[MAX_CANDIDATES];
                    int numCandidates = 0;

                    for (size_t i = 0; i + 5 < s_gm.size && callerCount < 300; i++) {
                        if (s_gm.base[i] != 0xE8) continue;
                        int32_t rel = *(int32_t*)(s_gm.base + i + 1);
                        uint8_t* target = s_gm.base + i + 5 + rel;
                        if (target != findObjFast) continue;
                        callerCount++;

                        // Walk back to function start
                        uint8_t* funcStart = nullptr;
                        for (int back = 1; back < 4096; back++) {
                            uint8_t* c = s_gm.base + i - back;
                            if (c < s_gm.base) break;
                            if (c[-1] == 0xCC || c[-1] == 0xC3) {
                                uint8_t b0 = c[0], b1 = c[1];
                                bool prologue = (b0 == 0x48 && (b1 == 0x89 || b1 == 0x8B || b1 == 0x83)) ||
                                                (b0 == 0x40 && (b1 >= 0x53 && b1 <= 0x57)) ||
                                                (b0 == 0x41) || (b0 == 0x4C && b1 == 0x89) ||
                                                (b0 == 0x55) || (b0 == 0x53) || (b0 == 0x56);
                                if (prologue) { funcStart = c; break; }
                            }
                        }
                        if (!funcStart || funcStart == findObjFast) continue;

                        // De-dup: same function may have multiple call sites.
                        bool dup = false;
                        for (int k = 0; k < numCandidates; k++) {
                            if (candidates[k].addr == funcStart) { dup = true; break; }
                        }
                        if (dup) continue;

                        // Estimate size by walking to next CC CC pad.
                        size_t funcSize = 0;
                        for (size_t off = 0; off < 8192; off++) {
                            if (funcStart[off] == 0xCC && funcStart[off+1] == 0xCC) { funcSize = off; break; }
                        }
                        // StaticFindObject is roughly 100-2000 bytes - widen
                        // the band so we don't miss the real one on forked builds.
                        if (funcSize <= 100 || funcSize >= 2000) continue;

                        if (numCandidates < MAX_CANDIDATES)
                            candidates[numCandidates++] = {funcStart, funcSize};
                    }

                    Hydro::logInfo("EngineAPI: %d callers of StaticFindObjectFast -> %d unique size-range candidates",
                        callerCount, numCandidates);

                    // Sort candidates by size descending - try the largest first
                    // since real StaticFindObject is usually toward the upper end
                    // of the band (it does ResolveName + several lookups).
                    for (int a = 0; a < numCandidates; a++) {
                        for (int b = a + 1; b < numCandidates; b++) {
                            if (candidates[b].size > candidates[a].size) {
                                Candidate t = candidates[a];
                                candidates[a] = candidates[b];
                                candidates[b] = t;
                            }
                        }
                    }

                    // Behavioral validation: call each candidate with
                    // L"/Script/CoreUObject.Object" - only the real
                    // StaticFindObject returns a UObject named "Object".
                    // Validation lives in a helper function so MSVC's C2712
                    // (no __try in functions with C++ unwinding) is avoided.
                    for (int k = 0; k < numCandidates; k++) {
                        uint8_t* fn = candidates[k].addr;
                        // Capture both probe results for diagnostics
                        void* a = safeCallFindObject(fn, L"/Script/CoreUObject.Object");
                        void* b = safeCallFindObject(fn, L"/Script/Engine.Actor");
                        bool ok = validateStaticFindObjectCandidate(fn, nullptr);
                        Hydro::logInfo("EngineAPI:   cand[%d] exe+0x%zX (size %zu) Object=%p Actor=%p -> %s",
                            k, (size_t)(fn - s_gm.base), candidates[k].size, a, b,
                            ok ? "MATCH" : "no match");
                        if (ok) {
                            s_realStaticFindObject = fn;
                            Hydro::logInfo("EngineAPI: Real StaticFindObject at exe+0x%zX (size ~%zu) - validated",
                                (size_t)(fn - s_gm.base), candidates[k].size);
                            break;
                        }
                    }
                }
            }
        }
        if (!s_realStaticFindObject)
            Hydro::logWarn("EngineAPI: Real StaticFindObject not found");

        // SLO scan moved out of !fromCache so it also runs after a cache load
        // when validation cleared s_staticLoadObject. See the block past
        // `} // end !fromCache (pattern scans)`.
        if (false) {
            uint8_t* sfo = (uint8_t*)s_realStaticFindObject;
            int callerCount = 0;
            struct SLOCand { uint8_t* addr; size_t size; };
            constexpr int MAX_SLO = 32;
            SLOCand candidates[MAX_SLO];
            int numCands = 0;

            for (size_t i = 0; i + 5 < s_gm.size && callerCount < 500; i++) {
                if (s_gm.base[i] != 0xE8) continue;
                int32_t rel = *(int32_t*)(s_gm.base + i + 1);
                uint8_t* target = s_gm.base + i + 5 + rel;
                if (target != sfo) continue;
                callerCount++;

                // Walk back to function prologue.
                uint8_t* funcStart = nullptr;
                for (int back = 1; back < 4096; back++) {
                    uint8_t* c = s_gm.base + i - back;
                    if (c < s_gm.base) break;
                    if (c[-1] == 0xCC || c[-1] == 0xC3) {
                        uint8_t b0 = c[0], b1 = c[1];
                        bool prologue = (b0 == 0x48 && (b1 == 0x89 || b1 == 0x8B || b1 == 0x83)) ||
                                        (b0 == 0x40 && (b1 >= 0x53 && b1 <= 0x57)) ||
                                        (b0 == 0x41) || (b0 == 0x4C && b1 == 0x89) ||
                                        (b0 == 0x55) || (b0 == 0x53) || (b0 == 0x56);
                        if (prologue) { funcStart = c; break; }
                    }
                }
                if (!funcStart || funcStart == sfo) continue;

                bool dup = false;
                for (int k = 0; k < numCands; k++) {
                    if (candidates[k].addr == funcStart) { dup = true; break; }
                }
                if (dup) continue;

                size_t funcSize = 0;
                for (size_t off = 0; off < 8192; off++) {
                    if (funcStart[off] == 0xCC && funcStart[off+1] == 0xCC) { funcSize = off; break; }
                }
                if (funcSize <= 100 || funcSize >= 4000) continue;

                if (numCands < MAX_SLO)
                    candidates[numCands++] = { funcStart, funcSize };
            }

            Hydro::logInfo("EngineAPI: SLO scan: %d callers of StaticFindObject -> %d candidates",
                           callerCount, numCands);

            // Sort by size descending - real SLO is usually larger than its
            // siblings (it does load-flags handling + LoadPackage + cache work).
            for (int a = 0; a < numCands; a++) {
                for (int b = a + 1; b < numCands; b++) {
                    if (candidates[b].size > candidates[a].size) {
                        SLOCand t = candidates[a];
                        candidates[a] = candidates[b];
                        candidates[b] = t;
                    }
                }
            }

            for (int k = 0; k < numCands; k++) {
                uint8_t* fn = candidates[k].addr;
                bool ok = validateStaticLoadObjectCandidate(fn);
                Hydro::logInfo("EngineAPI:   SLO cand[%d] exe+0x%zX (size %zu) -> %s",
                               k, (size_t)(fn - s_gm.base), candidates[k].size,
                               ok ? "MATCH" : "no match");
                if (ok) {
                    s_staticLoadObject = fn;
                    s_staticFindObject = s_staticLoadObject;
                    Hydro::logInfo("EngineAPI: Real StaticLoadObject at exe+0x%zX (size ~%zu) - validated",
                                   (size_t)(fn - s_gm.base), candidates[k].size);
                    break;
                }
            }
        }
    }

    // Step 6: Find FName constructor (needed for AssetRegistry calls)
    if (!findFNameConstructor()) {
        Hydro::logWarn("EngineAPI: FName constructor not found - asset loading disabled");
    }

    // Save scan cache for next launch
    saveScanCache(s_gm);
    } // end !fromCache (pattern scans)

    // -- Recover real StaticLoadObject by validating callers of the
    //    now-validated StaticFindObject. Runs in BOTH cache-load and
    //    full-scan paths - when cache loads a stale s_staticLoadObject
    //    that fails behavioral validation, this rescans without needing
    //    to nuke the entire cache.
    if (s_realStaticFindObject && !s_staticLoadObject) {
        uint8_t* sfo = (uint8_t*)s_realStaticFindObject;
        int callerCount = 0;
        struct SLOCand2 { uint8_t* addr; size_t size; };
        constexpr int MAX_SLO2 = 32;
        SLOCand2 candidates2[MAX_SLO2];
        int numCands = 0;

        for (size_t i = 0; i + 5 < s_gm.size && callerCount < 500; i++) {
            if (s_gm.base[i] != 0xE8) continue;
            int32_t rel = *(int32_t*)(s_gm.base + i + 1);
            uint8_t* target = s_gm.base + i + 5 + rel;
            if (target != sfo) continue;
            callerCount++;

            uint8_t* funcStart = nullptr;
            for (int back = 1; back < 4096; back++) {
                uint8_t* c = s_gm.base + i - back;
                if (c < s_gm.base) break;
                if (c[-1] == 0xCC || c[-1] == 0xC3) {
                    uint8_t b0 = c[0], b1 = c[1];
                    bool prologue = (b0 == 0x48 && (b1 == 0x89 || b1 == 0x8B || b1 == 0x83)) ||
                                    (b0 == 0x40 && (b1 >= 0x53 && b1 <= 0x57)) ||
                                    (b0 == 0x41) || (b0 == 0x4C && b1 == 0x89) ||
                                    (b0 == 0x55) || (b0 == 0x53) || (b0 == 0x56);
                    if (prologue) { funcStart = c; break; }
                }
            }
            if (!funcStart || funcStart == sfo) continue;
            bool dup = false;
            for (int k = 0; k < numCands; k++) {
                if (candidates2[k].addr == funcStart) { dup = true; break; }
            }
            if (dup) continue;
            size_t funcSize = 0;
            for (size_t off = 0; off < 8192; off++) {
                if (funcStart[off] == 0xCC && funcStart[off+1] == 0xCC) { funcSize = off; break; }
            }
            if (funcSize <= 100 || funcSize >= 4000) continue;
            if (numCands < MAX_SLO2)
                candidates2[numCands++] = { funcStart, funcSize };
        }

        Hydro::logInfo("EngineAPI: SLO scan: %d callers of StaticFindObject -> %d candidates",
                       callerCount, numCands);

        for (int a = 0; a < numCands; a++) {
            for (int b = a + 1; b < numCands; b++) {
                if (candidates2[b].size > candidates2[a].size) {
                    SLOCand2 t = candidates2[a]; candidates2[a] = candidates2[b]; candidates2[b] = t;
                }
            }
        }

        for (int k = 0; k < numCands; k++) {
            uint8_t* fn = candidates2[k].addr;
            bool ok = validateStaticLoadObjectCandidate(fn);
            Hydro::logInfo("EngineAPI:   SLO cand[%d] exe+0x%zX (size %zu) -> %s",
                           k, (size_t)(fn - s_gm.base), candidates2[k].size,
                           ok ? "MATCH" : "no match");
            if (ok) {
                s_staticLoadObject = fn;
                s_staticFindObject = s_staticLoadObject;
                Hydro::logInfo("EngineAPI: Real StaticLoadObject at exe+0x%zX - validated",
                               (size_t)(fn - s_gm.base));
                saveScanCache(s_gm);  // persist for next launch
                break;
            }
        }
    }

    // Runtime discovery (always runs - heap objects change each launch)

    // GUObjectArray: retry if not found (may have been cached as null)
    if (!s_guObjectArray) {
        if (findGUObjectArray()) {
            // Update cache with the newly found address
            saveScanCache(s_gm);
        } else {
            Hydro::logWarn("EngineAPI: GUObjectArray not found - findClass/findAll/assets unavailable");
        }
    }

    // Layer 2 - probe FProperty/FField/UStruct internal offsets BEFORE any
    // discovery code that walks property chains (AssetRegistry, engine
    // objects). Otherwise those discovery paths read with stock-UE-5.5
    // fallback offsets and cache garbage UFunction layouts that won't be
    // refreshed later. Layer 2 only needs s_guObjectArray + s_fnameConstructor,
    // which are up by this point.
    if (s_guObjectArray && s_fnameConstructor && !s_layout.succeeded) {
        if (discoverPropertyLayout()) {
            // Discard any UFunction layouts cached by earlier (broken) walks
            // so subsequent getUFunctionParmsSize/RetOffset re-derive with
            // the real Layer-2 offsets.
            clearUFuncLayoutCache();
            saveScanCache(s_gm);
        }
    }

    // FName resolution MUST come before AssetRegistry/engine-object discovery.
    // Both discovery paths call findObject -> getObjectName -> FNamePool reads;
    // if FNamePool isn't discovered yet, getObjectName returns "<FName:N>"
    // placeholders and string-based scans miss everything. Previous order
    // bug: FNamePool was at the end of init, so a fresh init after SLO-crash-
    // clear ran AR/engine-object discovery against null FNamePool and silently
    // found nothing, leaving s_assetRegHelpersCDO/s_world etc. permanently null.
    if (!s_poolReady) {
        if (discoverFNamePool()) {
            saveScanCache(s_gm);
        }
    }
    if (!s_poolReady && s_processEvent) {
        if (!discoverConvNameToString()) {
            Hydro::logWarn("EngineAPI: FName resolver not available - Hydro.Reflect names will show as indices");
        }
    }

    // Discover AssetRegistry objects (now safe - FNamePool reads work)
    if (s_fnameConstructor) {
        if (!discoverAssetRegistry()) {
            Hydro::logWarn("EngineAPI: AssetRegistry not fully initialized");
        }
    }

    // Discover engine objects (UWorld, GameplayStatics, spawn functions)
    if (!discoverEngineObjects()) {
        Hydro::logWarn("EngineAPI: Some engine objects not found - spawning may fail");
    }

    // FShaderCodeLibrary::OpenLibrary - needed for runtime-mounted mod paks
    // to actually render their materials. Without it, paks mount but their
    // shader archives stay invisible to the renderer -> materials fall back
    // to defaults.
    if (!s_openShaderLibrary) {
        if (findOpenShaderLibrary()) {
            saveScanCache(s_gm);
        } else {
            Hydro::logWarn("EngineAPI: OpenShaderLibrary not found - mod materials may render as defaults");
        }
    }

    // Layer 2: probe FProperty internal offsets against AActor anchors.
    // Only meaningful once findObject + FName ctor are up. Lazy-init also
    // covers later first-use paths, but triggering here lets us persist
    // the discovered layout into the scan cache.
    if (s_realStaticFindObject && s_fnameConstructor && !s_layout.succeeded) {
        if (discoverPropertyLayout()) saveScanCache(s_gm);
    }

    s_ready = (s_processEvent != nullptr);
    Hydro::logInfo("EngineAPI: Bootstrap %s", s_ready ? "COMPLETE" : "PARTIAL");

    // Helper: Descriptor literals here would trip MSVC C2712 (this fn has __try).
    if (s_ready) seedAndResolveRawFunctions();

    // -- FNamePool sanity diagnostic --
    // Hypothesis (confirmed by parallel research 2026-04-28): on forked
    // engines like Palworld, Strategy-2 data-section scan can latch onto a
    // structurally-valid-but-functionally-wrong FNamePool - a debug stub or
    // freshly-initialized object whose Block[0][0] happens to look like a
    // "None" entry but whose Blocks[1..N] are null/garbage. Symptom: low
    // FName indices (in Block[0]) decode correctly; higher indices return
    // empty strings, so most findObject lookups silently miss.
    // This dump captures the evidence to prove or refute the hypothesis.
    // Fires once at bootstrap; cheap; reads only the pool struct header
    // and a few sentinel block pointers.
    if (s_ready && s_fnamePool && s_poolBlocksOffset >= 0) {
        Hydro::logInfo("EngineAPI: FNamePool sanity diagnostic --");
        Hydro::logInfo("  s_fnamePool=%p  s_poolBlocksOffset=0x%X",
            s_fnamePool, s_poolBlocksOffset);
        // FNameEntryAllocator layout: FRWLock(8) + uint32 CurrentBlock(4)
        // + uint32 CurrentByteCursor(4) = 0x10, then Blocks[].
        uint32_t currentBlock = 0;
        uint32_t cursor = 0;
        safeReadInt32((uint8_t*)s_fnamePool + 8, (int32_t*)&currentBlock);
        safeReadInt32((uint8_t*)s_fnamePool + 12, (int32_t*)&cursor);
        Hydro::logInfo("  CurrentBlock=%u  CurrentByteCursor=%u  (real engine pool: dozens; stub: 0-1)",
            currentBlock, cursor);

        // Walk Blocks[0..min(currentBlock+1, 16)] and dump pointer + first
        // 8 bytes of each. Real pool: each block is a heap pointer (~64KB
        // away from prior). Stub: only Blocks[0] is set, rest null.
        int blocksToDump = (int)currentBlock + 1;
        if (blocksToDump > 16) blocksToDump = 16;
        if (blocksToDump < 4)  blocksToDump = 4;  // always probe at least 4 to expose stubs
        for (int b = 0; b < blocksToDump; b++) {
            void* blockPtr = nullptr;
            bool readOk = safeReadPtr(
                (uint8_t*)s_fnamePool + s_poolBlocksOffset + b * 8,
                &blockPtr);
            if (!readOk) {
                Hydro::logInfo("  Blocks[%d]: read FAILED", b);
                continue;
            }
            if (!blockPtr) {
                Hydro::logInfo("  Blocks[%d]: NULL", b);
                continue;
            }
            uint8_t firstBytes[8] = {};
            for (int k = 0; k < 8; k++) {
                int32_t v = 0;
                if (safeReadInt32((uint8_t*)blockPtr + k, &v))
                    firstBytes[k] = (uint8_t)v;
            }
            Hydro::logInfo("  Blocks[%d]=%p  first8: %02X %02X %02X %02X %02X %02X %02X %02X",
                b, blockPtr,
                firstBytes[0], firstBytes[1], firstBytes[2], firstBytes[3],
                firstBytes[4], firstBytes[5], firstBytes[6], firstBytes[7]);
        }

        // Probe a few specific FName indices via the real reader.
        // Index 0 = always "None". Index 0x10000 = first entry in Block[1].
        auto probeIdx = [](uint32_t idx, const char* label) {
            char buf[256] = {};
            bool wide = false;
            int len = readPoolEntryRaw(idx, buf, sizeof(buf) - 1, &wide);
            if (len > 0) {
                buf[len < (int)sizeof(buf) - 1 ? len : (int)sizeof(buf) - 1] = 0;
                Hydro::logInfo("  readFromPool(idx=0x%X, %s): len=%d wide=%d str='%s'",
                    idx, label, len, wide ? 1 : 0, buf);
            } else {
                Hydro::logInfo("  readFromPool(idx=0x%X, %s): FAILED (len=0)",
                    idx, label);
            }
        };
        probeIdx(0,        "Block[0] entry 0 - must be 'None'");
        probeIdx(2,        "Block[0] entry 1 - small early FName");
        probeIdx(0x10000,  "Block[1] entry 0 - DECOY TEST: garbage means stub pool");
        probeIdx(0x20000,  "Block[2] entry 0 - sanity, should be valid on real pool");
        Hydro::logInfo("EngineAPI: FNamePool sanity diagnostic done --");
    }

    return s_ready;
}

bool isReady() { return s_ready; }

// Public API: loadObject

// FName constructor discovery

// FName constructor: void __fastcall FName::FName(FName* this, const TCHAR* Name, int32 FindType)
// Where FindType: 0 = FNAME_Find, 1 = FNAME_Add
using FNameCtorFn = void(__fastcall*)(void* outFName, const wchar_t* name, int32_t findType);

struct FName8 { uint32_t comparisonIndex; uint32_t number; };

static bool safeConstructFName(FName8* out, const wchar_t* name, void* ctorOverride = nullptr) {
    void* ctor = ctorOverride ? ctorOverride : s_fnameConstructor;
    if (!ctor || !out || !name) return false;
#ifdef _WIN32
    __try {
        auto fn = (FNameCtorFn)ctor;
        fn(out, name, 1); // FNAME_Add = 1
        return true;
    } __except(1) { return false; }
#else
    auto fn = (FNameCtorFn)ctor;
    fn(out, name, 1);
    return true;
#endif
}

// Wildcard pattern matcher: '?' matches any byte
static bool matchPattern(const uint8_t* data, const char* pattern, size_t* outLen) {
    size_t i = 0, di = 0;
    while (pattern[i]) {
        while (pattern[i] == ' ') i++;
        if (!pattern[i]) break;
        if (pattern[i] == '?') {
            i++; if (pattern[i] == '?') i++;
            di++;
        } else {
            uint8_t b = (uint8_t)strtol(pattern + i, nullptr, 16);
            if (data[di] != b) return false;
            i += 2; di++;
        }
    }
    if (outLen) *outLen = di;
    return true;
}

// Resolve RIP-relative CALL at offset: target = addr + 4 + *(int32_t*)addr
static uint8_t* resolveRip4(uint8_t* addr) {
    int32_t rel = *(int32_t*)addr;
    return addr + 4 + rel;
}

static bool findFNameConstructor() {
    // Instruction-pattern search modeled on patternsleuth's fname.rs.
    // Tier 1: direct byte pattern whose trailing E8 targets the constructor.
    Hydro::logInfo("EngineAPI: Searching for FName constructor (Tier 1: direct pattern)...");
    {
        const char* pat = "EB 07 48 8D 15 ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 41 B8 01 00 00 00 E8";
        size_t patLen = 0;
        // Dry run to get pattern length
        matchPattern(s_gm.base, pat, &patLen);

        for (size_t i = 0; i + patLen + 4 < s_gm.size; i++) {
            if (!matchPattern(s_gm.base + i, pat, nullptr)) continue;
            // E8 is at offset patLen-1, call operand at patLen
            uint8_t* callAddr = s_gm.base + i + patLen;
            uint8_t* target = resolveRip4(callAddr);
            if (target >= s_gm.base && target < s_gm.base + s_gm.size) {
                s_fnameConstructor = target;
                Hydro::logInfo("EngineAPI: FName constructor at exe+0x%zX (Tier 1 pattern at exe+0x%zX)",
                    (size_t)(target - s_gm.base), i);
                return true;
            }
        }
    }

    // Tier 2: String xref patterns (from patternsleuth fname.rs lines 147-163)
    // Find known strings, then match instruction patterns around the LEA
    Hydro::logInfo("EngineAPI: Searching for FName constructor (Tier 2: string xref)...");
    const wchar_t* anchorStrings[] = {
        L"MovementComponent0",
        L"TGPUSkinVertexFactoryUnlimited",
    };

    for (auto* anchorStr : anchorStrings) {
        uint8_t* strAddr = findWideString(s_gm.base, s_gm.size, anchorStr);
        if (!strAddr) continue;

        Hydro::logInfo("EngineAPI: Found '%ls' at exe+0x%zX", anchorStr, (size_t)(strAddr - s_gm.base));

        // Find LEA rdx, [rip+strAddr] (48 8D 15 xx xx xx xx)
        for (size_t i = 0; i + 20 < s_gm.size; i++) {
            uint8_t* p = s_gm.base + i;
            if (p[0] != 0x48 || p[1] != 0x8D || p[2] != 0x15) continue;
            int32_t disp = *(int32_t*)(p + 3);
            if (p + 7 + disp != strAddr) continue;

            // Found LEA rdx to our string at exe+i. Check instruction patterns after it:

            // Pattern A: 48 8D 15 [str] 48 8D 0D ?? ?? ?? ?? E8 [FName ctor]
            // (lea rdx,[str]; lea rcx,[fname]; call ctor)
            if (p[7] == 0x48 && p[8] == 0x8D && p[9] == 0x0D) {
                if (p[14] == 0xE8) {
                    uint8_t* target = resolveRip4(p + 15);
                    if (target >= s_gm.base && target < s_gm.base + s_gm.size) {
                        FName8 validate = {0xFFFF, 0xFFFF};
                        if (safeConstructFName(&validate, L"None", target) &&
                            validate.comparisonIndex == 0 && validate.number == 0) {
                            s_fnameConstructor = target;
                            Hydro::logInfo("EngineAPI: FName constructor at exe+0x%zX (Tier 2A via '%ls', validated)",
                                (size_t)(target - s_gm.base), anchorStr);
                            return true;
                        }
                    }
                }
            }

            // Pattern B: 48 8D 15 [str] 4C 8D 05 ?? ?? ?? ?? 41 B1 01 E8 [wrapper->ctor]
            // (lea rdx,[str]; lea r8,[?]; mov r9b,1; call wrapper)
            if (p[7] == 0x4C && p[8] == 0x8D && p[9] == 0x05) {
                if (p[14] == 0x41 && p[15] == 0xB1 && p[16] == 0x01 && p[17] == 0xE8) {
                    uint8_t* target = resolveRip4(p + 18);
                    if (target >= s_gm.base && target < s_gm.base + s_gm.size) {
                        // This is a wrapper - find first CALL inside it
                        for (int off = 0; off < 64; off++) {
                            if (target[off] == 0xE8) {
                                uint8_t* innerTarget = resolveRip4(target + off + 1);
                                if (innerTarget >= s_gm.base && innerTarget < s_gm.base + s_gm.size) {
                                    FName8 validate = {0xFFFF, 0xFFFF};
                                    if (safeConstructFName(&validate, L"None", innerTarget) &&
                                        validate.comparisonIndex == 0 && validate.number == 0) {
                                        s_fnameConstructor = innerTarget;
                                        Hydro::logInfo("EngineAPI: FName constructor at exe+0x%zX (Tier 2B via '%ls', validated)",
                                            (size_t)(innerTarget - s_gm.base), anchorStr);
                                        return true;
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // Pattern C: 41 B8 01 00 00 00 48 8D 15 [str] 48 8D 0D ?? ?? ?? ?? E9 [jmp to ctor]
            // Check bytes before the LEA
            if (i >= 6 && p[-6] == 0x41 && p[-5] == 0xB8 && p[-4] == 0x01 &&
                p[-3] == 0x00 && p[-2] == 0x00 && p[-1] == 0x00) {
                if (p[7] == 0x48 && p[8] == 0x8D && p[9] == 0x0D && p[14] == 0xE9) {
                    uint8_t* target = resolveRip4(p + 15);
                    if (target >= s_gm.base && target < s_gm.base + s_gm.size) {
                        // Validate: construct FName("None") - must be index 0
                        FName8 validate = {0xFFFF, 0xFFFF};
                        if (safeConstructFName(&validate, L"None", target) &&
                            validate.comparisonIndex == 0 && validate.number == 0) {
                            s_fnameConstructor = target;
                            Hydro::logInfo("EngineAPI: FName constructor at exe+0x%zX (Tier 2C via '%ls', validated)",
                                (size_t)(target - s_gm.base), anchorStr);
                            return true;
                        }
                    }
                }
            }
        }
    }

    Hydro::logWarn("EngineAPI: FName constructor not found");
    return false;
}

// UClass function chain walker
// Walks UClass::Children (UField linked list) to find a UFunction by name.
// This is how UE4SS resolves functions (GetFunctionByNameInChain).
// Returns the UFunction with the correct native Func pointer.

static void* findFunctionOnClass(void* uclass, const wchar_t* funcName) {
    if (!uclass || !funcName || !s_fnameConstructor) return nullptr;

    // Construct FName for the function name
    FName8 targetName = {};
    if (!safeConstructFName(&targetName, funcName)) return nullptr;

    // Walk UClass::Children (UField* linked list at offset 0x48), then up
    // the SuperStruct chain. Inherited functions live on ancestor UClasses -
    // e.g. ACharacter inherits K2_GetActorLocation from AActor, so looking
    // only at ACharacter's Children finds nothing.
    void* current = uclass;
    int classDepth = 0;
    while (current && classDepth < 32) {
        void* child = nullptr;
        safeReadPtr((uint8_t*)current + USTRUCT_CHILDREN, &child);

        int count = 0;
        while (child && count < 500) {
            uint32_t nameIdx = 0;
            safeReadInt32((uint8_t*)child + UOBJ_NAME, (int32_t*)&nameIdx);

            if (nameIdx == targetName.comparisonIndex) {
                return child;
            }

            void* next = nullptr;
            safeReadPtr((uint8_t*)child + UFIELD_NEXT, &next);
            child = next;
            count++;
        }

        // Climb to the super class via UStruct::SuperStruct.
        void* super = nullptr;
        safeReadPtr((uint8_t*)current + (s_superOffset < 0 ? 0x40 : s_superOffset), &super);
        if (super == current) break;  // defensive: no self-loop
        current = super;
        classDepth++;
    }

    return nullptr;
}

// AssetRegistry loading

static bool discoverAssetRegistry() {
    if (!s_fnameConstructor) return false;

    // StaticFindObject: UObject*(UClass*, UObject*, const TCHAR*, bool)
    using FindObjFn = void*(__fastcall*)(void*, void*, const wchar_t*, bool);

    auto find = [](const wchar_t* path) -> void* {
        // Primary: GUObjectArray-walk findObject. Validated against forked
        // engines, doesn't depend on pattern-scanned C++ load primitives.
        if (void* r = findObject(path)) return r;
        // Fallback 1: discovered StaticFindObject (only if non-null - it gets
        // cleared on hosts where behavioral validation rejects it).
        if (s_realStaticFindObject) {
            auto fn = (FindObjFn)s_realStaticFindObject;
            void* result = nullptr;
#ifdef _WIN32
            __try { result = fn(nullptr, nullptr, path, false); }
            __except(1) { result = nullptr; }
#else
            result = fn(nullptr, nullptr, path, false);
#endif
            if (result) return result;
        }
        // Fallback 2: legacy alias (also nulled on hosts that fail validation).
        if (s_staticFindObject)
            return safeCallLoadObject(s_staticFindObject, path, 0);
        return nullptr;
    };

    // Find AssetRegistryHelpers CDO
    s_assetRegHelpersCDO = find(L"/Script/AssetRegistry.Default__AssetRegistryHelpers");
    if (s_assetRegHelpersCDO)
        Hydro::logInfo("EngineAPI: AssetRegistryHelpers CDO at %p", s_assetRegHelpersCDO);

    // Find GetAsset UFunction via the UClass function chain (same as
    // UE4SS's GetFunctionByNameInChain). This gives us the UFunction
    // with the intact native Func pointer.
    if (s_assetRegHelpersCDO) {
        void* cdoClass = nullptr;
        safeReadPtr((uint8_t*)s_assetRegHelpersCDO + UOBJ_CLASS, &cdoClass);
        if (cdoClass) {
            void* oldGetAsset = find(L"/Script/AssetRegistry.AssetRegistryHelpers:GetAsset");
            s_getAssetFunc = findFunctionOnClass(cdoClass, L"GetAsset");
            if (s_getAssetFunc) {
                uint16_t ps = getUFunctionParmsSize(s_getAssetFunc);
                uint16_t ro = getUFunctionRetOffset(s_getAssetFunc);
                void* funcPtr = nullptr;
                safeReadPtr((uint8_t*)s_getAssetFunc + UFUNC_FUNC, &funcPtr);
                Hydro::logInfo("EngineAPI: GetAsset UFunction at %p (via UClass chain)", s_getAssetFunc);
                Hydro::logInfo("EngineAPI:   ParmsSize=%u, RetOffset=%u, Func=%p", ps, ro, funcPtr);
                if (oldGetAsset && oldGetAsset != s_getAssetFunc) {
                    Hydro::logInfo("EngineAPI:   *** DIFFERENT from StaticFindObject result %p! ***", oldGetAsset);
                }
            }
        }
    }
    // Fallback to StaticFindObject if class chain failed
    if (!s_getAssetFunc) {
        s_getAssetFunc = find(L"/Script/AssetRegistry.AssetRegistryHelpers:GetAsset");
        if (s_getAssetFunc)
            Hydro::logInfo("EngineAPI: GetAsset UFunction at %p (fallback StaticFindObject)", s_getAssetFunc);
    }

    // Find AssetRegistry implementation
    s_assetRegImpl = find(L"/Script/AssetRegistry.Default__AssetRegistryImpl");
    if (!s_assetRegImpl)
        s_assetRegImpl = find(L"/Script/AssetRegistry.Default__AssetRegistry");
    if (s_assetRegImpl)
        Hydro::logInfo("EngineAPI: AssetRegistry impl at %p", s_assetRegImpl);

    // Find GetAssetByObjectPath UFunction - try multiple names
    const wchar_t* getByPathNames[] = {
        L"/Script/AssetRegistry.AssetRegistry:K2_GetAssetByObjectPath",
        L"/Script/AssetRegistry.AssetRegistryImpl:K2_GetAssetByObjectPath",
        L"/Script/AssetRegistry.AssetRegistry:GetAssetByObjectPath",
        L"/Script/AssetRegistry.AssetRegistryImpl:GetAssetByObjectPath",
    };
    for (auto* name : getByPathNames) {
        s_getByPathFunc = find(name);
        if (s_getByPathFunc) {
            std::wstring nw(name); std::string nn(nw.begin(), nw.end());
            Hydro::logInfo("EngineAPI: GetAssetByObjectPath at %p (%s)", s_getByPathFunc, nn.c_str());
            break;
        }
    }

    // Find GetAssetRegistry UFunction - needed to get the RUNTIME registry instance
    auto* getRegistryFunc = find(L"/Script/AssetRegistry.AssetRegistryHelpers:GetAssetRegistry");
    if (getRegistryFunc && s_assetRegHelpersCDO && s_processEvent) {
        Hydro::logInfo("EngineAPI: GetAssetRegistry UFunction at %p - calling to get runtime instance...", getRegistryFunc);

        // GetAssetRegistry returns FScriptInterface (UObject* + void* = 16 bytes)
        uint8_t regParams[32] = {};
        if (callProcessEvent(s_assetRegHelpersCDO, getRegistryFunc, regParams)) {
            void* runtimeRegistry = *(void**)(regParams + 0x00); // UObject* from FScriptInterface
            if (runtimeRegistry) {
                s_assetRegImpl = runtimeRegistry;
                Hydro::logInfo("EngineAPI: Runtime AssetRegistry at %p", s_assetRegImpl);
            }
        }
    }

    return s_assetRegHelpersCDO && s_getAssetFunc;
}

// Public API: findObject (in-memory only)

// StaticFindObject signature: UObject*(UClass*, UObject*, const TCHAR*, bool)
// Only 4 params - different from StaticLoadObject's 8
using StaticFindObjectFn = void*(__fastcall*)(void*, void*, const wchar_t*, bool);

// -- GUObjectArray-walk findObject ---
// Pattern-scanned StaticFindObject is fragile across UE forks: on Palworld
// our discovered function returns the wrong object for every path. The
// robust primitive is to walk GUObjectArray ourselves, matching by FName +
// outer chain. UE4SS uses the same approach for early-init lookups before
// offsets are validated (see deps/RE-UE4SS UObjectGlobals.cpp:135 -
// StaticFindObject_InternalNoToStringFromNames).
// Path syntax: split on '.' and ':' into ordered name parts, e.g.
//   /Script/Engine.Actor              -> ["/Script/Engine", "Actor"]
//   /Script/Pkg.Class:Member          -> ["/Script/Pkg", "Class", "Member"]
//   /Game/Mods/M/Sphere.Sphere_C      -> ["/Game/Mods/M/Sphere", "Sphere_C"]
// UPackages store their full slash-path as a single FName, which is why
// the leading slash stays bound to the first component.
// For each candidate object whose own FName matches the last part, walk
// its outer chain comparing each step's FName against the parts in
// reverse. Match = exact length and exact ordered name match.

static std::unordered_map<std::wstring, void*> s_findObjectCache;

static void* findObjectViaScan(const wchar_t* path) {
    if (!s_guObjectArray || !path || !s_fnameConstructor) return nullptr;

    // Split on '.' and ':' into ordered parts.
    std::wstring p(path);
    std::vector<std::wstring> parts;
    size_t start = 0;
    for (size_t i = 0; i <= p.size(); i++) {
        if (i == p.size() || p[i] == L'.' || p[i] == L':') {
            if (i > start) parts.emplace_back(p.substr(start, i - start));
            start = i + 1;
        }
    }
    if (parts.empty()) return nullptr;

    // Resolve each part to its FName ComparisonIndex (FNAME_Add - safe;
    // the engine canonicalizes case-insensitively).
    std::vector<uint32_t> partIdx;
    partIdx.reserve(parts.size());
    for (auto& s : parts) {
        FName8 fn = {};
        if (!safeConstructFName(&fn, s.c_str()) || !fn.comparisonIndex) return nullptr;
        partIdx.push_back(fn.comparisonIndex);
    }
    uint32_t targetNameIdx = partIdx.back();

    // Read GUObjectArray header.
    void** chunkTable = nullptr;
    int32_t count = 0;
    if (!safeReadPtr((uint8_t*)s_guObjectArray + FARRAY_OBJECTS, (void**)&chunkTable)) return nullptr;
    if (!safeReadInt32((uint8_t*)s_guObjectArray + FARRAY_NUM_ELEMS, &count)) return nullptr;
    if (!chunkTable || count <= 0) return nullptr;

    // First pass - strict match: obj.name == lastPart, full outer chain
    // matches the parts list in exact order with no intermediates allowed.
    // Catches typical /Script/ class lookups quickly.
    void* strictMatch = nullptr;
    void* fallbackMatch = nullptr;

    uint32_t firstPartIdx = partIdx.front();   // typically the package FName

    for (int32_t i = 0; i < count; i++) {
        int chunkIdx    = i / CHUNK_SIZE;
        int withinChunk = i % CHUNK_SIZE;
        void* chunk = nullptr;
        if (!safeReadPtr((uint8_t*)&chunkTable[chunkIdx], &chunk) || !chunk) continue;
        uint8_t* item = (uint8_t*)chunk + (withinChunk * FUOBJ_SIZE);
        void* obj = nullptr;
        if (!safeReadPtr(item + FUOBJ_OBJECT, &obj) || !obj) continue;

        // Early reject: own name FName must match the last path part.
        uint32_t selfIdx = 0;
        if (!safeReadInt32((uint8_t*)obj + UOBJ_NAME, (int32_t*)&selfIdx)) continue;
        if (selfIdx != targetNameIdx) continue;

        // Walk outer chain. Try strict first: every part must line up in
        // exact order with no leftover/missing levels.
        void* cur = obj;
        bool strict = true;
        for (int k = (int)partIdx.size() - 1; k >= 0; k--) {
            if (!cur) { strict = false; break; }
            uint32_t curName = 0;
            if (!safeReadInt32((uint8_t*)cur + UOBJ_NAME, (int32_t*)&curName)) { strict = false; break; }
            if (curName != partIdx[k]) { strict = false; break; }
            void* outer = nullptr;
            if (!safeReadPtr((uint8_t*)cur + UOBJ_OUTER, &outer)) { strict = false; break; }
            cur = outer;
        }
        if (strict && cur != nullptr) strict = false;
        if (strict) {
            strictMatch = obj;
            break;  // strict win - done
        }

        // Forgiving fallback (UE's StaticFindObject convention): obj.name
        // matches the last part AND obj's OUTERMOST package matches the
        // first part. Ignores intermediate UClass outers - handles CDO
        // queries like `/Script/AssetRegistry.Default__AssetRegistryHelpers`
        // where the actual outer chain is `Pkg -> UClass -> CDO` but UE
        // accepts the abbreviated `Pkg.CDO` query form.
        // Forgiving "Pkg.Leaf" form: walk to outermost, compare the package
        // name as a *string* (case-insensitive). FName ComparisonIndex parity
        // for package names isn't guaranteed across forked engines - we've
        // observed the same string yielding different indices via FNAME_Add
        // vs the actual stored index on the live UPackage. Decoded-string
        // compare is the resilient check at this level. Inner levels (which
        // tier-1 already covered) stay on the faster index path.
        if (!fallbackMatch && partIdx.size() >= 2) {
            void* outerCur = obj;
            void* outermost = obj;
            for (int hops = 0; hops < 32; hops++) {
                void* next = nullptr;
                if (!safeReadPtr((uint8_t*)outerCur + UOBJ_OUTER, &next) || !next) break;
                outermost = next;
                outerCur = next;
            }
            std::string outermostStr = getObjectName(outermost);
            std::string firstPartStr;
            firstPartStr.reserve(parts.front().size());
            for (wchar_t c : parts.front()) firstPartStr.push_back((char)(c & 0x7F));
            // Case-insensitive compare
            bool match = outermostStr.size() == firstPartStr.size();
            for (size_t k = 0; match && k < outermostStr.size(); k++) {
                unsigned char a = (unsigned char)outermostStr[k];
                unsigned char b = (unsigned char)firstPartStr[k];
                if (a >= 'A' && a <= 'Z') a += 32;
                if (b >= 'A' && b <= 'Z') b += 32;
                if (a != b) match = false;
            }
            if (match) {
                fallbackMatch = obj;
                // Don't break - keep scanning in case a strict match exists
                // later. Strict wins over fallback.
            }
        }
    }

    if (strictMatch) return strictMatch;
    if (fallbackMatch) return fallbackMatch;

    // -- Tier-3 diagnostics - fired only when tiers 1+2 missed.
    // Goal: capture *why* the FName-index path missed so we can fix the
    // actual root cause, not just lean on the string-compare fallback.
    // Strategy: for the LEAF FName (the rightmost path part), find every
    // candidate object whose *string* name matches (case-insensitive),
    // dump its outer chain with both FName-indices and resolved strings,
    // and explicitly highlight any mismatch between our computed
    // partIdx[k] and the candidate's actual stored FName index at chain
    // position k.
    // One-shot per unique path to avoid log spam.
    static std::unordered_map<std::wstring, bool> s_diagDumped;
    if (s_diagDumped.find(p) == s_diagDumped.end()) {
        s_diagDumped[p] = true;

        std::string narrowPath;
        for (wchar_t c : p) narrowPath.push_back((char)(c & 0x7F));
        std::string narrowLeaf;
        for (wchar_t c : parts.back()) narrowLeaf.push_back((char)(c & 0x7F));

        Hydro::logInfo("findObject DIAG: tiers 1+2 missed for '%s'", narrowPath.c_str());
        Hydro::logInfo("findObject DIAG: parts=%zu, our computed FName indices:", parts.size());
        for (size_t k = 0; k < parts.size(); k++) {
            std::string narrowPart;
            for (wchar_t c : parts[k]) narrowPart.push_back((char)(c & 0x7F));
            Hydro::logInfo("  [%zu] '%s' -> our FNAME_Add idx=%u",
                (size_t)k, narrowPart.c_str(), partIdx[k]);
        }

        auto iequalNarrow0 = [](const std::string& a, const std::string& b) {
            if (a.size() != b.size()) return false;
            for (size_t i = 0; i < a.size(); i++) {
                unsigned char ca = (unsigned char)a[i];
                unsigned char cb = (unsigned char)b[i];
                if (ca >= 'A' && ca <= 'Z') ca += 32;
                if (cb >= 'A' && cb <= 'Z') cb += 32;
                if (ca != cb) return false;
            }
            return true;
        };

        int diagFound = 0;
        for (int32_t i2 = 0; i2 < count && diagFound < 6; i2++) {
            int chunkIdx2    = i2 / CHUNK_SIZE;
            int withinChunk2 = i2 % CHUNK_SIZE;
            void* chunk2 = nullptr;
            if (!safeReadPtr((uint8_t*)&chunkTable[chunkIdx2], &chunk2) || !chunk2) continue;
            uint8_t* item2 = (uint8_t*)chunk2 + (withinChunk2 * FUOBJ_SIZE);
            void* obj2 = nullptr;
            if (!safeReadPtr(item2 + FUOBJ_OBJECT, &obj2) || !obj2) continue;

            std::string objLeaf2 = getObjectName(obj2);
            if (objLeaf2.empty()) continue;
            if (!iequalNarrow0(objLeaf2, narrowLeaf)) continue;

            diagFound++;
            uint32_t storedSelfIdx = 0;
            safeReadInt32((uint8_t*)obj2 + UOBJ_NAME, (int32_t*)&storedSelfIdx);
            std::string fullStr = getObjectPath(obj2);
            Hydro::logInfo("findObject DIAG: candidate #%d at %p", diagFound, obj2);
            Hydro::logInfo("  fullPath='%s'", fullStr.c_str());
            Hydro::logInfo("  self.name idx STORED=%u  vs OUR idx=%u  (%s)",
                storedSelfIdx, partIdx.back(),
                storedSelfIdx == partIdx.back() ? "MATCH" : "MISMATCH");

            // Walk outer chain
            void* cur2 = obj2;
            for (int hops = 0; hops < 8 && cur2; hops++) {
                uint32_t curIdx = 0;
                safeReadInt32((uint8_t*)cur2 + UOBJ_NAME, (int32_t*)&curIdx);
                std::string curName = getObjectName(cur2);
                Hydro::logInfo("    chain[%d] obj=%p name='%s' idx=%u",
                    hops, cur2, curName.c_str(), curIdx);
                void* next2 = nullptr;
                if (!safeReadPtr((uint8_t*)cur2 + UOBJ_OUTER, &next2) || !next2) break;
                cur2 = next2;
            }
        }
        if (diagFound == 0) {
            // Ground-truth dump: iterate the first 10 GUObjectArray entries
            // and print everything we read. If these come back as garbage
            // (wild pointers, empty names, classes with no name) it means
            // our GUObjectArray walk or FNamePool reads are broken -
            // fundamentally different bug than "object not loaded".
            // Fires once globally to avoid log spam.
            static bool s_groundTruthDumped = false;
            if (!s_groundTruthDumped) {
                s_groundTruthDumped = true;
                Hydro::logInfo("findObject DIAG: ground-truth dump of first 10 GUObjectArray entries:");
                Hydro::logInfo("  guObjectArray=%p  count=%d  CHUNK_SIZE=%d  FUOBJ_SIZE=%d",
                    s_guObjectArray, count, (int)CHUNK_SIZE, (int)FUOBJ_SIZE);
                for (int32_t gi = 0; gi < 10 && gi < count; gi++) {
                    int chunkIdx3    = gi / CHUNK_SIZE;
                    int withinChunk3 = gi % CHUNK_SIZE;
                    void* chunk3 = nullptr;
                    if (!safeReadPtr((uint8_t*)&chunkTable[chunkIdx3], &chunk3) || !chunk3) {
                        Hydro::logInfo("  [%d] chunk read failed", gi);
                        continue;
                    }
                    uint8_t* item3 = (uint8_t*)chunk3 + (withinChunk3 * FUOBJ_SIZE);
                    void* obj3 = nullptr;
                    if (!safeReadPtr(item3 + FUOBJ_OBJECT, &obj3) || !obj3) {
                        Hydro::logInfo("  [%d] obj=null", gi);
                        continue;
                    }
                    uint32_t selfIdx3 = 0;
                    safeReadInt32((uint8_t*)obj3 + UOBJ_NAME, (int32_t*)&selfIdx3);
                    std::string selfStr = getObjectName(obj3);
                    void* clsPtr = nullptr;
                    safeReadPtr((uint8_t*)obj3 + UOBJ_CLASS, &clsPtr);
                    std::string clsStr = clsPtr ? getObjectName(clsPtr) : std::string();
                    void* outerPtr = nullptr;
                    safeReadPtr((uint8_t*)obj3 + UOBJ_OUTER, &outerPtr);
                    std::string outerStr = outerPtr ? getObjectName(outerPtr) : std::string();
                    Hydro::logInfo("  [%d] obj=%p name='%s' idx=%u class=%p('%s') outer=%p('%s')",
                        gi, obj3, selfStr.c_str(), selfIdx3,
                        clsPtr, clsStr.c_str(), outerPtr, outerStr.c_str());
                }
            }
            Hydro::logInfo("findObject DIAG: NO objects found whose leaf name matches '%s' (case-insensitive). The class/object may not be loaded yet.",
                narrowLeaf.c_str());
        }
    }

    // Tier 3 - UE4SS-style full-path string compare.
    // Mirrors StaticFindObject_InternalSlow (UObjectGlobals.cpp:111): walk
    // every UObject whose self-name matches the leaf, build its full outer-
    // chain path string, case-insensitive compare against the input. This is
    // the resilient catch-all for forks where the CDO's outer chain shape
    // differs from stock UE (e.g. Palworld's UE 5.1.x AssetRegistry CDOs).
    auto iequalNarrow = [](const std::string& a, const std::string& b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); i++) {
            unsigned char ca = (unsigned char)a[i];
            unsigned char cb = (unsigned char)b[i];
            if (ca >= 'A' && ca <= 'Z') ca += 32;
            if (cb >= 'A' && cb <= 'Z') cb += 32;
            if (ca != cb) return false;
        }
        return true;
    };
    auto wideToNarrow = [](const std::wstring& w) {
        std::string out;
        out.reserve(w.size());
        for (wchar_t c : w) out.push_back((char)(c & 0x7F));
        return out;
    };
    std::string targetNarrow = wideToNarrow(p);
    std::string leafNarrow   = wideToNarrow(parts.back());

    // Prefilter on leaf NAME STRING (not FName index) to dodge potential
    // FName interning quirks on forked engines where FNAME_Add for our query
    // string may not return the same ComparisonIndex as the actual stored
    // object's name. Slower than index compare but handles every fork.
    for (int32_t i = 0; i < count; i++) {
        int chunkIdx    = i / CHUNK_SIZE;
        int withinChunk = i % CHUNK_SIZE;
        void* chunk = nullptr;
        if (!safeReadPtr((uint8_t*)&chunkTable[chunkIdx], &chunk) || !chunk) continue;
        uint8_t* item = (uint8_t*)chunk + (withinChunk * FUOBJ_SIZE);
        void* obj = nullptr;
        if (!safeReadPtr(item + FUOBJ_OBJECT, &obj) || !obj) continue;

        std::string objLeaf = getObjectName(obj);
        if (objLeaf.empty()) continue;
        if (!iequalNarrow(objLeaf, leafNarrow)) continue;

        std::string built = getObjectPath(obj);
        if (built.empty()) continue;
        if (iequalNarrow(built, targetNarrow)) return obj;
    }

    return nullptr;
}

void* findObject(const wchar_t* path) {
    if (!path) return nullptr;

    // Cache check. Validate cached entry with one read so a GC'd object
    // doesn't stick around (rare for /Script/ classes which are root-held,
    // but possible for /Game/ assets).
    std::wstring key(path);
    auto it = s_findObjectCache.find(key);
    if (it != s_findObjectCache.end() && it->second) {
        void* cls = nullptr;
        if (safeReadPtr((uint8_t*)it->second + UOBJ_CLASS, &cls) && cls) {
            return it->second;
        }
        s_findObjectCache.erase(it);
    }

    // Primary: GUObjectArray walk. Robust on every UE fork because it
    // depends only on UOBJ_NAME (0x18) / UOBJ_OUTER (0x20) / GUObjectArray
    // layout - all stable and validated at bootstrap.
    if (void* result = findObjectViaScan(path)) {
        s_findObjectCache[key] = result;
        return result;
    }

    // Fallback: pattern-scanned StaticFindObject. Only used on hosts where
    // it's been validated to actually return correct objects (otherwise
    // returns wrong things, e.g. AssetRegistry on Palworld). Uses the
    // SEH-isolated helper so it can coexist with C++ unwinding objects.
    if (s_realStaticFindObject) {
        if (void* result = safeCallFindObject(s_realStaticFindObject, path)) {
            s_findObjectCache[key] = result;
            return result;
        }
    }

    return nullptr;
}

// Public API: loadAsset (via AssetRegistry GetAsset)
// Uses the DOCUMENTED FAssetData contract: PackageName + AssetName uniquely
// identify an asset (operator==, GetTypeHash, IsValid all confirm this).
// FastGetAsset only uses these two fields - no other state needed.
// This is the same approach BPModLoaderMod uses in production.

void* loadAsset(const wchar_t* assetPath) {
    if (!assetPath || !s_fnameConstructor || !s_processEvent) return nullptr;
    if (!s_assetRegHelpersCDO || !s_getAssetFunc) {
        // One-shot deferred retry: AR CDOs aren't in GUObjectArray during
        // HydroCore bootstrap. Retry once here. This costs ~1s on a miss
        // (full GUObjectArray walk), but happens at most once per
        // loadAsset call - and after the FIRST success no further scans
        // run because everything is cached/discovered.
        discoverAssetRegistry();
        if (!s_assetRegHelpersCDO || !s_getAssetFunc) {
            Hydro::logError("EngineAPI: AssetRegistry not initialized");
            return nullptr;
        }
        Hydro::logInfo("EngineAPI: AssetRegistry deferred-init succeeded on loadAsset");
    }

    // Parse "PackageName.AssetName" format
    std::wstring fullPath(assetPath);
    std::wstring packageName = fullPath;
    std::wstring assetName;
    auto dotPos = fullPath.rfind(L'.');
    if (dotPos != std::wstring::npos) {
        packageName = fullPath.substr(0, dotPos);
        assetName = fullPath.substr(dotPos + 1);
    }

    // Construct FNames
    FName8 pkgFName = {}, assetFName = {};
    if (!safeConstructFName(&pkgFName, packageName.c_str())) {
        Hydro::logError("EngineAPI: Failed to construct FName for package");
        return nullptr;
    }
    if (!assetName.empty()) {
        safeConstructFName(&assetFName, assetName.c_str());
    }

    Hydro::logInfo("EngineAPI: Loading '%ls' (pkg={%u,%u} asset={%u,%u})",
        assetPath, pkgFName.comparisonIndex, pkgFName.number,
        assetFName.comparisonIndex, assetFName.number);

    // Read GetAsset's exact param layout from the UFunction (Layer 3 derived).
    uint16_t gaParmsSize = getUFunctionParmsSize(s_getAssetFunc);
    uint16_t gaRetOffset = getUFunctionRetOffset(s_getAssetFunc);

    // Read runtime field offsets from the FAssetData UScriptStruct rather
    // than hardcoding them - compiler layout can shift between builds.
    int32_t pkgNameOffset = -1, assetNameOffset = -1;
    {
        void* firstProp = nullptr;
        safeReadPtr((uint8_t*)s_getAssetFunc + UFUNC_CHILD_PROPS, &firstProp);
        if (firstProp) {
            // The UFunction's own property chain holds parameter entries, not
            // the struct's members, so resolve FAssetData directly.
            void* assetDataStruct = safeCallLoadObject(s_staticFindObject, L"/Script/CoreUObject.AssetData", 0);
            if (!assetDataStruct)
                assetDataStruct = safeCallLoadObject(s_staticFindObject, L"/Script/CoreUObject.ScriptStruct'/Script/CoreUObject.AssetData'", 0);

            if (assetDataStruct) {
                Hydro::logInfo("EngineAPI: FAssetData UScriptStruct at %p", assetDataStruct);

                // Walk the struct's property chain via accessors so it picks
                // up Layer 2's discovered offsets transparently.
                void* prop = getChildProperties(assetDataStruct);
                int fieldIdx = 0;
                while (prop && fieldIdx < 15) {
                    int32_t offset  = getPropertyOffset(prop);
                    int32_t size    = getPropertyElementSize(prop);
                    uint32_t nameIdx = 0;
                    int nmOff = (s_layout.fieldNameOffset >= 0) ? s_layout.fieldNameOffset : FFIELD_NAME;
                    safeReadInt32((uint8_t*)prop + nmOff, (int32_t*)&nameIdx);

                    Hydro::logInfo("EngineAPI:   Field[%d]: '%s' offset=0x%X size=%d",
                        fieldIdx, getNameString(nameIdx).c_str(), offset, size);

                    fieldIdx++;
                    prop = getNextProperty(prop);
                }
            } else {
                Hydro::logWarn("EngineAPI: Could not find FAssetData UScriptStruct");
            }
        }
    }

    // Use hardcoded offsets (verified against C++ source) unless we find better
    if (pkgNameOffset < 0) pkgNameOffset = 0x00;
    if (assetNameOffset < 0) assetNameOffset = 0x10;

    // Compute PackagePath = directory part of PackageName.
    // /Game/Mods/TestMod/Sphere -> /Game/Mods/TestMod
    std::wstring pkgPathStr = packageName;
    auto lastSlash = pkgPathStr.find_last_of(L'/');
    if (lastSlash != std::wstring::npos) pkgPathStr = pkgPathStr.substr(0, lastSlash);
    FName8 pkgPathFName = {};
    safeConstructFName(&pkgPathFName, pkgPathStr.c_str());

    uint8_t params[1024] = {};
    // FAssetData layout (UE 5.x):
    //   +0x00 FName PackageName
    //   +0x08 FName PackagePath        <- required for GetAsset's load path
    //   +0x10 FName AssetName
    //   +0x18 FName AssetClass         <- left as 0; usually optional
    memcpy(params + pkgNameOffset, &pkgFName, 8);      // PackageName
    memcpy(params + 0x08,          &pkgPathFName, 8);  // PackagePath
    memcpy(params + assetNameOffset, &assetFName, 8);   // AssetName

    if (!callProcessEvent(s_assetRegHelpersCDO, s_getAssetFunc, params)) {
        Hydro::logError("EngineAPI: GetAsset ProcessEvent failed");
        return nullptr;
    }

    // Dump params AFTER ProcessEvent to check if our FNames survived initialization
    Hydro::logInfo("EngineAPI: GetAsset post-call (ParmsSize=%u, RetOff=%u):", gaParmsSize, gaRetOffset);
    for (int row = 0; row < 48; row += 16) {
        uint32_t* p = (uint32_t*)(params + row);
        Hydro::logInfo("  +0x%02X: %08X %08X %08X %08X", row, p[0], p[1], p[2], p[3]);
    }

    void* loadedAsset = *(void**)(params + gaRetOffset);
    if (loadedAsset) {
        Hydro::logInfo("EngineAPI: Asset loaded at %p!", loadedAsset);
        return loadedAsset;
    }

    // GetAsset returned null. Most common cause on runtime-mounted paks:
    // AssetRegistry hasn't indexed the pak yet. Trigger an explicit scan
    // of the package's parent directory (e.g. /Game/Mods/) and retry once.
    Hydro::logInfo("EngineAPI: GetAsset null - triggering AssetRegistry scan + retry");
    {
        // Compute the parent directory of the package: /Game/Mods/X/Pkg -> /Game/Mods/X
        std::wstring scanPath = packageName;
        auto slash = scanPath.find_last_of(L'/');
        if (slash != std::wstring::npos) scanPath = scanPath.substr(0, slash);
        if (scanPath.empty()) scanPath = L"/Game";
        scanAssetRegistryPaths(scanPath.c_str(), true);  // forceRescan=true

        // Re-issue GetAsset - its FName params may have been clobbered by
        // the previous call's return-write, so reconstruct.
        memset(params, 0, sizeof(params));
        memcpy(params + pkgNameOffset, &pkgFName, 8);
        memcpy(params + 0x08,          &pkgPathFName, 8);
        memcpy(params + assetNameOffset, &assetFName, 8);
        if (callProcessEvent(s_assetRegHelpersCDO, s_getAssetFunc, params)) {
            loadedAsset = *(void**)(params + gaRetOffset);
            if (loadedAsset) {
                Hydro::logInfo("EngineAPI: Asset loaded at %p after scan!", loadedAsset);
                return loadedAsset;
            }
        }
        Hydro::logInfo("EngineAPI: GetAsset still null after scan. Package '%ls' in memory: %p",
                       packageName.c_str(), findObject(packageName.c_str()));

        // -- ScanFilesSynchronous fallback (Palworld runtime-mount path) --
        // ScanPathsSynchronous's virtual-path lookup relies on FPackageName's
        // mount tree, which on Palworld doesn't contain entries for runtime-
        // mounted paks (the pak-mount broadcast that should have called
        // RegisterMountPoint never fires). ScanFilesSynchronous takes filesystem
        // paths and may bypass that lookup - it reads the .uasset header to
        // derive the package name. Try several candidate filename forms; if
        // any indexes our content, GetAsset can then succeed on retry.
        // Candidate forms (try each, retry GetAsset after):
        //   1. "../../../Pal/Content/Mods/TestMod/Sphere.uasset" (engine virtual,
        //       Palworld project name)
        //   2. "../../../DummyModdableGame/Content/Mods/TestMod/Sphere.uasset"
        //       (matches our test pak's MountPoint header if it was cooked for
        //       DummyModdableGame)
        {
            // Strip leading "/Game/" from package path -> "Mods/TestMod/Sphere"
            std::wstring pkgRel = packageName;
            const wchar_t kGamePrefix[] = L"/Game/";
            if (pkgRel.compare(0, 6, kGamePrefix) == 0) pkgRel = pkgRel.substr(6);

            // Two candidate forms - Palworld project name first, then the cook-
            // time project name from our test pak. Both as .uasset filesystem
            // paths in the engine's virtual root format.
            const wchar_t* projectNames[] = { L"Pal", L"DummyModdableGame" };
            for (const wchar_t* projName : projectNames) {
                std::wstring fsPath = std::wstring(L"../../../") + projName +
                                       L"/Content/" + pkgRel + L".uasset";
                Hydro::logInfo("EngineAPI: trying ScanFilesSynchronous with '%ls'", fsPath.c_str());
                bool scanOk = scanAssetRegistryFiles(fsPath.c_str(), true);
                if (!scanOk) {
                    Hydro::logInfo("EngineAPI:   ScanFilesSynchronous dispatch failed for that candidate");
                    continue;
                }

                // Re-issue GetAsset after this candidate's scan
                memset(params, 0, sizeof(params));
                memcpy(params + pkgNameOffset, &pkgFName, 8);
                memcpy(params + 0x08,          &pkgPathFName, 8);
                memcpy(params + assetNameOffset, &assetFName, 8);
                if (callProcessEvent(s_assetRegHelpersCDO, s_getAssetFunc, params)) {
                    loadedAsset = *(void**)(params + gaRetOffset);
                    if (loadedAsset) {
                        Hydro::logInfo("EngineAPI: Asset loaded at %p after ScanFilesSynchronous(projectName='%ls')!",
                                       loadedAsset, projName);
                        return loadedAsset;
                    }
                }
                Hydro::logInfo("EngineAPI:   GetAsset still null after ScanFilesSynchronous('%ls')", projName);
            }
            Hydro::logInfo("EngineAPI: All ScanFilesSynchronous candidates exhausted - falling through to AR-state diagnostic");
        }

        // -- AR-state introspection diagnostic --
        // We need to disambiguate three hypotheses about why GetAsset returns
        // null on Palworld even after a successful ScanPathsSynchronous:
        //   Claim A: ScanPathsSynchronous registered the asset metadata in
        //     AR's map, but the package isn't loaded - GetAsset's internal
        //     fallback to sync LoadPackage fails on Palworld (anchor strings
        //     stripped). Fix: trigger an async load via a different UFunction.
        //   Claim B: ScanPathsSynchronous registered NOTHING (Palworld AR
        //     doesn't actually parse pak headers at runtime). AR map miss.
        //     Fix: pre-load the package by some other means before scan.
        //   Claim C: AR registered the metadata correctly, but our FAssetData
        //     parameter buffer is malformed for Palworld's UE 5.1 layout.
        //     Fix: rebuild FAssetData with reflected struct offsets.
        // Diagnostic: walk GUObjectArray for ANY object whose full path
        // contains '/Game/Mods/TestMod' (case-insensitive). If matches > 0,
        // pak content IS loaded into memory - Claim B refuted; we then see
        // exactly what's loaded and what isn't (e.g. UPackage but not
        // Sphere_C, or vice versa). If matches == 0, nothing made it into
        // GUObjectArray - strongly suggests Claim B.
        // Bounded one-shot per session to avoid log spam.
        static bool s_arDiagDumped = false;
        if (!s_arDiagDumped) {
            s_arDiagDumped = true;
            Hydro::logInfo("EngineAPI: AR-state diagnostic -- walking GUObjectArray for '/Game/Mods/TestMod' matches");

            // Compute the prefix to match (everything up to the leaf segment
            // of the path being loaded - e.g. /Game/Mods/TestMod for path
            // /Game/Mods/TestMod/Sphere.Sphere_C). Case-folded for compare.
            std::wstring prefix = packageName;
            auto lastSlash = prefix.find_last_of(L'/');
            if (lastSlash != std::wstring::npos) prefix = prefix.substr(0, lastSlash);
            std::string prefixNarrow;
            for (wchar_t c : prefix) {
                char ch = (char)(c & 0x7F);
                if (ch >= 'A' && ch <= 'Z') ch += 32;
                prefixNarrow.push_back(ch);
            }
            Hydro::logInfo("  searching for path-substring '%s' (case-insensitive)", prefixNarrow.c_str());

            void** chunkTable = nullptr;
            int32_t count = 0;
            safeReadPtr((uint8_t*)s_guObjectArray + FARRAY_OBJECTS, (void**)&chunkTable);
            safeReadInt32((uint8_t*)s_guObjectArray + FARRAY_NUM_ELEMS, &count);

            int matches = 0;
            const int kMaxDump = 30;
            if (chunkTable && count > 0) {
                for (int32_t i = 0; i < count && matches < kMaxDump; i++) {
                    int chunkIdx2    = i / CHUNK_SIZE;
                    int withinChunk2 = i % CHUNK_SIZE;
                    void* chunk2 = nullptr;
                    if (!safeReadPtr((uint8_t*)&chunkTable[chunkIdx2], &chunk2) || !chunk2) continue;
                    uint8_t* item2 = (uint8_t*)chunk2 + (withinChunk2 * FUOBJ_SIZE);
                    void* obj2 = nullptr;
                    if (!safeReadPtr(item2 + FUOBJ_OBJECT, &obj2) || !obj2) continue;

                    std::string p = getObjectPath(obj2);
                    if (p.empty()) continue;
                    // Case-fold and substring search
                    std::string pLower;
                    pLower.reserve(p.size());
                    for (char ch : p) {
                        if (ch >= 'A' && ch <= 'Z') ch += 32;
                        pLower.push_back(ch);
                    }
                    if (pLower.find(prefixNarrow) == std::string::npos) continue;

                    matches++;
                    void* cls = getClass(obj2);
                    std::string clsName = cls ? getObjectName(cls) : std::string("?");
                    Hydro::logInfo("  match[%d] %p class='%s' path='%s'",
                                   matches, obj2, clsName.c_str(), p.c_str());
                }
            }
            if (matches == 0) {
                Hydro::logInfo("  RESULT: NO objects in GUObjectArray under '%s' - pak content was not loaded into memory by scan or mount.", prefixNarrow.c_str());
            } else {
                Hydro::logInfo("  RESULT: %d object(s) under prefix found loaded - pak content IS in memory.", matches);
            }

            // -- UPackage census --
            // Walk GUObjectArray for ALL UPackage instances. Tells us:
            //   (a) Total package count: confirms our walk reaches everything
            //   (b) Are there ANY /Game/Mods/* packages? (zero = mount broadcast
            //       definitely didn't fire for runtime-mounted paks)
            //   (c) Are there /Game/Pal/* packages? (baseline - base game
            //       content; if zero, our walk is broken not the broadcast)
            //   (d) First 10 UPackage paths: shows what's there
            int totalPkg = 0, gameModsPkg = 0, gamePalPkg = 0, scriptPkg = 0, gamePkg = 0;
            int firstPkgsLogged = 0;
            const int kMaxPkgsToLog = 10;
            for (int32_t i = 0; i < count; i++) {
                int chunkIdx3    = i / CHUNK_SIZE;
                int withinChunk3 = i % CHUNK_SIZE;
                void* chunk3 = nullptr;
                if (!safeReadPtr((uint8_t*)&chunkTable[chunkIdx3], &chunk3) || !chunk3) continue;
                uint8_t* item3 = (uint8_t*)chunk3 + (withinChunk3 * FUOBJ_SIZE);
                void* obj3 = nullptr;
                if (!safeReadPtr(item3 + FUOBJ_OBJECT, &obj3) || !obj3) continue;
                void* cls = getClass(obj3);
                if (!cls) continue;
                std::string clsName = getObjectName(cls);
                if (clsName != "Package") continue;
                totalPkg++;
                std::string p3 = getObjectPath(obj3);
                // Pkg paths look like "/Game/Pal/...", "/Script/...", "/Game/Mods/..."
                if (p3.find("/Game/Mods") != std::string::npos) gameModsPkg++;
                else if (p3.find("/Game/Pal") != std::string::npos) gamePalPkg++;
                else if (p3.find("/Script") != std::string::npos) scriptPkg++;
                else if (p3.find("/Game") != std::string::npos) gamePkg++;
                if (firstPkgsLogged < kMaxPkgsToLog) {
                    Hydro::logInfo("  UPackage[%d] %s", firstPkgsLogged, p3.c_str());
                    firstPkgsLogged++;
                }
            }
            Hydro::logInfo("  UPackage census: total=%d, /Game/Mods=%d, /Game/Pal=%d, /Script=%d, other /Game=%d",
                           totalPkg, gameModsPkg, gamePalPkg, scriptPkg, gamePkg);
            if (gameModsPkg == 0 && gamePalPkg > 0) {
                Hydro::logInfo("  -> Diagnosis: mount broadcast did NOT fire for runtime-mounted pak. Base game packages registered correctly (gamePal>0), but our pak's mount point was never broadcast. Need explicit FCoreDelegates::OnPakFileMounted2 broadcast OR FPackageName::RegisterMountPoint call.");
            } else if (gameModsPkg == 0 && gamePalPkg == 0) {
                Hydro::logInfo("  -> Diagnosis: NO /Game/* packages found at all. Either GUObjectArray walk is incomplete, or AR is sealed for this entire path tree.");
            }
            Hydro::logInfo("EngineAPI: AR-state diagnostic done --");
        }
    }

    return nullptr;
}

// LoadPackage signature variants. We try the simpler 3-arg public variant
// first, then the internal variant if needed. Both are SEH-wrapped because
// pattern-scanning may have landed us on the wrong function on forked engines.
using LoadPackageFn3 = void*(__fastcall*)(void* InOuter, const wchar_t* PackageName, uint32_t LoadFlags);

static void* safeCallLoadPackage3(void* fn, const wchar_t* pkg, uint32_t flags) {
#ifdef _WIN32
    __try {
        auto f = (LoadPackageFn3)fn;
        return f(nullptr, pkg, flags);
    } __except(1) { return nullptr; }
#else
    auto f = (LoadPackageFn3)fn;
    return f(nullptr, pkg, flags);
#endif
}

// Legacy loadObject - three-tier cascade:
//   1. discovered StaticLoadObject (works on stock UE; broken on some forks)
//   2. LoadPackage(packagePath) + findObject(fullPath) - survives a broken
//      StaticLoadObject because LoadPackage is a separate function with its
//      own anchor string ("Attempted to LoadPackage"), and findObject is
//      now GUObjectArray-walk-based and reliable
//   3. StaticFindObject (in-memory only - last-ditch for already-loaded objects)

void* loadObject(const wchar_t* path) {
    if (!path) return nullptr;

    // Tier 1: discovered StaticLoadObject. Try multiple flag combos.
    if (s_staticLoadObject) {
        uint32_t flagSets[] = { 0x0A, 0x00, 0x02, 0x2000 };
        for (auto flags : flagSets) {
            void* result = safeCallLoadObject(s_staticLoadObject, path, flags);
            if (result) {
                if (flags != 0) Hydro::logInfo("EngineAPI: Loaded via StaticLoadObject with flags 0x%X", flags);
                return result;
            }
        }
    }

    // Tier 2: LoadPackage(packagePath) + findObject(fullPath). For a path
    // like "/Game/Mods/M/X.X_C", split at the LAST '.' - the part before is
    // the package path, the full string is the object path. After
    // LoadPackage successfully loads the package from disk (or a mounted
    // pak), findObject can resolve the asset via GUObjectArray walk.
    if (s_loadPackage) {
        std::wstring full(path);
        auto dot = full.rfind(L'.');
        std::wstring pkg = (dot == std::wstring::npos) ? full : full.substr(0, dot);
        // 0 = LOAD_None, 0x0A = LOAD_NoVerify | LOAD_NoWarn, 0x2000 = LOAD_Quiet
        uint32_t flagSets[] = { 0x00, 0x0A, 0x2000 };
        for (auto flags : flagSets) {
            void* loaded = safeCallLoadPackage3(s_loadPackage, pkg.c_str(), flags);
            (void)loaded; // its return value may or may not be a UPackage; we don't trust it
            // Whether or not LoadPackage returned a usable value, the package
            // may now be in memory. Try findObject for the requested asset.
            void* obj = findObject(path);
            if (obj) {
                Hydro::logInfo("EngineAPI: Loaded via LoadPackage('%ls', flags=0x%X) + findObject",
                               pkg.c_str(), flags);
                return obj;
            }
        }
    }

    // Tier 3: StaticFindObject (in-memory only).
    if (s_staticFindObject) {
        void* result = safeCallLoadObject(s_staticFindObject, path, 0);
        if (result) return result;
    }

    return nullptr;
}

// Public API: refreshWorld

// Cached GWorld pointer location - once found, just dereference to get current UWorld
static void** s_gWorldPtr = nullptr;

bool refreshWorld() {
    if (!s_staticLoadObject || !s_gm.base) return false;

    void* oldWorld = s_world;

    // Fast path: if we already know where GWorld lives, just dereference
    if (s_gWorldPtr) {
        void* candidate = nullptr;
        if (safeReadPtr(s_gWorldPtr, &candidate) && candidate) {
            s_world = candidate;
            if (s_world != oldWorld)
                Hydro::logInfo("EngineAPI: World refreshed (fast): %p -> %p", oldWorld, s_world);
            return true;
        }
    }

    // Slow path: find UWorld class, then scan
    auto loadObj = (StaticLoadObjectFn_t)s_staticLoadObject;
    void* worldClass = nullptr;
#ifdef _WIN32
    __try {
        worldClass = loadObj(nullptr, nullptr, L"/Script/Engine.World", nullptr, 0, nullptr, true, nullptr);
    } __except(1) { worldClass = nullptr; }
#endif
    if (!worldClass) {
        Hydro::logWarn("EngineAPI: refreshWorld - UWorld class not found");
        return false;
    }

    s_world = nullptr;

    // Try GUObjectArray first
    if (s_guObjectArray) {
        int32_t count = getObjectCount();
        for (int32_t i = 0; i < count; i++) {
            void* obj = getObjectAt(i);
            if (!obj) continue;
            if (getObjClass(obj) == worldClass) {
                s_world = obj;
                break;
            }
        }
    }

    // Fallback: scan for GWorld global pointer (and cache it for fast path)
    if (!s_world) {
        for (size_t i = 0; i + 7 < s_gm.size; i++) {
            uint8_t* p = s_gm.base + i;
            if (p[0] != 0x48 || p[1] != 0x8B || p[2] != 0x05) continue;
            int32_t disp = *(int32_t*)(p + 3);
            void** globalPtr = (void**)(p + 7 + disp);
            if ((uint8_t*)globalPtr < s_gm.base || (uint8_t*)globalPtr >= s_gm.base + s_gm.size)
                continue;
            void* candidate = nullptr;
            if (!safeReadPtr(globalPtr, &candidate) || !candidate) continue;
            if ((uintptr_t)candidate < 0x10000) continue;
            if ((uint8_t*)candidate >= s_gm.base && (uint8_t*)candidate < s_gm.base + s_gm.size)
                continue;
            void* cls = nullptr;
            if (!safeReadPtr((uint8_t*)candidate + UOBJ_CLASS, &cls)) continue;
            if (cls == worldClass) {
                s_world = candidate;
                s_gWorldPtr = globalPtr; // Cache for fast path
                Hydro::logInfo("EngineAPI: GWorld cached at %p", globalPtr);
                break;
            }
        }
    }

    if (s_world != oldWorld) {
        Hydro::logInfo("EngineAPI: World refreshed: %p -> %p", oldWorld, s_world);
    }

    return s_world != nullptr;
}

// Public API: spawnActor

// BeginDeferredActorSpawnFromClass params (UE 5.5, As502Plus)
#pragma pack(push, 1)
struct alignas(16) SpawnParams {
    void*    worldContext;       // 0x00
    void*    actorClass;        // 0x08
    // FTransform (96 bytes, 16-byte aligned)
    double   rotX, rotY, rotZ, rotW;  // 0x10 - FQuat
    double   locX, locY, locZ;        // 0x30 - FVector translation
    double   pad1;                     // 0x48
    double   scaleX, scaleY, scaleZ;  // 0x50 - FVector scale
    double   pad2;                     // 0x68
    uint8_t  collisionMethod;   // 0x70
    uint8_t  pad3[7];           // 0x71
    void*    owner;             // 0x78
    uint8_t  scaleMethod;       // 0x80
    uint8_t  pad4[7];           // 0x81
    void*    returnValue;       // 0x88 - OUT
};
#pragma pack(pop)

void* getProcessEventAddress() {
    return (void*)s_processEvent;
}

// --- AActor::DispatchBeginPlay discovery ---
// Background: DispatchBeginPlay is the non-virtual engine funnel that every
// actor's BeginPlay passes through. Hooking it catches every actor's begin-
// play exactly once, regardless of subclass vtable overrides. Hooking the
// virtual AActor::BeginPlay directly misses calls dispatched through
// subclass vtables, which is why our earlier attempt produced zero fires.
// We find DispatchBeginPlay by scanning for its distinctive last-instruction
// shape: `call qword ptr [reg + BeginPlay_vtable_offset]`, where the offset
// is whatever vtable slot AActor::BeginPlay occupies in the current engine
// version (0x3A0 in UE 5.5). That instruction is 6 or 7 bytes of the form
// `FF 9X XX XX XX XX` (ModRM byte 0x90..0x97 selects the register; no SIB).
// A literal byte scan finds MANY matches - any function that does a virtual
// call at that offset. To narrow to DispatchBeginPlay we also require:
//   - exactly ONE such virtual call in the enclosing function
//   - function size is small (DispatchBeginPlay is ~40-200 bytes)
//   - the function is called by OTHER code in the module (non-leaf caller)
// Candidates that pass are scored. The single best candidate wins. All
// candidates are logged so a future engine version shift is debuggable.

// Walk backwards from `at` looking for the function start. Heuristic: the
// previous function ends with a RET (0xC3/0xC2) followed by alignment padding
// (0xCC INT3 bytes). When we see that padding run, the byte after it is the
// next function's entry. We cap the walk at 4KB to keep cost bounded.
static uint8_t* findFunctionStartBackwards(uint8_t* at, uint8_t* modBase) {
    uint8_t* p = at;
    uint8_t* limit = (p > modBase + 4096) ? p - 4096 : modBase;
    for (; p > limit; p--) {
        // Look for the pattern: RET (0xC3 or 0xC2 xx xx) followed by 0xCC padding.
        // The candidate function starts right after the padding run.
        if (p[0] == 0xCC) {
            // Walk forward past the padding run to the first non-CC byte.
            uint8_t* q = p;
            while (q < at && *q == 0xCC) q++;
            // Preceded by a RET or near-RET instruction? Check a few bytes back.
            // C3 = RET, C2 xx xx = RET imm16, E9 xx xx xx xx = JMP rel32 (tail call)
            uint8_t prev = (p > modBase) ? *(p - 1) : 0;
            if (prev == 0xC3 || prev == 0xCC) {
                return q;
            }
            // Also accept "JMP rel32" as function boundary (tail-call ending)
            if (p > modBase + 5 && *(p - 5) == 0xE9) {
                return q;
            }
        }
    }
    return nullptr;
}

struct DbpCandidate {
    uint8_t* funcStart;
    size_t approxSize;
    int virtualCalls;    // count of call [reg+offset] matching the target offset
    int flagWrites;      // count of mov byte ptr [reg+N], imm8 (actor flag sets)
    int callers;         // count of direct E8 calls targeting this function
    int score;
};

// Count direct CALL instructions (E8 rel32) targeting `target` across a
// subset of the module. DispatchBeginPlay has multiple callers (at least
// UWorld::InitializeActorsForPlay and AActor::PostActorConstruction), so
// a callers count of 0 rules out many false positives. We cap the search
// region so this doesn't blow the frame budget on a module that might be
// hundreds of MB.
static int countDirectCallers(uint8_t* target, uint8_t* modBase, size_t modSize) {
    int count = 0;
    const size_t kMaxScan = 64 * 1024 * 1024;  // 64MB - plenty for .text
    size_t limit = modSize < kMaxScan ? modSize : kMaxScan;
    for (size_t i = 0; i + 5 < limit; i++) {
        if (modBase[i] != 0xE8) continue;
        int32_t rel = *(int32_t*)(modBase + i + 1);
        uint8_t* dst = modBase + i + 5 + rel;
        if (dst == target && ++count >= 4) return count;  // 4+ is plenty
    }
    return count;
}

void* findDispatchBeginPlay(int beginPlayVtableOffset) {
    if (!s_gm.base || !s_gm.size) {
        logError("EngineAPI: findDispatchBeginPlay - game module not found");
        return nullptr;
    }

    uint8_t* mod = s_gm.base;
    size_t modSize = s_gm.size;
    int32_t targetOffset = (int32_t)beginPlayVtableOffset;

    // Pass 1: find every `FF /2 m32` (near call to memory with disp32) whose
    // displacement matches the BeginPlay vtable offset. ModRM byte is
    // 0x90..0x97 (mod=10, reg=010 for CALL, rm picks the register - we don't
    // care which). SIB-indexed forms (rm=4, 0x94) are possible but rare for
    // virtual dispatch and we skip them to avoid SIB parsing.
    std::vector<uint8_t*> hits;
    for (size_t i = 0; i + 6 < modSize; i++) {
        if (mod[i] != 0xFF) continue;
        uint8_t modrm = mod[i + 1];
        // mod=10 (disp32), reg=010 (CALL), rm=0..7 except 4 (SIB)
        if ((modrm & 0xF8) != 0x90) continue;
        if ((modrm & 0x07) == 4) continue;
        int32_t disp = *(int32_t*)(mod + i + 2);
        if (disp == targetOffset) {
            hits.push_back(mod + i);
        }
    }
    logInfo("EngineAPI: DispatchBeginPlay scan - %zu virtual-call hits at offset 0x%X",
            hits.size(), targetOffset);
    if (hits.empty()) return nullptr;

    // Pass 2: group hits by enclosing function. Build candidate set.
    std::unordered_map<uint8_t*, DbpCandidate> candidates;
    for (uint8_t* hit : hits) {
        uint8_t* start = findFunctionStartBackwards(hit, mod);
        if (!start) continue;
        auto& c = candidates[start];
        c.funcStart = start;
        c.virtualCalls++;
    }

    // Pass 3a: cheap structural scoring for every unique candidate (local
    // only - no full-module scans). This trims the field before we spend
    // effort counting direct callers, which is quadratic otherwise.
    std::vector<DbpCandidate*> scored;
    for (auto& [start, c] : candidates) {
        // Approximate function size: scan forward until first RET + padding.
        size_t maxScan = 512;
        size_t fnLen = 0;
        for (size_t j = 0; j < maxScan && (start + j) < (mod + modSize); j++) {
            if (start[j] == 0xC3 || start[j] == 0xC2) {
                fnLen = j + 1;
                break;
            }
        }
        if (fnLen == 0) fnLen = 64;  // unknown; assume reasonable
        c.approxSize = fnLen;

        // Flag writes: count `mov byte ptr [reg+N], imm8` patterns (C6 4X NN II)
        // inside the function. DispatchBeginPlay writes the HasActorBegunPlay
        // bitfield before calling BeginPlay.
        for (size_t j = 0; j + 4 < fnLen; j++) {
            if (start[j] == 0xC6 && (start[j + 1] & 0xF0) == 0x40) {
                c.flagWrites++;
            }
        }
        c.callers = 0;  // computed later for the top-N only
        scored.push_back(&c);
    }

    // Cheap pre-rank: virtualCalls==1 + reasonable size wins first.
    std::sort(scored.begin(), scored.end(),
              [](const DbpCandidate* a, const DbpCandidate* b) {
                  if ((a->virtualCalls == 1) != (b->virtualCalls == 1))
                      return a->virtualCalls == 1;
                  bool aSize = a->approxSize >= 32 && a->approxSize <= 256;
                  bool bSize = b->approxSize >= 32 && b->approxSize <= 256;
                  if (aSize != bSize) return aSize;
                  return a->approxSize < b->approxSize;
              });

    // Pass 3b: count direct callers only for the top 10 - keeps the scan
    // bounded regardless of total candidate count. 10 is plenty because
    // the cheap pre-rank puts the right candidate near the top.
    size_t deepScan = scored.size() < 10 ? scored.size() : (size_t)10;
    for (size_t i = 0; i < deepScan; i++) {
        scored[i]->callers = countDirectCallers(scored[i]->funcStart, mod, modSize);
    }

    // Score all candidates. Rubric:
    //   +1000 if exactly 1 virtual call at the offset (DispatchBeginPlay
    //         invokes BeginPlay exactly once - functions with many such
    //         calls are almost certainly something else)
    //   +200 per caller (non-leaf -> more likely a dispatch funnel)
    //   +100 if function is reasonably sized (32..256 bytes)
    //   +50 per flag write (up to 3)
    //   -200 if function is very large (>400 bytes; DispatchBeginPlay is tight)
    for (auto* cp : scored) {
        DbpCandidate& c = *cp;
        c.score = 0;
        if (c.virtualCalls == 1) c.score += 1000;
        c.score += c.callers * 200;
        if (c.approxSize >= 32 && c.approxSize <= 256) c.score += 100;
        c.score += (c.flagWrites < 3 ? c.flagWrites : 3) * 50;
        if (c.approxSize > 400) c.score -= 200;
    }

    // Sort by score descending, log top candidates for debuggability.
    std::sort(scored.begin(), scored.end(),
              [](const DbpCandidate* a, const DbpCandidate* b) { return a->score > b->score; });

    size_t topN = scored.size() < 5 ? scored.size() : (size_t)5;
    logInfo("EngineAPI: DispatchBeginPlay - %zu unique candidates, top %zu:",
            scored.size(), topN);
    for (size_t i = 0; i < topN; i++) {
        const DbpCandidate* c = scored[i];
        logInfo("  [%zu] score=%d func=exe+0x%zX size=%zu vcalls=%d flagWr=%d callers=%d",
                i, c->score, (size_t)(c->funcStart - mod),
                c->approxSize, c->virtualCalls, c->flagWrites, c->callers);
    }

    if (!scored.empty() && scored[0]->score > 500) {
        uint8_t* bestFunc = scored[0]->funcStart;
        logInfo("EngineAPI: DispatchBeginPlay chosen at %p (exe+0x%zX, score=%d)",
                bestFunc, (size_t)(bestFunc - mod), scored[0]->score);
        return bestFunc;
    }

    logError("EngineAPI: no strong DispatchBeginPlay candidate (best score=%d)",
             scored.empty() ? 0 : scored[0]->score);
    return nullptr;
}

static bool callProcessEvent(void* obj, void* func, void* params) {
    if (!obj || !func) return false;

    // Virtual call through the TARGET object's own vtable - matches how
    // UE4SS and the engine itself call ProcessEvent. Different classes
    // may have different ProcessEvent implementations.
    void** vtable = *(void***)obj;
    if (!vtable) return false;
    auto pe = (ProcessEventFn)vtable[VTABLE_PROCESS_EVENT];
    if (!pe) return false;

    bool crashed = false;
    DWORD crashCode = 0;
    void* crashAddr = nullptr;
#ifdef _WIN32
    __try {
        pe(obj, func, params);
    } __except(crashCode = GetExceptionCode(),
               crashAddr = GetExceptionInformation()->ExceptionRecord->ExceptionAddress,
               EXCEPTION_EXECUTE_HANDLER) {
        crashed = true;
    }
#else
    pe(obj, func, params);
#endif

    if (crashed) {
        Hydro::logError("EngineAPI: ProcessEvent crashed (code=0x%08X at %p, obj=%p func=%p)",
            (unsigned)crashCode, crashAddr, obj, func);
        return false;
    }
    return true;
}

// Cached param offsets for BeginDeferredActorSpawnFromClass. Looked up once
// per session via the UFunction's property chain, so the layout is correct
// for whichever UE 5.x build the host uses (UE 5.1 / 5.5 / 5.6 may shift
// alignment or insert/remove flags). Using static + sentinel; -1 means
// "not yet resolved" so we re-attempt on first spawn after engine init.
struct SpawnParamLayout {
    bool resolved = false;
    int32_t worldOff = -1;
    int32_t classOff = -1;
    int32_t transformOff = -1;
    int32_t collisionOff = -1;
    int32_t ownerOff = -1;
    int32_t scaleMethodOff = -1;
    int32_t retOff = -1;
    int32_t totalSize = -1;
};
static SpawnParamLayout s_spawnLayout;

static void resolveSpawnLayout() {
    if (s_spawnLayout.resolved) return;
    if (!s_spawnFunc) return;
    // BeginDeferredActorSpawnFromClass param names (must match UE source):
    auto off = [](const wchar_t* n) -> int32_t {
        void* prop = findProperty(s_spawnFunc, n);
        return prop ? getPropertyOffset(prop) : -1;
    };
    s_spawnLayout.worldOff       = off(L"WorldContextObject");
    s_spawnLayout.classOff       = off(L"ActorClass");
    s_spawnLayout.transformOff   = off(L"SpawnTransform");
    s_spawnLayout.collisionOff   = off(L"CollisionHandlingOverride");
    s_spawnLayout.ownerOff       = off(L"Owner");
    s_spawnLayout.scaleMethodOff = off(L"TransformScaleMethod");
    s_spawnLayout.retOff         = (int32_t)getUFunctionRetOffset(s_spawnFunc);
    s_spawnLayout.totalSize      = (int32_t)getUFunctionParmsSize(s_spawnFunc);
    s_spawnLayout.resolved = (s_spawnLayout.worldOff >= 0 && s_spawnLayout.classOff >= 0
        && s_spawnLayout.transformOff >= 0 && s_spawnLayout.totalSize > 0);
    Hydro::logInfo("EngineAPI: SpawnParams layout (reflection-derived): "
        "World=%d Class=%d Transform=%d Collision=%d Owner=%d ScaleMethod=%d Ret=%d Total=%d resolved=%d",
        s_spawnLayout.worldOff, s_spawnLayout.classOff, s_spawnLayout.transformOff,
        s_spawnLayout.collisionOff, s_spawnLayout.ownerOff, s_spawnLayout.scaleMethodOff,
        s_spawnLayout.retOff, s_spawnLayout.totalSize, s_spawnLayout.resolved ? 1 : 0);
}

static bool isActorSubclass(void* uclass);
static void* findInWorldActor();

void* spawnActor(void* actorClass, double x, double y, double z) {
    if (!s_ready || !s_spawnFunc || !s_gameplayStaticsCDO) {
        Hydro::logError("EngineAPI: Not ready for spawning");
        return nullptr;
    }
    if (!actorClass) {
        Hydro::logError("EngineAPI: actorClass is null");
        return nullptr;
    }

    // Refresh world pointer before each spawn - the world may have changed
    // since initialize() ran (e.g., map transition from menu to gameplay).
    refreshWorld();
    if (!s_world) {
        Hydro::logError("EngineAPI: No valid UWorld found");
        return nullptr;
    }

    Hydro::logInfo("EngineAPI: Spawning actor at (%.0f, %.0f, %.0f) in world %p...", x, y, z, s_world);

    // Resolve param offsets via reflection on first call. If the property
    // chain isn't readable yet (e.g., very early init), fall back to the
    // hardcoded UE 5.5 layout so existing UE 5.5 paths still work.
    resolveSpawnLayout();

    if (s_spawnLayout.resolved && s_spawnLayout.totalSize <= 256) {
        // BeginDeferredActorSpawnFromClass has meta=(WorldContext="WorldContextObject")
        // - UE's GetWorldFromContextObject() calls obj->GetWorld() and expects
        // the object to BE in a world. UWorld::GetWorld() returns nullptr in
        // some UE 5.5 builds (DMG ThirdPersonMap empirically), causing access
        // violation deep in the spawn code. Prefer a player Pawn/Character
        // as WorldContext (canonical BP-callable pattern).
        // WorldContext resolution. UE's GetWorldFromContextObject expects an
        // object that's IN a world (i.e. an actor instance). A UWorld* itself
        // does not satisfy this (DMG UE 5.5 empirically crashes). A CDO
        // also fails (returns null GetWorld). We need a real in-world actor.
        // Strategy: prime the find cache by calling findFirstOf once (which
        // resolves the UClass* via GUObjectArray walk - may return a CDO),
        // then call again - the second call routes through
        // UGameplayStatics::GetAllActorsOfClass, which ONLY returns
        // in-world instances, never CDOs.
        // Helper: detect Class Default Object via UObject flags. RF_ClassDefaultObject
        // = 0x10 - stable across UE versions. CDOs have no world (GetWorld()
        // returns null), so passing one as WorldContext fails the spawn.
        auto isCDO = [](void* obj) {
            if (!obj) return true;
            uint32_t flags = 0;
            if (!safeReadInt32((uint8_t*)obj + UOBJ_FLAGS, (int32_t*)&flags)) return true;
            return (flags & 0x10) != 0;  // RF_ClassDefaultObject
        };

        void* worldContext = nullptr;
        const char* wcSource = nullptr;

        // Walk GUObjectArray to find a non-CDO actor instance. We can't rely
        // on findFirstOf("Character") because:
        //   - It matches by class-of-obj, so the very first object with class
        //     Character is `Default__Character` (the CDO).
        //   - getAllActorsOfClass on the *generic* Character class may return
        //     0 (the player is a DMG-specific subclass like
        //     DummyModdableGameCharacter), causing fallback to GUObjectArray
        //     walk that re-picks the CDO.
        // Direct walk + CDO filter is the resilient option.
        if (s_guObjectArray) {
            int32_t count = getObjectCount();
            for (int32_t i = 0; i < count && !worldContext; i++) {
                void* obj = getObjectAt(i);
                if (!obj || isCDO(obj)) continue;
                void* cls = getObjClass(obj);
                if (!cls) continue;
                if (!isActorSubclass(cls)) continue;
                // Found a non-CDO actor instance with a class chain through AActor.
                worldContext = obj;
                wcSource = "GUObjectArray walk (non-CDO actor instance)";
            }
        }
        if (!worldContext) {
            worldContext = getPlayerCharacter(0);
            if (worldContext) wcSource = "getPlayerCharacter UFunction";
        }
        if (!worldContext) {
            worldContext = getPlayerPawn(0);
            if (worldContext) wcSource = "getPlayerPawn UFunction";
        }
        if (!worldContext) {
            worldContext = s_world;
            wcSource = "UWorld* (last-resort fallback - likely to crash)";
        }
        // Diagnostic: print WorldContext object name + class name so we can
        // see whether we picked a real in-world actor or a CDO/template.
        if (worldContext) {
            uint32_t wcNameIdx = 0;
            safeReadInt32((uint8_t*)worldContext + UOBJ_NAME, (int32_t*)&wcNameIdx);
            std::string wcName = getNameString(wcNameIdx);
            void* wcClass = getObjClass(worldContext);
            uint32_t wcClassNameIdx = 0;
            if (wcClass) safeReadInt32((uint8_t*)wcClass + UOBJ_NAME, (int32_t*)&wcClassNameIdx);
            std::string wcClassName = wcClass ? getNameString(wcClassNameIdx) : "<null>";
            uint32_t wcFlags = 0;
            safeReadInt32((uint8_t*)worldContext + UOBJ_FLAGS, (int32_t*)&wcFlags);
            // Diagnostic on actor class too
            uint32_t acNameIdx = 0;
            safeReadInt32((uint8_t*)actorClass + UOBJ_NAME, (int32_t*)&acNameIdx);
            std::string acName = getNameString(acNameIdx);
            void* acClass = getObjClass(actorClass);
            uint32_t acClassNameIdx = 0;
            if (acClass) safeReadInt32((uint8_t*)acClass + UOBJ_NAME, (int32_t*)&acClassNameIdx);
            std::string acClassName = acClass ? getNameString(acClassNameIdx) : "<null>";
            Hydro::logInfo("EngineAPI: WorldContext = %p name='%s' class='%s' flags=0x%X (%s)",
                worldContext, wcName.c_str(), wcClassName.c_str(), wcFlags, wcSource);
            Hydro::logInfo("EngineAPI: ActorClass = %p name='%s' class='%s'",
                actorClass, acName.c_str(), acClassName.c_str());
        } else {
            Hydro::logInfo("EngineAPI: WorldContext for spawn = %p (%s)",
                worldContext, wcSource);
        }
        // Reflection-derived param block - version-resilient path.
        alignas(16) uint8_t params[256] = {};
        *(void**)(params + s_spawnLayout.worldOff) = worldContext;
        *(void**)(params + s_spawnLayout.classOff) = actorClass;
        // FTransform: FQuat at +0x00 (32 bytes), Translation FVector at +0x20
        // (24+8 pad = 32 bytes), Scale3D FVector at +0x40 (24+8 pad). Total
        // 96 bytes, double-precision (UE 5.x default since 5.0). Layout
        // within FTransform itself is stable across UE 5.x.
        uint8_t* tx = params + s_spawnLayout.transformOff;
        *(double*)(tx + 0x00) = 0.0;  // QuatX
        *(double*)(tx + 0x08) = 0.0;  // QuatY
        *(double*)(tx + 0x10) = 0.0;  // QuatZ
        *(double*)(tx + 0x18) = 1.0;  // QuatW (identity)
        *(double*)(tx + 0x20) = x;    // TranslationX
        *(double*)(tx + 0x28) = y;
        *(double*)(tx + 0x30) = z;
        *(double*)(tx + 0x40) = 1.0;  // ScaleX
        *(double*)(tx + 0x48) = 1.0;
        *(double*)(tx + 0x50) = 1.0;
        if (s_spawnLayout.collisionOff >= 0)
            *(uint8_t*)(params + s_spawnLayout.collisionOff) = 1;  // AlwaysSpawn
        if (s_spawnLayout.ownerOff >= 0)
            *(void**)(params + s_spawnLayout.ownerOff) = nullptr;
        if (s_spawnLayout.scaleMethodOff >= 0)
            *(uint8_t*)(params + s_spawnLayout.scaleMethodOff) = 1;  // MultiplyWithRoot
        // ReturnValue slot is zero already.

        if (!callProcessEvent(s_gameplayStaticsCDO, s_spawnFunc, params)) {
            Hydro::logError("EngineAPI: BeginDeferredActorSpawnFromClass failed (reflection path, ProcessEvent crashed)");
            return nullptr;
        }
        void* result = (s_spawnLayout.retOff >= 0)
            ? *(void**)(params + s_spawnLayout.retOff)
            : nullptr;
        if (!result) {
            Hydro::logError("EngineAPI: BeginDeferredActorSpawnFromClass returned null");
            return nullptr;
        }

        // FinishSpawningActor - derive its param layout reflectively too.
        if (s_finishSpawnFunc) {
            int32_t fsActorOff = -1, fsTxOff = -1, fsScaleOff = -1;
            uint16_t fsTotal = getUFunctionParmsSize(s_finishSpawnFunc);
            if (void* p = findProperty(s_finishSpawnFunc, L"Actor"))
                fsActorOff = getPropertyOffset(p);
            if (void* p = findProperty(s_finishSpawnFunc, L"SpawnTransform"))
                fsTxOff = getPropertyOffset(p);
            if (void* p = findProperty(s_finishSpawnFunc, L"TransformScaleMethod"))
                fsScaleOff = getPropertyOffset(p);
            if (fsActorOff >= 0 && fsTxOff >= 0 && fsTotal <= 512) {
                alignas(16) uint8_t fsParams[512] = {};
                *(void**)(fsParams + fsActorOff) = result;
                uint8_t* fsTx = fsParams + fsTxOff;
                *(double*)(fsTx + 0x00) = 0.0;
                *(double*)(fsTx + 0x08) = 0.0;
                *(double*)(fsTx + 0x10) = 0.0;
                *(double*)(fsTx + 0x18) = 1.0;
                *(double*)(fsTx + 0x20) = x;
                *(double*)(fsTx + 0x28) = y;
                *(double*)(fsTx + 0x30) = z;
                *(double*)(fsTx + 0x40) = 1.0;
                *(double*)(fsTx + 0x48) = 1.0;
                *(double*)(fsTx + 0x50) = 1.0;
                if (fsScaleOff >= 0)
                    *(uint8_t*)(fsParams + fsScaleOff) = 1;
                if (!callProcessEvent(s_gameplayStaticsCDO, s_finishSpawnFunc, fsParams))
                    Hydro::logWarn("EngineAPI: FinishSpawningActor failed (reflection path)");
            } else {
                Hydro::logWarn("EngineAPI: FinishSpawningActor reflection lookup failed; skipping finish step");
            }
        }
        return result;
    }

    // Fallback: hardcoded UE 5.5 layout (legacy path, kept for cache-load
    // case where reflection chain isn't walked yet on this session).
    Hydro::logWarn("EngineAPI: SpawnParams reflection unavailable; using hardcoded UE 5.5 layout");
    SpawnParams sp = {};
    sp.worldContext = s_world;
    sp.actorClass = actorClass;
    sp.rotX = 0.0; sp.rotY = 0.0; sp.rotZ = 0.0; sp.rotW = 1.0; // identity quat
    sp.locX = x; sp.locY = y; sp.locZ = z;
    sp.scaleX = 1.0; sp.scaleY = 1.0; sp.scaleZ = 1.0;
    sp.collisionMethod = 1; // AlwaysSpawn (enum value 1, NOT 3)
    sp.owner = nullptr;
    sp.scaleMethod = 1; // MultiplyWithRoot
    sp.returnValue = nullptr;

    if (!callProcessEvent(s_gameplayStaticsCDO, s_spawnFunc, &sp) || !sp.returnValue) {
        Hydro::logError("EngineAPI: BeginDeferredActorSpawnFromClass failed");
        return nullptr;
    }

    // FinishSpawningActor - completes construction, registers components, dispatches BeginPlay
    if (s_finishSpawnFunc) {
        // Params: Actor(0x00), pad(0x08), FTransform(0x10-0x6F), ScaleMethod(0x70), ReturnValue(0x78)
        alignas(16) uint8_t fsParams[512] = {};
        *(void**)(fsParams + 0x00) = sp.returnValue;
        // FTransform: FQuat(0x10) + FVector Translation(0x30) + FVector Scale3D(0x50)
        *(double*)(fsParams + 0x10) = 0.0;
        *(double*)(fsParams + 0x18) = 0.0;
        *(double*)(fsParams + 0x20) = 0.0;
        *(double*)(fsParams + 0x28) = 1.0; // identity quat W
        *(double*)(fsParams + 0x30) = x;
        *(double*)(fsParams + 0x38) = y;
        *(double*)(fsParams + 0x40) = z;
        *(double*)(fsParams + 0x50) = 1.0; // Scale3D
        *(double*)(fsParams + 0x58) = 1.0;
        *(double*)(fsParams + 0x60) = 1.0;
        *(uint8_t*)(fsParams + 0x70) = 1;  // MultiplyWithRoot

        bool fsOk = callProcessEvent(s_gameplayStaticsCDO, s_finishSpawnFunc, fsParams);
        if (!fsOk)
            Hydro::logWarn("EngineAPI: FinishSpawningActor failed");
    }

    return sp.returnValue;
}

// --- UE-indexed player / actor lookups ---
// Instead of scanning GUObjectArray (~900k objects) to find the player
// or all actors of a class, call UE's own hash/index-backed UFunctions.
// These are how blueprint and engine code do the same operations, so they
// stay correct across engine versions without us maintaining anything.

void* getPlayerCharacter(int playerIndex) {
    if (!s_ready || !s_getPlayerCharacterFunc || !s_gameplayStaticsCDO) return nullptr;
    refreshWorld();
    if (!s_world) return nullptr;

    // Read the UFunction's ReturnValueOffset so we store the return pointer
    // at the right slot. GetPlayerCharacter's params are (WorldContext, PlayerIndex)
    // with return value at offset = RetOffset.
    uint16_t retOffset = getUFunctionRetOffset(s_getPlayerCharacterFunc);
    uint16_t parmsSize = getUFunctionParmsSize(s_getPlayerCharacterFunc);
    if (parmsSize > 256) return nullptr;

    alignas(16) uint8_t params[256] = {};
    void* worldCtx = findInWorldActor();
    Hydro::logInfo("getPlayerCharacter: findInWorldActor=%p s_world=%p s_ready=%d retOff=%u parmsSize=%u",
        worldCtx, s_world, s_ready ? 1 : 0, retOffset, parmsSize);
    if (!worldCtx) worldCtx = s_world;       // last-resort; UE 5.5 DMG returns null
    *(void**)(params + 0x00) = worldCtx;     // WorldContextObject - must be in-world
    *(int32_t*)(params + 0x08) = playerIndex; // PlayerIndex
    // Return slot is at retOffset; the called UFunction writes the ACharacter* there.

    bool peOk = callProcessEvent(s_gameplayStaticsCDO, s_getPlayerCharacterFunc, params);
    void* result = peOk ? *(void**)(params + retOffset) : nullptr;
    Hydro::logInfo("getPlayerCharacter: peOk=%d result=%p", peOk ? 1 : 0, result);
    return result;
}

void* getPlayerPawn(int playerIndex) {
    if (!s_ready || !s_getPlayerPawnFunc || !s_gameplayStaticsCDO) return nullptr;
    refreshWorld();
    if (!s_world) return nullptr;

    uint16_t retOffset = getUFunctionRetOffset(s_getPlayerPawnFunc);
    uint16_t parmsSize = getUFunctionParmsSize(s_getPlayerPawnFunc);
    if (parmsSize > 256) return nullptr;

    alignas(16) uint8_t params[256] = {};
    void* worldCtx = findInWorldActor();
    if (!worldCtx) worldCtx = s_world;
    *(void**)(params + 0x00) = worldCtx;
    *(int32_t*)(params + 0x08) = playerIndex;

    if (!callProcessEvent(s_gameplayStaticsCDO, s_getPlayerPawnFunc, params))
        return nullptr;

    return *(void**)(params + retOffset);
}

int getAllActorsOfClass(void* actorClass, void** outArray, int maxResults) {
    if (!s_ready || !s_getAllActorsOfClassFunc || !s_gameplayStaticsCDO) return 0;
    if (!actorClass || !outArray || maxResults <= 0) return 0;
    refreshWorld();
    if (!s_world) return 0;

    // GetAllActorsOfClass signature:
    //   static void GetAllActorsOfClass(UObject* WorldContext, TSubclassOf<AActor> ActorClass,
    //                                   TArray<AActor*>& OutActors);
    // Params layout (roughly):
    //   0x00: WorldContextObject (UObject*)
    //   0x08: ActorClass (UClass*)
    //   0x10: OutActors - a TArray (Data ptr, Num, Max) - we populate the struct's
    //         memory so UE can grow it; but the easier pattern used by UE internally
    //         is "pass a TArray by ref and UE appends". We allocate a temp array
    //         buffer and pre-seed the TArray to point at it.
    // Simplest safe approach: zero-initialized TArray; UE allocates the backing
    // buffer via its own TArray::Reserve on append. After the call, read the
    // Data/Num fields and copy the pointers into outArray, then free the
    // UE-allocated buffer - except freeing requires UE's allocator, which we
    // don't have direct access to. Small leak each call. Acceptable for MVP;
    // proper fix uses UE's FMemory_Free or ArrayDestructor later.

    alignas(16) uint8_t params[256] = {};
    *(void**)(params + 0x00) = s_world;
    *(void**)(params + 0x08) = actorClass;
    // Zero TArray at +0x10 (Data=nullptr, Num=0, Max=0) - UE will allocate.

    if (!callProcessEvent(s_gameplayStaticsCDO, s_getAllActorsOfClassFunc, params))
        return 0;

    void* data = *(void**)(params + 0x10);
    int32_t num = *(int32_t*)(params + 0x18);
    if (!data || num <= 0) return 0;

    int copied = 0;
    int limit = (num < maxResults) ? num : maxResults;
    for (int i = 0; i < limit; i++) {
        outArray[copied++] = ((void**)data)[i];
    }
    return copied;
}

// Reflection API (public wrappers around internal helpers)

void* getClass(void* obj) {
    if (!obj) return nullptr;
    void* cls = nullptr;
    safeReadPtr((uint8_t*)obj + UOBJ_CLASS, &cls);
    return cls;
}

void* findFunction(void* uclass, const wchar_t* funcName) {
    return findFunctionOnClass(uclass, funcName);
}

void* findProperty(void* uclass, const wchar_t* propName) {
    if (!uclass || !propName || !s_fnameConstructor) return nullptr;

    FName8 targetName = {};
    if (!safeConstructFName(&targetName, propName)) return nullptr;

    // Walk ChildProperties on this class, then up the SuperStruct chain.
    // Inherited properties (e.g. AActor::RootComponent on an ACharacter
    // instance) live on ancestor classes' FField list, not the subclass.
    // Use discovered chain-walk offsets (Layer 2) with hardcoded fallback -
    // Palworld's UE 5.1.x fork shifts these by +8 bytes vs. stock UE 5.5.
    int chOff   = (s_layout.childPropsOffset >= 0) ? s_layout.childPropsOffset : USTRUCT_CHILD_PROPS;
    int nextOff = (s_layout.fieldNextOffset  >= 0) ? s_layout.fieldNextOffset  : FFIELD_NEXT;
    int nameOff = (s_layout.fieldNameOffset  >= 0) ? s_layout.fieldNameOffset  : FFIELD_NAME;

    void* current = uclass;
    int classDepth = 0;
    while (current && classDepth < 32) {
        void* prop = nullptr;
        safeReadPtr((uint8_t*)current + chOff, &prop);

        int count = 0;
        while (prop && count < 500) {
            uint32_t nameIdx = 0;
            safeReadInt32((uint8_t*)prop + nameOff, (int32_t*)&nameIdx);

            if (nameIdx == targetName.comparisonIndex) {
                return prop;
            }

            void* next = nullptr;
            safeReadPtr((uint8_t*)prop + nextOff, &next);
            prop = next;
            count++;
        }

        void* super = nullptr;
        safeReadPtr((uint8_t*)current + (s_superOffset < 0 ? 0x40 : s_superOffset), &super);
        if (super == current) break;
        current = super;
        classDepth++;
    }
    return nullptr;
}

bool callFunction(void* obj, void* func, void* params) {
    return callProcessEvent(obj, func, params);
}

// Lazy-init s_gm for callers (e.g. PakLoader) running before initialize().
static void ensureGameModuleResolved() {
    if (!s_gm.base) s_gm = findGameModule();
}
uint8_t* getGameModuleBase() { ensureGameModuleResolved(); return s_gm.base; }
size_t   getGameModuleSize() { ensureGameModuleResolved(); return s_gm.size; }

// Built-in RawFn descriptors so adjacent modules can resolve("X") directly.
static void seedAndResolveRawFunctions() {
    using namespace ::Hydro::Engine::RawFn;
    registerFn("ProcessInternal", { Strategy::UFuncImpl,
        L"/Script/CoreUObject.Object:ExecuteUbergraph", 0 });
    registerFn("StaticLoadObject", { Strategy::StringRefAnchor,
        L"Failed to find object", 0 });
    resolveAllRegistered();
}

bool readPtr(void* addr, void** out) {
    return safeReadPtr(addr, out);
}

bool readInt32(void* addr, int32_t* out) {
    return safeReadInt32(addr, out);
}

// FField layout: +0x08 = FFieldClass* ClassPrivate
// FFieldClass layout: +0x00 = FName Name
constexpr int FFIELD_CLASS = 0x08;

uint32_t getPropertyTypeNameIndex(void* prop) {
    if (!prop) return 0;
    void* fieldClass = nullptr;
    safeReadPtr((uint8_t*)prop + FFIELD_CLASS, &fieldClass);
    if (!fieldClass) return 0;
    uint32_t nameIdx = 0;
    safeReadInt32((uint8_t*)fieldClass, (int32_t*)&nameIdx);
    return nameIdx;
}

int32_t getPropertyOffset(void* prop) {
    if (!prop) return -1;
    if (!s_layout.initialized) discoverPropertyLayout();
    int probeOff = (s_layout.offsetInternal >= 0) ? s_layout.offsetInternal : FPROP_OFFSET_INTERNAL;
    int32_t off = 0;
    safeReadInt32((uint8_t*)prop + probeOff, &off);
    return off;
}

int32_t getPropertyElementSize(void* prop) {
    if (!prop) return 0;
    if (!s_layout.initialized) discoverPropertyLayout();
    int probeOff = (s_layout.elementSize >= 0) ? s_layout.elementSize : FPROP_ELEMENT_SIZE;
    int32_t sz = 0;
    safeReadInt32((uint8_t*)prop + probeOff, &sz);
    return sz;
}

void* getNextProperty(void* prop) {
    if (!prop) return nullptr;
    int off = (s_layout.fieldNextOffset >= 0) ? s_layout.fieldNextOffset : FFIELD_NEXT;
    void* next = nullptr;
    safeReadPtr((uint8_t*)prop + off, &next);
    return next;
}

void* getChildProperties(void* ustruct) {
    if (!ustruct) return nullptr;
    int off = (s_layout.childPropsOffset >= 0) ? s_layout.childPropsOffset : USTRUCT_CHILD_PROPS;
    void* props = nullptr;
    safeReadPtr((uint8_t*)ustruct + off, &props);
    return props;
}

uint32_t makeFName(const wchar_t* str) {
    if (!str || !s_fnameConstructor) return 0;
    FName8 name = {};
    if (!safeConstructFName(&name, str)) return 0;
    return name.comparisonIndex;
}

// Property type names

static PropertyTypeNames s_propTypeNames = {};

bool initPropertyTypeNames() {
    if (!s_fnameConstructor) return false;
    if (s_propTypeNames.initialized) return true;

    s_propTypeNames.intProperty     = makeFName(L"IntProperty");
    s_propTypeNames.int64Property   = makeFName(L"Int64Property");
    s_propTypeNames.floatProperty   = makeFName(L"FloatProperty");
    s_propTypeNames.doubleProperty  = makeFName(L"DoubleProperty");
    s_propTypeNames.boolProperty    = makeFName(L"BoolProperty");
    s_propTypeNames.strProperty     = makeFName(L"StrProperty");
    s_propTypeNames.nameProperty    = makeFName(L"NameProperty");
    s_propTypeNames.textProperty    = makeFName(L"TextProperty");
    s_propTypeNames.objectProperty  = makeFName(L"ObjectProperty");
    s_propTypeNames.classProperty   = makeFName(L"ClassProperty");
    s_propTypeNames.structProperty  = makeFName(L"StructProperty");
    s_propTypeNames.arrayProperty   = makeFName(L"ArrayProperty");
    s_propTypeNames.enumProperty    = makeFName(L"EnumProperty");
    s_propTypeNames.byteProperty    = makeFName(L"ByteProperty");
    s_propTypeNames.uint32Property  = makeFName(L"UInt32Property");
    s_propTypeNames.uint16Property  = makeFName(L"UInt16Property");
    s_propTypeNames.int16Property   = makeFName(L"Int16Property");
    s_propTypeNames.int8Property    = makeFName(L"Int8Property");
    s_propTypeNames.initialized     = true;

    Hydro::logInfo("EngineAPI: Property type names initialized");
    return true;
}

const PropertyTypeNames& getPropertyTypeNames() {
    return s_propTypeNames;
}

// Object discovery (GUObjectArray iteration)
// Performance notes (see memory: hydrocore_find_performance):
//   Layer 1 - resolve className -> UClass* once, then compare obj->class
//             to that pointer instead of reading each obj->class->name.
//   Layer 2 - raw reads in the hot loop under one SEH block, not per-read.
//             The UOBJ_CLASS/UOBJ_NAME offsets are validated at bootstrap;
//             once validated, per-object reads at those offsets are safe.
//   Layer 3 - cache last found instance. A warm call only does ONE read
//             (instance->class) to validate, then returns instantly.
// Combined: cold call ~10ms, warm call ~nanoseconds vs. ~1s previously.

struct FindCacheEntry {
    uint32_t targetNameIdx = 0;
    void* uclass = nullptr;        // resolved UClass* (set on first successful scan)
    void* lastInstance = nullptr;  // most recent successful lookup
};
static std::unordered_map<std::wstring, FindCacheEntry> s_findCache;

/// Validate a cached instance pointer with a single SEH-wrapped read:
/// if instance->class still equals the cached UClass*, the cache is good.
/// Catches the common invalidation cases (GC'd object, class mismatch).
static bool validateCachedInstance(void* instance, void* expectedClass) {
    if (!instance || !expectedClass) return false;
    void* cls = nullptr;
    if (!safeReadPtr((uint8_t*)instance + UOBJ_CLASS, &cls)) return false;
    return cls == expectedClass;
}

/// Core scan. If targetClass is non-null, does a fast pointer-compare on
/// obj->class. Otherwise falls back to matching obj->class->nameIdx. On
/// match, writes the object pointer to *outFirst and/or appends to outAll.
/// Returns number of matches (or 1 and short-circuits if outAll is null).
///
/// Raw reads throughout - wrapped in a single __try that falls through on
/// access violation (e.g. torn chunk mid-GC), returning whatever matches
/// we'd found up to that point.
static int fastScan(uint32_t targetNameIdx, void* targetClass,
                    void** outFirst, void** outAll, int maxResults,
                    void** outResolvedClass) {
    if (!s_guObjectArray) return 0;
    int found = 0;
#ifdef _WIN32
    __try {
#endif
        void** chunkTable = *(void***)((uint8_t*)s_guObjectArray + FARRAY_OBJECTS);
        int32_t count = *(int32_t*)((uint8_t*)s_guObjectArray + FARRAY_NUM_ELEMS);
        if (!chunkTable || count <= 0) return 0;

        for (int32_t i = 0; i < count; i++) {
            int chunkIdx = i / CHUNK_SIZE;
            int withinChunk = i % CHUNK_SIZE;
            void* chunk = chunkTable[chunkIdx];
            if (!chunk) continue;

            uint8_t* item = (uint8_t*)chunk + (withinChunk * FUOBJ_SIZE);
            void* obj = *(void**)(item + FUOBJ_OBJECT);
            if (!obj) continue;

            void* cls = *(void**)((uint8_t*)obj + UOBJ_CLASS);
            if (!cls) continue;

            bool match;
            if (targetClass) {
                match = (cls == targetClass);
            } else {
                uint32_t classNameIdx = *(uint32_t*)((uint8_t*)cls + UOBJ_NAME);
                match = (classNameIdx == targetNameIdx);
            }
            if (!match) continue;

            // First match also fixes up the resolved UClass* for the cache.
            if (found == 0 && outResolvedClass && !*outResolvedClass)
                *outResolvedClass = cls;

            if (outFirst && found == 0) {
                *outFirst = obj;
                if (!outAll) return 1;  // findFirstOf - done
            }
            if (outAll && found < maxResults) outAll[found] = obj;
            found++;
            if (outAll && found >= maxResults) break;
        }
#ifdef _WIN32
    } __except (1) {
        // Fault mid-scan (rare: chunk torn by concurrent GC). Return what
        // we have so far - caller treats zero matches as "not found".
    }
#endif
    return found;
}

// Cached AActor UClass*. Resolved lazily via findObject once StaticFindObject
// is up; nullable (returns false from isActorSubclass until it resolves).
static void* s_actorClassPtr = nullptr;

// Does `uclass` descend from AActor? Walks the SuperStruct chain, bounded.
// Used by findFirstOf/findAllOf to route actor queries through UE's own
// O(instances) actor iteration (GetAllActorsOfClass) instead of scanning
// all ~77k live UObjects.
static bool isActorSubclass(void* uclass) {
    if (!uclass) return false;
    if (!s_actorClassPtr) {
        s_actorClassPtr = findObject(L"/Script/Engine.Actor");
        if (!s_actorClassPtr) return false;
    }
    int superOff = (s_superOffset > 0) ? s_superOffset : 0x40;
    void* cur = uclass;
    for (int depth = 0; depth < 64 && cur; depth++) {
        if (cur == s_actorClassPtr) return true;
        void* super = nullptr;
        if (!safeReadPtr((uint8_t*)cur + superOff, &super)) return false;
        if (super == cur) return false;   // self-loop guard
        cur = super;
    }
    return false;
}

// Walk GUObjectArray for a non-CDO actor instance suitable as a UFunction
// WorldContext. UE's GetWorldFromContextObject expects an object that's IN
// a world; UWorld* itself fails on some 5.5 builds (DMG empirically), and
// CDOs always fail. Used by spawn / GetPlayerCharacter / GetPlayerPawn.
// Returns nullptr if no in-world actor exists yet (very early init).
static void* findInWorldActor() {
    if (!s_guObjectArray) return nullptr;
    int32_t count = getObjectCount();
    for (int32_t i = 0; i < count; i++) {
        void* obj = getObjectAt(i);
        if (!obj) continue;
        uint32_t flags = 0;
        if (!safeReadInt32((uint8_t*)obj + UOBJ_FLAGS, (int32_t*)&flags)) continue;
        if (flags & 0x10) continue;            // RF_ClassDefaultObject
        void* cls = getObjClass(obj);
        if (!cls || !isActorSubclass(cls)) continue;
        return obj;
    }
    return nullptr;
}

void* findFirstOf(const wchar_t* className) {
    if (!s_guObjectArray || !s_fnameConstructor || !className) return nullptr;

    std::wstring key(className);
    auto& entry = s_findCache[key];

    // Warm path: we've seen this class before AND still have a recent
    // instance. Validate with one read; if it's still the same class,
    // return immediately (near-zero cost).
    if (entry.lastInstance &&
        validateCachedInstance(entry.lastInstance, entry.uclass))
        return entry.lastInstance;

    // Resolve the target FName index once per distinct className.
    if (entry.targetNameIdx == 0) {
        FName8 targetName = {};
        if (!safeConstructFName(&targetName, className)) return nullptr;
        entry.targetNameIdx = targetName.comparisonIndex;
    }

    // Actor-specialized fast path. Once we've resolved the UClass* (first
    // call does this via fastScan) and it descends from AActor, use UE's
    // own hash-backed iteration via UGameplayStatics::GetAllActorsOfClass -
    // same channel as GetPlayerCharacter. O(instances-in-world), not
    // O(all UObjects). Survives engine updates because it goes through the
    // public reflected UFUNCTION ABI, not internal struct layout.
    if (entry.uclass && isActorSubclass(entry.uclass)) {
        void* buf[1] = {};
        if (getAllActorsOfClass(entry.uclass, buf, 1) > 0) {
            entry.lastInstance = buf[0];
            return buf[0];
        }
        return nullptr;   // definitively no instances of this actor class
    }

    // Fall through to fastScan for:
    //   - first call on a class we haven't seen (uclass not yet resolved -
    //     cold scan also resolves it, so next call takes the actor path)
    //   - non-actor classes (UStruct, UEnum, UPackage, CDOs, UFunctions, ...)
    void* first = nullptr;
    void* resolved = entry.uclass;
    fastScan(entry.targetNameIdx, entry.uclass, &first, nullptr, 0, &resolved);
    if (resolved && !entry.uclass) entry.uclass = resolved;
    entry.lastInstance = first;
    return first;
}

int findAllOf(const wchar_t* className, void** outArray, int maxResults) {
    if (!s_guObjectArray || !s_fnameConstructor || !className || !outArray || maxResults <= 0)
        return 0;

    std::wstring key(className);
    auto& entry = s_findCache[key];

    if (entry.targetNameIdx == 0) {
        FName8 targetName = {};
        if (!safeConstructFName(&targetName, className)) return 0;
        entry.targetNameIdx = targetName.comparisonIndex;
    }

    // Actor fast path - see findFirstOf rationale.
    if (entry.uclass && isActorSubclass(entry.uclass)) {
        int found = getAllActorsOfClass(entry.uclass, outArray, maxResults);
        if (found > 0) entry.lastInstance = outArray[0];
        return found;
    }

    void* resolved = entry.uclass;
    int found = fastScan(entry.targetNameIdx, entry.uclass,
                         nullptr, outArray, maxResults, &resolved);
    if (resolved && !entry.uclass) entry.uclass = resolved;
    if (found > 0) entry.lastInstance = outArray[0];
    return found;
}

void* getWorld() {
    // Fast path: dereference cached GWorld pointer
    if (s_gWorldPtr) {
        void* w = nullptr;
        if (safeReadPtr(s_gWorldPtr, &w) && w)
            return w;
    }
    return s_world;
}

int getNetMode() {
    // UWorld::GetNetMode is a Blueprint-callable UFUNCTION returning ENetMode
    // (uint8). Call via reflection so we never bind to engine-private offsets:
    // the property chain walk gives us ParmsSize and ReturnValueOffset, then
    // ProcessEvent does the work.
    void* world = getWorld();
    if (!world) return 0;
    void* cls = getClass(world);
    if (!cls) return 0;

    static void* s_getNetModeFunc = nullptr;
    if (!s_getNetModeFunc) {
        s_getNetModeFunc = findFunction(cls, L"GetNetMode");
    }
    if (!s_getNetModeFunc) return 0;

    uint16_t parmsSize = getUFunctionParmsSize(s_getNetModeFunc);
    uint16_t retOff    = getUFunctionRetOffset(s_getNetModeFunc);
    if (parmsSize == 0 || retOff == 0xFFFF) return 0;

    std::vector<uint8_t> params(parmsSize, 0);
    if (!callFunction(world, s_getNetModeFunc, params.data())) return 0;

    // ENetMode is TEnumAsByte<ENetMode> - single byte at the return slot.
    if (retOff >= parmsSize) return 0;
    return static_cast<int>(params[retOff]);
}

// FProperty::PropertyFlags fallback (uint64 at +0x38 in stock UE 5.5).
// The real offset is discovered at runtime - see s_layout / discoverPropertyLayout.
constexpr int FPROP_FLAGS = 0x38;

// -- Layer 2: reflection-driven FProperty layout discovery ---
// Stock UE 5.5 has FProperty::Offset_Internal at +0x44, ElementSize at +0x34,
// PropertyFlags at +0x38 - but Pocketpair's Palworld fork (UE 5.1.x base) and
// other forked-engine hosts shift these. Hardcoding them gives RootComponent
// reads as nil, K2_GetActorLocation EXCEPTION_ACCESS_VIOLATION on host, etc.
// We probe the layout once at first use against a known-good anchor:
// AActor::PrimaryActorTick (a StructProperty wrapping FTickFunction), with
// AActor::RootComponent (TObjectPtr, ElementSize=8) and AActor::Tags
// (TArray<FName>, ElementSize=16) as cross-checks. The candidate offset
// triple that yields sane values across all three properties wins.
// Discovered values live in s_layout (declared near the top, alongside the
// scan cache that persists them). Hardcoded constants stay as fallbacks if
// discovery hasn't run or fails. The accessors read s_layout.* with the
// hardcoded constants as fallback.

// Read a uint64 with SEH guard.
static bool safeReadU64(void* addr, uint64_t* out) {
    HYDRO_SEH_TRY(*out = *(uint64_t*)addr);
}

// Walk a candidate property chain manually using raw probe offsets - used
// only by discoverPropertyLayout. Returns the count of FName indices in the
// chain that match anything in `expectedNames`. SEH-guarded so a wrong
// (chOff, nextOff, nameOff) triple producing garbage just scores 0.
static int probePropertyChainScore(void* uclass, int chOff, int nextOff, int nameOff,
                                   const uint32_t* expectedNames, int expectedCount,
                                   int maxWalk = 200) {
    if (!uclass) return 0;
    int hits = 0;
    void* prop = nullptr;
    if (!safeReadPtr((uint8_t*)uclass + chOff, &prop) || !prop) return 0;
    // First-element pointer must be heap (not in module / not low-magic).
    if ((uint8_t*)prop >= s_gm.base && (uint8_t*)prop < s_gm.base + s_gm.size) return 0;
    if ((uintptr_t)prop < 0x1000) return 0;

    int count = 0;
    while (prop && count < maxWalk) {
        uint32_t nameIdx = 0;
        if (!safeReadInt32((uint8_t*)prop + nameOff, (int32_t*)&nameIdx)) break;
        if (nameIdx == 0 || nameIdx > 0x7FFFFFFF) break;
        for (int i = 0; i < expectedCount; i++) {
            if (nameIdx == expectedNames[i]) { hits++; break; }
        }
        void* next = nullptr;
        if (!safeReadPtr((uint8_t*)prop + nextOff, &next)) break;
        if (next == prop) break;       // self-loop guard
        prop = next;
        count++;
    }
    return hits;
}

// Scan GUObjectArray directly for AActor's UClass. Doesn't rely on
// StaticFindObject (which is unreliable on Palworld - returns wrong objects
// for /Script/ paths). Matches by:
//   obj's class name FName == "Class"  AND  obj's own name FName == "Actor"
// This uniquely identifies AActor's UClass even on forked engines.
static void* findActorClassViaScan() {
    if (!s_guObjectArray) {
        Hydro::logWarn("EngineAPI: findActorClassViaScan: GUObjectArray not ready");
        return nullptr;
    }

    FName8 classFn = {}, actorFn = {};
    if (!safeConstructFName(&classFn, L"Class") || !safeConstructFName(&actorFn, L"Actor")) {
        Hydro::logWarn("EngineAPI: findActorClassViaScan: FName construction failed");
        return nullptr;
    }
    uint32_t classIdx = classFn.comparisonIndex;
    uint32_t actorIdx = actorFn.comparisonIndex;
    Hydro::logInfo("EngineAPI: findActorClassViaScan: class='Class'(%u) target='Actor'(%u)",
                   classIdx, actorIdx);
    if (!classIdx || !actorIdx) return nullptr;

    int32_t count = 0;
    void** chunkTable = nullptr;
    int classMatches = 0;     // total UClass-typed objects seen
    int nameMatches  = 0;     // total objects whose own name == "Actor"
    int faults = 0;           // how many bad-chunk reads we tolerated
    void* result = nullptr;

    // Read array header under one SEH guard.
    if (!safeReadPtr((uint8_t*)s_guObjectArray + FARRAY_OBJECTS, (void**)&chunkTable)) {
        Hydro::logWarn("EngineAPI: findActorClassViaScan: chunk table unreadable");
        return nullptr;
    }
    if (!safeReadInt32((uint8_t*)s_guObjectArray + FARRAY_NUM_ELEMS, &count) || count <= 0) {
        Hydro::logWarn("EngineAPI: findActorClassViaScan: count unreadable");
        return nullptr;
    }
    if (!chunkTable) return nullptr;

    // Per-iteration SEH guards so one torn chunk doesn't kill the scan.
    for (int32_t i = 0; i < count; i++) {
        int chunkIdx    = i / CHUNK_SIZE;
        int withinChunk = i % CHUNK_SIZE;

        void* chunk = nullptr;
        if (!safeReadPtr((uint8_t*)&chunkTable[chunkIdx], &chunk) || !chunk) {
            faults++;
            continue;
        }
        uint8_t* item = (uint8_t*)chunk + (withinChunk * FUOBJ_SIZE);
        void* obj = nullptr;
        if (!safeReadPtr(item + FUOBJ_OBJECT, &obj) || !obj) continue;

        void* cls = nullptr;
        if (!safeReadPtr((uint8_t*)obj + UOBJ_CLASS, &cls) || !cls) continue;

        uint32_t selfNameIdx = 0, clsNameIdx = 0;
        if (!safeReadInt32((uint8_t*)obj + UOBJ_NAME, (int32_t*)&selfNameIdx)) continue;
        if (!safeReadInt32((uint8_t*)cls + UOBJ_NAME, (int32_t*)&clsNameIdx)) continue;

        if (clsNameIdx == classIdx) classMatches++;
        if (selfNameIdx == actorIdx) {
            nameMatches++;
            if (nameMatches <= 8) {
                Hydro::logInfo("  Actor-named obj at %p, class=%p clsName=%u",
                               obj, cls, clsNameIdx);
            }
        }

        if (clsNameIdx == classIdx && selfNameIdx == actorIdx) {
            result = obj;
            break;
        }
    }

    Hydro::logInfo("EngineAPI: findActorClassViaScan: scanned=%d classMatches=%d "
                   "nameMatches=%d chunkFaults=%d result=%p",
                   count, classMatches, nameMatches, faults, result);
    return result;
}

bool discoverPropertyLayout() {
    if (s_layout.initialized) return s_layout.succeeded;
    s_layout.initialized = true;

    // Resolve AActor's UClass by direct GUObjectArray scan rather than
    // StaticFindObject - Palworld's findObject path returns the wrong
    // object (UPackage /Script/AssetRegistry) for /Script/Engine.Actor.
    void* actorClass = findActorClassViaScan();
    if (!actorClass) {
        // Fallback: try the (possibly-broken) findObject as a last resort.
        actorClass = findObject(L"/Script/Engine.Actor");
    }
    if (!actorClass) {
        Hydro::logWarn("EngineAPI: Layer 2 probe deferred - AActor UClass not yet in GUObjectArray");
        s_layout.initialized = false;   // allow retry on next call
        return false;
    }

    // -- Stage A: pre-resolve known AActor property name FName indices --
    // We can construct FNames at this point (FName ctor was discovered
    // during pattern-scan). These names are present on AActor in every UE5
    // version we know of.
    static const wchar_t* kKnownActorProps[] = {
        L"PrimaryActorTick",
        L"RootComponent",
        L"Tags",
        L"Owner",
        L"Instigator",
        L"Layer",
        L"NetUpdateFrequency",
        L"ReplicatedMovement",
        L"InstanceComponents",
        L"BlueprintCreatedComponents",
        L"AttachmentReplication",
    };
    constexpr int kKnownCount = sizeof(kKnownActorProps) / sizeof(kKnownActorProps[0]);
    uint32_t expectedNames[kKnownCount] = {};
    int expectedReady = 0;
    for (int i = 0; i < kKnownCount; i++) {
        FName8 fn = {};
        if (safeConstructFName(&fn, kKnownActorProps[i]) && fn.comparisonIndex != 0) {
            expectedNames[expectedReady++] = fn.comparisonIndex;
        }
    }
    if (expectedReady < 4) {
        Hydro::logWarn("EngineAPI: Layer 2 probe failed - couldn't resolve AActor property name FNames");
        return false;
    }
    // Diagnostic: dump expected names and their resolved FName indices.
    Hydro::logInfo("EngineAPI: Layer 2 expected names (%d resolved):", expectedReady);
    for (int i = 0; i < expectedReady; i++) {
        Hydro::logInfo("  '%ls' = idx %u", kKnownActorProps[i], expectedNames[i]);
    }

    // Diagnostic: validate actorClass identity. Read its UOBJ_NAME (FName
    // index at +0x18) and the FName index of its OWN class via UOBJ_CLASS
    // (+0x10) -> that class' name. AActor's UClass should report name="Actor"
    // and its meta-class should report name="Class".
    {
        uint32_t selfNameIdx = 0;
        safeReadInt32((uint8_t*)actorClass + UOBJ_NAME, (int32_t*)&selfNameIdx);
        std::string selfName = getNameString(selfNameIdx);
        void* metaCls = nullptr;
        safeReadPtr((uint8_t*)actorClass + UOBJ_CLASS, &metaCls);
        uint32_t metaNameIdx = 0;
        if (metaCls) safeReadInt32((uint8_t*)metaCls + UOBJ_NAME, (int32_t*)&metaNameIdx);
        std::string metaName = metaCls ? getNameString(metaNameIdx) : std::string("<null>");
        Hydro::logInfo("EngineAPI: actorClass identity check: self='%s' (idx=%u) meta=%p '%s' (idx=%u)",
                       selfName.c_str(), selfNameIdx, metaCls, metaName.c_str(), metaNameIdx);
    }

    // Diagnostic: dump 8-byte slots from 0x00 through 0x180 - UClass on UE5
    // is hundreds of bytes (vtable + UObject base + UField + UStruct + UClass
    // members). We need to see the full picture to find ChildProperties.
    Hydro::logInfo("EngineAPI: Layer 2 actorClass=%p; full UClass slot dump:", actorClass);
    for (int off = 0x00; off <= 0x180; off += 8) {
        void* p = nullptr;
        if (!safeReadPtr((uint8_t*)actorClass + off, &p)) {
            Hydro::logInfo("  +0x%02X: <unreadable>", off);
            continue;
        }
        uintptr_t v = (uintptr_t)p;
        bool inMod = ((uint8_t*)p >= s_gm.base && (uint8_t*)p < s_gm.base + s_gm.size);
        // Real Windows x64 user-mode heap pointers live above ~0x10000000000
        // and have nonzero bytes throughout - sentinels typically have
        // (low32==0 || high32==0).
        uint32_t lo = (uint32_t)(v & 0xFFFFFFFFULL);
        uint32_t hi = (uint32_t)(v >> 32);
        bool heapLike = !inMod && hi != 0 && lo != 0 && hi < 0x10000U;
        const char* tag = inMod ? " <module>" : (heapLike ? " <heap>" : "");
        Hydro::logInfo("  +0x%02X: %p%s", off, p, tag);
    }

    // -- Stage B: probe (USTRUCT_CHILD_PROPS, FFIELD_NEXT, FFIELD_NAME) --
    // Walk AActor's child chain with each candidate triple; the combo with
    // the most matches against `expectedNames` wins. Stock UE 5.5 lands on
    // (0x50, 0x18, 0x20). Palworld's fork shifts these by +8 bytes ->
    // (0x58, 0x20, 0x28). Other forks may differ - extend candidates as
    // new hosts surface.
    static const int kChildProps[] = { 0x50, 0x58, 0x48, 0x60, 0x40 };
    static const int kFieldNext[]  = { 0x18, 0x20, 0x28, 0x10, 0x30 };
    static const int kFieldName[]  = { 0x20, 0x28, 0x30, 0x18, 0x38 };

    int bestScore = 0;
    int bestCh = -1, bestNext = -1, bestName = -1;
    for (int ch : kChildProps) {
        // Validate first-element pointer once per CHILD_PROPS candidate so
        // diagnostic output is per-candidate, not per-triple.
        void* firstProp = nullptr;
        bool readOk = safeReadPtr((uint8_t*)actorClass + ch, &firstProp);
        if (!readOk || !firstProp) continue;
        if ((uint8_t*)firstProp >= s_gm.base && (uint8_t*)firstProp < s_gm.base + s_gm.size) continue;
        if ((uintptr_t)firstProp < 0x1000) continue;
        Hydro::logInfo("EngineAPI: Layer 2 trying CH=0x%X firstProp=%p", ch, firstProp);

        for (int nm : kFieldName) {
            // Probe the first-element name at each candidate name offset to
            // surface what's actually stored there - even before counting hits.
            uint32_t firstIdx = 0;
            if (safeReadInt32((uint8_t*)firstProp + nm, (int32_t*)&firstIdx) && firstIdx != 0) {
                std::string fname = (firstIdx <= 0x7FFFFFFF) ? getNameString(firstIdx) : std::string("<garbage>");
                Hydro::logInfo("  CH=0x%X NAME=0x%X -> firstIdx=%u ('%s')", ch, nm, firstIdx, fname.c_str());
            }
        }

        for (int nx : kFieldNext) {
            for (int nm : kFieldName) {
                if (nx == nm) continue;   // Next and Name can't share an offset
                int score = probePropertyChainScore(actorClass, ch, nx, nm,
                                                    expectedNames, expectedReady);
                if (score > bestScore) {
                    bestScore = score;
                    bestCh = ch; bestNext = nx; bestName = nm;
                }
            }
        }
    }

    // Need at least 3 known names to land - anything less risks false match.
    if (bestScore < 3) {
        Hydro::logWarn("EngineAPI: Layer 2 chain-walk probe FAILED - best score %d "
                       "(needed >= 3). FField/UStruct layout may have shifted "
                       "in an unexpected way; extend candidate lists.", bestScore);
        return false;
    }

    s_layout.childPropsOffset = bestCh;
    s_layout.fieldNextOffset  = bestNext;
    s_layout.fieldNameOffset  = bestName;

    Hydro::logInfo("EngineAPI: Layer 2 chain-walk DISCOVERED - "
                   "CHILD_PROPS=0x%X FF_NEXT=0x%X FF_NAME=0x%X (score=%d/%d)",
                   bestCh, bestNext, bestName, bestScore, expectedReady);
    if (bestCh != USTRUCT_CHILD_PROPS || bestNext != FFIELD_NEXT || bestName != FFIELD_NAME)
        Hydro::logInfo("EngineAPI: chain-walk offsets DIFFER from stock UE 5.5 "
                       "(stock: CH=0x%X NEXT=0x%X NAME=0x%X)",
                       USTRUCT_CHILD_PROPS, FFIELD_NEXT, FFIELD_NAME);

    // -- Stage C: with chain offsets in hand, find anchor properties --
    // findProperty now uses s_layout.{childProps,fieldNext,fieldName} - so
    // this works even if the host has shifted offsets.
    void* tickProp = findProperty(actorClass, L"PrimaryActorTick");
    void* rootProp = findProperty(actorClass, L"RootComponent");
    void* tagsProp = findProperty(actorClass, L"Tags");
    if (!tickProp) {
        Hydro::logWarn("EngineAPI: Layer 2 stage C failed - PrimaryActorTick not findable "
                       "even with discovered chain-walk offsets");
        // Mark chain-walk as discovered but FProperty internals as not.
        // Subsequent code can still walk chains, just can't read offsets.
        return false;
    }

    // -- Stage D: probe FPROP_OFFSET_INTERNAL --
    // Each prop's offset must be 4-byte aligned, < 8KB, distinct between
    // PrimaryActorTick / RootComponent / Tags (they live at different
    // positions in AActor).
    static const int kOffsetInternal[] = { 0x44, 0x4C, 0x40, 0x48, 0x50, 0x54, 0x3C };
    int32_t bestOI = -1;
    for (int off : kOffsetInternal) {
        int32_t vTick = 0, vRoot = 0, vTags = 0;
        if (!safeReadInt32((uint8_t*)tickProp + off, &vTick)) continue;
        auto sane = [](int32_t v) { return v >= 0 && v < 8192 && (v & 3) == 0; };
        if (!sane(vTick)) continue;
        bool ok = true;
        if (rootProp) {
            if (!safeReadInt32((uint8_t*)rootProp + off, &vRoot) || !sane(vRoot) || vRoot == vTick) ok = false;
        }
        if (ok && tagsProp) {
            if (!safeReadInt32((uint8_t*)tagsProp + off, &vTags) || !sane(vTags) || vTags == vTick) ok = false;
        }
        if (!ok) continue;
        bestOI = off;
        break;
    }

    // -- Stage E: probe FPROP_ELEMENT_SIZE --
    // RootComponent is TObjectPtr (8 bytes), Tags is TArray<FName> (16 bytes).
    static const int kElementSize[] = { 0x34, 0x3C, 0x40, 0x38, 0x30, 0x44 };
    int32_t bestES = -1;
    for (int off : kElementSize) {
        int32_t vTick = 0;
        if (!safeReadInt32((uint8_t*)tickProp + off, &vTick)) continue;
        if (vTick <= 0 || vTick > 1024) continue;
        bool ok = true;
        if (rootProp) {
            int32_t v = 0;
            if (!safeReadInt32((uint8_t*)rootProp + off, &v) || v != 8) ok = false;
        }
        if (ok && tagsProp) {
            int32_t v = 0;
            if (!safeReadInt32((uint8_t*)tagsProp + off, &v) || v != 16) ok = false;
        }
        if (!ok) continue;
        bestES = off;
        break;
    }

    // -- Stage F: probe FPROP_FLAGS (uint64) --
    // Strict validation: AActor's member properties (PrimaryActorTick, Tags,
    // RootComponent) all carry CPF_Edit (bit 0). Parameters carry CPF_Parm
    // (bit 7), and these are NOT parameters so CPF_Parm must be CLEAR.
    // We also require the top 16 bits to be clear since real CPF_* defines
    // fit comfortably below 2^48. A loose nonzero+top-byte-clear check is
    // not enough - Palworld's FProperty has another uint64 a few bytes off
    // that satisfies the loose check but isn't the flags field.
    static const int kFlags[] = { 0x38, 0x40, 0x30, 0x48, 0x44 };
    constexpr uint64_t CPF_Edit_local         = 0x0000000000000001ULL;
    constexpr uint64_t CPF_Parm_local         = 0x0000000000000080ULL;
    constexpr uint64_t CPF_NoClear_local      = 0x0000000000100000ULL;
    auto looksLikePropertyFlags = [&](uint64_t v) {
        if (v == 0 || v == ~0ULL) return false;
        // CPF_* defines extend up to ~bit 52 (CPF_HasGetValueTypeHash etc.),
        // so only the top byte (bits 56-63) needs to be clear. Earlier I
        // checked top 16 bits clear which incorrectly rejected real flag
        // values with CPF_NativeAccessSpecifierPublic (bit 50) set.
        if ((v >> 56) != 0) return false;
        if (!(v & CPF_Edit_local)) return false; // member must have Edit/Visible bit
        if (v & CPF_Parm_local) return false;    // member is NOT a parameter
        return true;
    };
    // DIAGNOSTIC: dump every candidate FLAGS offset's value for all three
    // anchors so we can see what's there and tune validation.
    Hydro::logInfo("EngineAPI: FLAGS candidate dump:");
    for (int off : kFlags) {
        uint64_t vTick = 0, vRoot = 0, vTags = 0;
        bool tickOk = safeReadU64((uint8_t*)tickProp + off, &vTick);
        bool rootOk = rootProp && safeReadU64((uint8_t*)rootProp + off, &vRoot);
        bool tagsOk = tagsProp && safeReadU64((uint8_t*)tagsProp + off, &vTags);
        Hydro::logInfo("  +0x%02X: Tick=%s%llX Root=%s%llX Tags=%s%llX",
            off,
            tickOk ? "0x" : "<r:", (unsigned long long)vTick,
            rootOk ? "0x" : "<r:", (unsigned long long)vRoot,
            tagsOk ? "0x" : "<r:", (unsigned long long)vTags);
    }

    // Cross-validation across three anchor properties. Real PropertyFlags
    // exhibit a rich, diverse bit pattern in the low 16 (multiple CPF bits
    // per property) and differ between properties (since each property has
    // its own CPF combination). Wrong offsets read pointers, hashes, or
    // adjacent fields that share a sparse, uniform pattern across props.
    auto popcount16 = [](uint64_t v) {
        int n = 0;
        for (int b = 0; b < 16; b++) if (v & (1ULL << b)) n++;
        return n;
    };
    auto validFlags = [](uint64_t v) {
        if (v == 0 || v == ~0ULL) return false;
        if ((v >> 56) != 0) return false;     // top byte clear
        if (v & CPF_Parm_local) return false; // member, not parameter
        return true;
    };
    int32_t bestFL = -1;
    for (int off : kFlags) {
        uint64_t vTick = 0, vRoot = 0, vTags = 0;
        if (!safeReadU64((uint8_t*)tickProp + off, &vTick) || !validFlags(vTick)) continue;
        bool ok = true;
        if (rootProp) {
            if (!safeReadU64((uint8_t*)rootProp + off, &vRoot) || !validFlags(vRoot)) ok = false;
        }
        if (ok && tagsProp) {
            if (!safeReadU64((uint8_t*)tagsProp + off, &vTags) || !validFlags(vTags)) ok = false;
        }
        if (!ok) continue;

        // Diversity: real PropertyFlags differ across properties. If any
        // two anchors share a value at this offset, it's reading something
        // not-property-specific (e.g. RepNotifyFunc=null packed similarly).
        if ((rootProp && vTick == vRoot) || (tagsProp && vTick == vTags) ||
            (rootProp && tagsProp && vRoot == vTags)) continue;

        // Richness: properties typically carry several CPF bits in the low
        // 16 (Edit, BlueprintVisible, BlueprintReadOnly, ReplicatedUsing,
        // ReferenceParm, etc.). Wrong offsets show 1-2 bits at most.
        int richness = popcount16(vTick) + popcount16(vRoot) + popcount16(vTags);
        if (richness < 6) continue;

        bestFL = off;
        Hydro::logInfo("EngineAPI:   FLAGS=0x%X validated (richness=%d) via Tick=0x%llX Root=0x%llX Tags=0x%llX",
                       off, richness,
                       (unsigned long long)vTick, (unsigned long long)vRoot, (unsigned long long)vTags);
        break;
    }

    s_layout.offsetInternal = bestOI;
    s_layout.elementSize    = bestES;
    s_layout.flags          = bestFL;
    s_layout.succeeded      = (bestOI >= 0 && bestES >= 0 && bestFL >= 0);

    Hydro::logInfo("EngineAPI: Layer 2 FProperty layout %s - "
                   "OFFSET_INTERNAL=0x%X ELEMENT_SIZE=0x%X FLAGS=0x%X",
                   s_layout.succeeded ? "DISCOVERED" : "PARTIAL",
                   bestOI, bestES, bestFL);
    if (bestOI != FPROP_OFFSET_INTERNAL || bestES != FPROP_ELEMENT_SIZE || bestFL != FPROP_FLAGS)
        Hydro::logInfo("EngineAPI: FProperty offsets DIFFER from stock UE 5.5 "
                       "(stock: OI=0x%X ES=0x%X FL=0x%X)",
                       FPROP_OFFSET_INTERNAL, FPROP_ELEMENT_SIZE, FPROP_FLAGS);
    return s_layout.succeeded;
}

uint64_t getPropertyFlags(void* prop) {
    if (!prop) return 0;
    if (!s_layout.initialized) discoverPropertyLayout();
    int off = (s_layout.flags >= 0) ? s_layout.flags : FPROP_FLAGS;
    uint64_t flags = 0;
    safeReadU64((uint8_t*)prop + off, &flags);
    return flags;
}

// -- FNamePool direct reading --
// Fastest name resolution: reads directly from the engine's FNamePool
// using pointer arithmetic. No function calls, no ProcessEvent.
// Discovery: the FName constructor (already found) references the pool
// via LEA instructions in its prologue. We resolve those, then validate
// by checking that entry 0 reads as "None".
// FNamePool layout (UE5):
//   FNameEntryAllocator:
//     +0x00: FRWLock Lock (8 bytes on Windows - SRWLOCK)
//     +0x08: uint32 CurrentBlock
//     +0x0C: uint32 CurrentByteCursor
//     +0x10: uint8* Blocks[FNameMaxBlocks]  (pointers to memory blocks)
// FNameEntry layout:
//     +0x00: uint16 Header  (bit 0 = bIsWide, bits [6..15] = Len)
//     +0x02: char/wchar_t Name[Len]
// ComparisonIndex encoding:
//     Block = idx >> FNameBlockOffsetBits  (typically 16)
//     Offset = (idx & mask) * stride       (stride typically 2)

// s_fnamePool and s_poolBlocksOffset declared early (near s_poolReady)

// Try to validate a candidate pool address with a specific blocks offset.
// Checks if entry at ComparisonIndex 0 is "None".
static bool tryValidatePool(void* candidate, int blocksOffset) {
    // Read Blocks[0] pointer
    void* block0 = nullptr;
    if (!safeReadPtr((uint8_t*)candidate + blocksOffset, &block0) || !block0)
        return false;

    // Read first 8 bytes: header (2) + "None" (4) + padding
    uint8_t data[8] = {};
#ifdef _WIN32
    __try {
        memcpy(data, block0, 8);
    } __except(1) { return false; }
#else
    memcpy(data, block0, 8);
#endif

    uint16_t header = *(uint16_t*)data;
    bool isWide = header & 1;
    int len = (header >> 6) & 0x3FF;

    if (isWide || len != 4) return false;
    if (memcmp(data + 2, "None", 4) != 0) return false;

    return true;
}

// Read raw FNameEntry data from the pool into a caller-provided buffer.
// Returns the string length, or 0 on failure. isWide set if wide chars.
static int readPoolEntryRaw(uint32_t comparisonIndex, char* buf, int bufSize, bool* isWide) {
    int blockIdx = comparisonIndex >> 16;
    int entryOffset = (comparisonIndex & 0xFFFF) * 2; // stride = 2

    void* blockPtr = nullptr;
    if (!safeReadPtr((uint8_t*)s_fnamePool + s_poolBlocksOffset + blockIdx * 8, &blockPtr) || !blockPtr)
        return 0;

    uint16_t header = 0;
    if (!safeReadInt32((uint8_t*)blockPtr + entryOffset, (int32_t*)&header))
        return 0;
    // Only lower 16 bits are the header
    header &= 0xFFFF;

    *isWide = header & 1;
    int len = (header >> 6) & 0x3FF;
    if (len <= 0 || len > 1024 || len > bufSize) return 0;

    uint8_t* namePtr = (uint8_t*)blockPtr + entryOffset + 2;
    int bytesToRead = *isWide ? len * 2 : len;

#ifdef _WIN32
    __try {
        memcpy(buf, namePtr, bytesToRead);
    } __except(1) { return 0; }
#else
    memcpy(buf, namePtr, bytesToRead);
#endif

    return len;
}

// Read a name string directly from the FNamePool.
static std::string readFromPool(uint32_t comparisonIndex) {
    if (!s_poolReady || !s_fnamePool) return {};

    char buf[2048];
    bool isWide = false;
    int len = readPoolEntryRaw(comparisonIndex, buf, sizeof(buf) / 2, &isWide);
    if (len <= 0) return {};

    if (isWide) {
        std::string result;
        result.reserve(len);
        const wchar_t* wstr = (const wchar_t*)buf;
        for (int i = 0; i < len; i++) result += (char)(wstr[i] & 0x7F);
        return result;
    } else {
        return std::string(buf, len);
    }
}

static bool discoverFNamePool() {
    static const int kBlocksOffsets[] = { 0x10, 0x18, 0x08, 0x20, 0x28, 0x30, 0x38, 0x40 };

    // -- Strategy 1: walk the FName constructor's call tree (2 levels deep) --
    // The pool global is accessed via LEA or MOV [rip+disp] from a sub-function
    // of the constructor (not from the constructor itself, which just hashes).
    if (s_fnameConstructor) {
        uint8_t* func = (uint8_t*)s_fnameConstructor;

        // Collect unique functions: constructor + level-1 callees + level-2 callees
        constexpr int MAX_TARGETS = 64;
        uint8_t* targets[MAX_TARGETS];
        int numTargets = 0;
        targets[numTargets++] = func;

        // Level 1: scan constructor body (2048 bytes) for E8 CALL targets
        int level1End = 1;
        for (int i = 0; i < 2048 && numTargets < MAX_TARGETS; i++) {
            if (func[i] == 0xCC && i > 0 && func[i-1] == 0xCC) break;
            if (func[i] == 0xE8) {
                int32_t rel = *(int32_t*)(func + i + 1);
                uint8_t* t = func + i + 5 + rel;
                if (t >= s_gm.base && t < s_gm.base + s_gm.size) {
                    bool dup = false;
                    for (int j = 0; j < numTargets; j++) if (targets[j] == t) { dup = true; break; }
                    if (!dup) targets[numTargets++] = t;
                }
                i += 4;
            }
        }
        level1End = numTargets;

        // Level 2: scan first 512 bytes of each level-1 callee
        for (int k = 1; k < level1End && numTargets < MAX_TARGETS; k++) {
            uint8_t* callee = targets[k];
            for (int i = 0; i < 512 && numTargets < MAX_TARGETS; i++) {
                if (callee[i] == 0xCC && i > 0 && callee[i-1] == 0xCC) break;
                if (callee[i] == 0xE8) {
                    int32_t rel = *(int32_t*)(callee + i + 1);
                    uint8_t* t = callee + i + 5 + rel;
                    if (t >= s_gm.base && t < s_gm.base + s_gm.size) {
                        bool dup = false;
                        for (int j = 0; j < numTargets; j++) if (targets[j] == t) { dup = true; break; }
                        if (!dup) targets[numTargets++] = t;
                    }
                    i += 4;
                }
            }
        }

        Hydro::logInfo("EngineAPI: FNamePool: scanning %d functions (2-level call tree)", numTargets);

        // For each function, scan for LEA or MOV [rip+disp32] to a pool candidate
        for (int t = 0; t < numTargets; t++) {
            uint8_t* scan = targets[t];
            for (int i = 0; i + 7 < 512; i++) {
                uint8_t rex = scan[i];
                if (rex != 0x48 && rex != 0x4C && rex != 0x4D) continue;
                if (scan[i+1] != 0x8D && scan[i+1] != 0x8B) continue;  // LEA or MOV
                if ((scan[i+2] & 0xC7) != 0x05) continue;

                int32_t disp = *(int32_t*)(scan + i + 3);
                void* resolved = scan + i + 7 + disp;
                if ((uint8_t*)resolved < s_gm.base || (uint8_t*)resolved >= s_gm.base + s_gm.size)
                    continue;

                bool isMov = (scan[i+1] == 0x8B);
                void* candidate = resolved;

                // MOV [rip+disp]: resolved is a pointer-to-pool variable; dereference it
                if (isMov) {
                    void* ptr = nullptr;
                    if (!safeReadPtr(resolved, &ptr) || !ptr) continue;
                    candidate = ptr;
                }

                for (int off : kBlocksOffsets) {
                    if (tryValidatePool(candidate, off)) {
                        s_fnamePool = candidate;
                        s_poolBlocksOffset = off;
                        s_poolReady = true;
                        Hydro::logInfo("EngineAPI: FNamePool at %p (blocksOffset=0x%X) via %s in func[%d] exe+0x%zX",
                            candidate, off, isMov ? "MOV" : "LEA", t, (size_t)(scan - s_gm.base));
                        return true;
                    }
                }
            }
        }
        Hydro::logWarn("EngineAPI: FNamePool not found via call tree (%d funcs)", numTargets);
    }

    // -- Strategy 2: data section scan --
    // The FNameEntryAllocator (embedded at offset 0 of FNamePool) has a distinctive layout:
    //   +0x00: SRWLOCK (8 bytes, usually 0)
    //   +0x08: CurrentBlock (uint32, typically 1-100)
    //   +0x0C: CurrentByteCursor (uint32, 0..131072)
    //   +0x10: Blocks[0] -> heap ptr -> FNameEntry starting with "None"
    // Scan writable sections (same approach that found GUObjectArray).
    Hydro::logInfo("EngineAPI: FNamePool: trying data section scan...");
    {
        auto* dosHeader = (IMAGE_DOS_HEADER*)s_gm.base;
        auto* ntHeaders = (IMAGE_NT_HEADERS*)(s_gm.base + dosHeader->e_lfanew);
        auto* sec = IMAGE_FIRST_SECTION(ntHeaders);
        int numSec = ntHeaders->FileHeader.NumberOfSections;

        for (int s = 0; s < numSec; s++, sec++) {
            if (!(sec->Characteristics & IMAGE_SCN_MEM_WRITE)) continue;
            uint8_t* secStart = s_gm.base + sec->VirtualAddress;
            size_t secSize = sec->Misc.VirtualSize;

            for (size_t off = 0; off + 0x40 <= secSize; off += 8) {
                uint8_t* addr = secStart + off;

                for (int bOff : kBlocksOffsets) {
                    if ((size_t)(off + bOff + 8) > secSize) continue;

                    // Direct read (in-module, always mapped)
                    void* block0 = *(void**)(addr + bOff);
                    if (!block0) continue;
                    // Blocks[0] must be a heap pointer, not inside the module image
                    if ((uint8_t*)block0 >= s_gm.base && (uint8_t*)block0 < s_gm.base + s_gm.size) continue;

                    if (tryValidatePool(addr, bOff)) {
                        s_fnamePool = addr;
                        s_poolBlocksOffset = bOff;
                        s_poolReady = true;
                        Hydro::logInfo("EngineAPI: FNamePool at %p (blocksOffset=0x%X) via data scan (exe+0x%zX)",
                            addr, bOff, (size_t)(addr - s_gm.base));
                        return true;
                    }
                }
            }
        }
    }

    Hydro::logWarn("EngineAPI: FNamePool not found by any method");
    return false;
}

// -- Conv_NameToString fallback --
// Used only if FNamePool direct reading fails. Goes through ProcessEvent
// which is much slower but always works.

static void* s_kismetStringCDO = nullptr;
static void* s_convNameToStringFunc = nullptr;
static std::unordered_map<uint32_t, std::string> s_fnameCache;
static bool s_fnameResolverReady = false;

static bool discoverConvNameToString() {
    if (s_fnameResolverReady) return true;

    // Find KismetStringLibrary CDO
    s_kismetStringCDO = findObject(L"/Script/Engine.Default__KismetStringLibrary");
    if (!s_kismetStringCDO) {
        Hydro::logWarn("EngineAPI: KismetStringLibrary CDO not found");
        return false;
    }

    // Find Conv_NameToString UFunction via class chain
    void* cls = nullptr;
    safeReadPtr((uint8_t*)s_kismetStringCDO + UOBJ_CLASS, &cls);
    if (cls) {
        s_convNameToStringFunc = findFunctionOnClass(cls, L"Conv_NameToString");
    }
    if (!s_convNameToStringFunc) {
        s_convNameToStringFunc = findObject(
            L"/Script/Engine.KismetStringLibrary:Conv_NameToString");
    }
    if (!s_convNameToStringFunc) {
        Hydro::logWarn("EngineAPI: Conv_NameToString not found");
        return false;
    }

    uint16_t ps = getUFunctionParmsSize(s_convNameToStringFunc);
    uint16_t ro = getUFunctionRetOffset(s_convNameToStringFunc);
    Hydro::logInfo("EngineAPI: Conv_NameToString at %p (ParmsSize=%u, RetOff=%u)",
                   s_convNameToStringFunc, ps, ro);

    // Validate: resolve FName index 0 - must be "None"
    s_fnameResolverReady = true; // temporarily enable for the test call
    std::string test = getNameString(0);
    if (test != "None") {
        Hydro::logWarn("EngineAPI: Conv_NameToString validation failed (idx=0 -> '%s', expected 'None')",
                       test.c_str());
        s_fnameResolverReady = false;
        return false;
    }

    Hydro::logInfo("EngineAPI: FName resolver ready (Conv_NameToString validated: idx 0 -> 'None')");
    return true;
}

// Safe helper: call Conv_NameToString via ProcessEvent. Returns empty on failure.
static std::string callConvNameToString(uint32_t comparisonIndex) {
    if (!s_kismetStringCDO || !s_convNameToStringFunc || !s_processEvent) return {};

    // Filter out values that are clearly garbage pointers, not FName indices.
    // On x64, pointers have high bits set (> 0x7FFFFFFF). Valid FName indices
    // can be large but fit in the lower 31 bits.
    if (comparisonIndex > 0x7FFFFFFF) return {};

    uint16_t parmsSize = getUFunctionParmsSize(s_convNameToStringFunc);
    uint16_t retOff    = getUFunctionRetOffset(s_convNameToStringFunc);

    if (parmsSize > 256 || parmsSize == 0) return {};

    // Build params buffer: FName input at offset 0, FString output at retOff.
    // Must be 16-byte aligned - ProcessEvent uses SSE/AVX instructions.
    alignas(16) uint8_t params[256] = {};
    // FName = { ComparisonIndex: uint32, Number: uint32 }
    memcpy(params, &comparisonIndex, 4);
    uint32_t number = 0;
    memcpy(params + 4, &number, 4);

    bool ok = callProcessEvent(s_kismetStringCDO, s_convNameToStringFunc, params);
    if (!ok) return {};

    // Read FString from params at retOff.
    // FString = { TCHAR* Data, int32 ArrayNum, int32 ArrayMax }
    void* data = nullptr;
    int32_t arrayNum = 0;
    memcpy(&data, params + retOff, sizeof(void*));
    memcpy(&arrayNum, params + retOff + sizeof(void*), 4);

    if (!data || arrayNum <= 0) return {};

    // Data points to a wchar_t string (UE5 TCHAR = wchar_t on Windows)
    // Convert to narrow string. arrayNum includes null terminator.
    int32_t len = arrayNum - 1;
    if (len <= 0 || len > 1024) return {};

    std::string result;
    result.reserve(len);
    const wchar_t* wdata = (const wchar_t*)data;
    for (int32_t i = 0; i < len; i++) {
        result += (char)(wdata[i] & 0x7F);
    }

    // Note: FString Data is allocated by the engine's GMalloc. We don't
    // free it here - the leak is bounded by the cache (each index resolved
    // once). Total leak is ~20KB for a typical game with ~1000 unique names
    // accessed during a Reflect session.

    return result;
}

std::string getNameString(uint32_t nameIdx) {
    // Check cache first
    auto it = s_fnameCache.find(nameIdx);
    if (it != s_fnameCache.end()) return it->second;

    // Fast path: direct pool reading (no function calls)
    if (s_poolReady) {
        std::string result = readFromPool(nameIdx);
        if (!result.empty()) {
            s_fnameCache[nameIdx] = result;
            return result;
        }
    }

    // Slow path: Conv_NameToString via ProcessEvent
    if (s_fnameResolverReady) {
        std::string result = callConvNameToString(nameIdx);
        if (!result.empty()) {
            s_fnameCache[nameIdx] = result;
            return result;
        }
    }

    // Cache the failure
    char buf[32];
    snprintf(buf, sizeof(buf), "<FName:%u>", nameIdx);
    std::string fallback(buf);
    s_fnameCache[nameIdx] = fallback;
    return fallback;
}

std::string getObjectName(void* obj) {
    return getNameString(getNameIndex(obj));
}

// FField name offset: discovered once at first call by using findProperty
// with a known property name and checking which offset produces the
// matching FName index. Same "validate against a known invariant" approach
// as FFrame::Node probing and BeginPlay vtable discovery.
static int s_ffieldNameOffset = -1;

static void discoverFieldNameOffset() {
    void* actorClass = findObject(L"/Script/Engine.Actor");
    if (!actorClass) return;

    uint32_t expectedIdx = makeFName(L"PrimaryActorTick");
    if (expectedIdx == 0) return;

    void* prop = findProperty(actorClass, L"PrimaryActorTick");
    if (!prop) return;

    // Probe candidate offsets on the known-valid FProperty
    static const int kOffsets[] = { 0x20, 0x28, 0x18, 0x30, 0x38 };
    for (int off : kOffsets) {
        uint32_t idx = 0;
        safeReadInt32((uint8_t*)prop + off, (int32_t*)&idx);
        if (idx == expectedIdx) {
            s_ffieldNameOffset = off;
            Hydro::logInfo("EngineAPI: FField name offset = 0x%X (validated via PrimaryActorTick, idx=%u)",
                           off, idx);
            return;
        }
    }

    Hydro::logWarn("EngineAPI: FField name offset not found (PrimaryActorTick probe failed)");
    s_ffieldNameOffset = FFIELD_NAME; // fallback to default
}

std::string getFieldName(void* field) {
    if (!field) return {};
    if (s_ffieldNameOffset < 0) discoverFieldNameOffset();
    uint32_t idx = 0;
    safeReadInt32((uint8_t*)field + s_ffieldNameOffset, (int32_t*)&idx);
    return getNameString(idx);
}

// SuperStruct offset discovery. AActor's SuperStruct is the UClass for
// "Object" in most UE5 builds; we probe a few known layout offsets until
// one dereferences to a struct named "Object".
static void discoverSuperOffset() {
    void* actorClass = findObject(L"/Script/Engine.Actor");
    if (!actorClass) { s_superOffset = 0x40; return; }

    uint32_t objectIdx = makeFName(L"Object");
    if (objectIdx == 0) { s_superOffset = 0x40; return; }

    // Probe offsets in the UStruct region (after UField/UObject base, before Children)
    static const int kOffsets[] = { 0x40, 0x48, 0x38, 0x50, 0x30, 0x58 };
    for (int off : kOffsets) {
        void* candidate = nullptr;
        if (!readPtr((uint8_t*)actorClass + off, &candidate) || !candidate) continue;
        uint32_t idx = 0;
        if (!safeReadInt32((uint8_t*)candidate + UOBJ_NAME, (int32_t*)&idx)) continue;
        if (idx == objectIdx) {
            s_superOffset = off;
            Hydro::logInfo("EngineAPI: UStruct::SuperStruct offset = 0x%X (validated via Actor->Object)", off);
            return;
        }
    }

    Hydro::logWarn("EngineAPI: SuperStruct offset not found, defaulting to 0x40");
    s_superOffset = 0x40;
}

void* getSuper(void* ustruct) {
    if (!ustruct) return nullptr;
    if (s_superOffset < 0) discoverSuperOffset();
    void* super = nullptr;
    readPtr((uint8_t*)ustruct + s_superOffset, &super);
    return super;
}

// -- Layer 3: UFunction parameter layout derived from the property chain --
// UFunction::ParmsSize and UFunction::ReturnValueOffset aren't really
// independent fields - they're already encoded by the parameter FProperty
// chain. Walking the chain (which uses Layer 2's discovered FProperty
// offsets) gives us:
//   ParmsSize         = max(prop->Offset_Internal + prop->ElementSize)
//   ReturnValueOffset = (the FProperty with CPF_ReturnParm set)->Offset_Internal
// This eliminates UFUNC_PARMS_SIZE / UFUNC_RET_VAL_OFFSET as load-bearing
// constants - they're now bootstrap fallbacks only. Per-UFunction cache
// avoids re-walking on every call (HydroCore calls each UFunction many
// times). Cache is bounded by the small number of UFunctions HydroCore
// touches; memory is trivial.

struct UFuncLayoutCache {
    uint16_t parmsSize;
    uint16_t retOffset;
    bool     hasReturn;
    bool     derived;     // true = computed from chain, false = fallback raw read
};
static std::unordered_map<void*, UFuncLayoutCache> s_ufuncLayouts;

static void clearUFuncLayoutCache() { s_ufuncLayouts.clear(); }

static UFuncLayoutCache computeUFunctionLayout(void* ufunc) {
    UFuncLayoutCache out = {0, 0, false, false};
    if (!ufunc) return out;

    // DIAGNOSTIC: dump UFunction slot layout to find where its property chain
    // actually lives. Our Layer 2 probe found USTRUCT_CHILD_PROPS=0x50 against
    // AActor's UClass - but UFunction may store its parameter chain at a
    // different offset on forked engines.
    Hydro::logInfo("EngineAPI: UFunction %p slot dump:", ufunc);
    for (int off = 0x40; off <= 0xE0; off += 8) {
        void* p = nullptr;
        if (!safeReadPtr((uint8_t*)ufunc + off, &p)) continue;
        uintptr_t v = (uintptr_t)p;
        bool inMod = ((uint8_t*)p >= s_gm.base && (uint8_t*)p < s_gm.base + s_gm.size);
        uint32_t lo = (uint32_t)(v & 0xFFFFFFFFULL);
        uint32_t hi = (uint32_t)(v >> 32);
        bool heapLike = !inMod && hi != 0 && lo != 0 && hi < 0x10000U;
        Hydro::logInfo("  +0x%02X: %p%s", off, p,
                       inMod ? " <module>" : (heapLike ? " <heap>" : ""));
    }

    // Use discovered chain-walk offsets so this works on hosts where the
    // UStruct/FField layout has shifted (e.g. Palworld's UE 5.1.x fork).
    void* prop = getChildProperties(ufunc);

    Hydro::logInfo("EngineAPI: computeUFunctionLayout %p chain (firstProp=%p):", ufunc, prop);

    // DIAGNOSTIC: dump the FIRST FProperty's bytes from offset 0x00 to 0x60.
    // We need to see whether it's an FField (vtable + ClassPrivate + Owner +
    // Next + NamePrivate) or a UField-derived UProperty (UObject base then
    // Next at +0x28). The Next offset on UFunction's param chain may differ
    // from what we discovered against AActor's UClass chain.
    if (prop) {
        Hydro::logInfo("EngineAPI: first FProperty %p slot dump:", prop);
        for (int off = 0x00; off <= 0x60; off += 8) {
            void* p = nullptr;
            if (!safeReadPtr((uint8_t*)prop + off, &p)) continue;
            uintptr_t v = (uintptr_t)p;
            bool inMod = ((uint8_t*)p >= s_gm.base && (uint8_t*)p < s_gm.base + s_gm.size);
            uint32_t lo = (uint32_t)(v & 0xFFFFFFFFULL);
            uint32_t hi = (uint32_t)(v >> 32);
            bool heapLike = !inMod && hi != 0 && lo != 0 && hi < 0x10000U;
            Hydro::logInfo("  +0x%02X: %p%s", off, p,
                           inMod ? " <module/vtable>" : (heapLike ? " <heap>" : ""));
        }
    }
    int count = 0;
    int paramCount = 0;
    while (prop && count < 64) {
        int32_t off = getPropertyOffset(prop);
        int32_t sz  = getPropertyElementSize(prop);
        uint64_t pflags = getPropertyFlags(prop);
        uint32_t nameIdx = 0;
        int nmOff = (s_layout.fieldNameOffset >= 0) ? s_layout.fieldNameOffset : FFIELD_NAME;
        safeReadInt32((uint8_t*)prop + nmOff, (int32_t*)&nameIdx);

        Hydro::logInfo("  [%d] prop=%p name=%u off=0x%X sz=%d flags=0x%llX %s%s%s",
                       count, prop, nameIdx, off, sz, (unsigned long long)pflags,
                       (pflags & CPF_Parm) ? "Parm " : "",
                       (pflags & CPF_OutParm) ? "Out " : "",
                       (pflags & CPF_ReturnParm) ? "Return " : "");

        // Only properties on the UFunction's chain are parameters (incl. return).
        // Defensive: skip clearly-bogus values rather than poison the result.
        if (off >= 0 && off < 4096 && sz > 0 && sz < 1024) {
            uint32_t end = (uint32_t)off + (uint32_t)sz;
            if (end > out.parmsSize && end < 4096)
                out.parmsSize = (uint16_t)end;

            if (pflags & CPF_ReturnParm) {
                out.retOffset = (uint16_t)off;
                out.hasReturn = true;
            }
            paramCount++;
        }

        prop = getNextProperty(prop);
        count++;
    }

    out.derived = (paramCount > 0);
    return out;
}

static UFuncLayoutCache getUFuncLayoutCached(void* ufunc) {
    auto it = s_ufuncLayouts.find(ufunc);
    if (it != s_ufuncLayouts.end()) return it->second;

    UFuncLayoutCache layout = computeUFunctionLayout(ufunc);

    // Fallback: if the chain walk yielded nothing (e.g. native UFunction with
    // no reflected parameters, or Layer 2 not yet up), fall back to the raw
    // hardcoded offsets. Better wrong than zero - avoids silent ProcessEvent
    // failures on bootstrap.
    if (!layout.derived) {
        uint16_t ps = 0, ro = 0;
#ifdef _WIN32
        __try {
            ps = *(uint16_t*)((uint8_t*)ufunc + UFUNC_PARMS_SIZE);
            ro = *(uint16_t*)((uint8_t*)ufunc + UFUNC_RET_VAL_OFFSET);
        } __except(1) {}
#endif
        layout.parmsSize = ps;
        layout.retOffset = ro;
        layout.hasReturn = (ro > 0 && ro < ps);
    }

    s_ufuncLayouts[ufunc] = layout;
    return layout;
}

// UFunction metadata readers - derive from the parameter property chain.
// First call per UFunction walks the chain; subsequent calls hit the cache.
uint16_t getUFunctionParmsSize(void* ufunc) {
    if (!ufunc) return 0;
    return getUFuncLayoutCached(ufunc).parmsSize;
}

uint16_t getUFunctionRetOffset(void* ufunc) {
    if (!ufunc) return 0;
    return getUFuncLayoutCached(ufunc).retOffset;
}

// -- Layer 4: per-class named property offset cache ---
// findReflectedFieldOffset(uclassPtr, "RootComponent") returns the byte
// offset of that field within instances of `uclassPtr` - discovered once
// via the property chain (which uses Layer 2's offsets), then cached.
// This replaces hardcoded class-level offsets like AActor::RootComponent
// at +0x198. Tier 2 modules can use this directly to read fields without
// committing to per-engine-version constants.
struct FieldOffsetKey {
    void* uclass;
    std::wstring fieldName;
    bool operator==(const FieldOffsetKey& o) const {
        return uclass == o.uclass && fieldName == o.fieldName;
    }
};
struct FieldOffsetKeyHash {
    size_t operator()(const FieldOffsetKey& k) const {
        size_t h = std::hash<void*>()(k.uclass);
        for (wchar_t c : k.fieldName) h = h * 131u + (size_t)c;
        return h;
    }
};
static std::unordered_map<FieldOffsetKey, int32_t, FieldOffsetKeyHash> s_fieldOffsetCache;

int32_t findReflectedFieldOffset(void* uclassPtr, const wchar_t* fieldName) {
    if (!uclassPtr || !fieldName) return -1;
    FieldOffsetKey key{uclassPtr, std::wstring(fieldName)};
    auto it = s_fieldOffsetCache.find(key);
    if (it != s_fieldOffsetCache.end()) return it->second;

    void* prop = findProperty(uclassPtr, fieldName);
    int32_t off = prop ? getPropertyOffset(prop) : -1;
    s_fieldOffsetCache[key] = off;
    return off;
}

uint32_t getUFunctionFlags(void* ufunc) {
    if (!ufunc) return 0;
    uint32_t val = 0;
    memcpy(&val, (uint8_t*)ufunc + UFUNC_FLAGS, 4);
    return val;
}

// UClass::ClassFlags offset: discovered once at first call.
// /Script/CoreUObject.Object always has CLASS_Native (0x00000001) set, plus
// CLASS_RequiredAPI (0x00000200) is a strong additional hint. Probe offsets
// until we find one where both flags are set on UObject.
static int s_classFlagsOffset = -1;

static void discoverClassFlagsOffset() {
    void* uobjectClass = findObject(L"/Script/CoreUObject.Object");
    if (!uobjectClass) { s_classFlagsOffset = 0x1C0; return; }

    // Probe common UE5 offsets for UClass::ClassFlags.
    static const int kOffsets[] = {
        0x1C0, 0x1B0, 0x1C8, 0x1D0, 0x1B8, 0x1A8, 0x1E0, 0x1E8, 0x1F0, 0x1F8, 0x200
    };
    for (int off : kOffsets) {
        uint32_t flags = 0;
        if (!safeReadInt32((uint8_t*)uobjectClass + off, (int32_t*)&flags)) continue;
        // UObject must have CLASS_Native (0x1) set.
        // Also, the upper half shouldn't be 0xFFFFFFFF or clearly garbage.
        if ((flags & 0x1) == 0) continue;
        if (flags == 0xFFFFFFFF || flags == 0) continue;
        // UObject has ~CLASS_Native|CLASS_MatchedSerializers|CLASS_RequiredAPI at minimum.
        // Accept if flags has CLASS_Native and is plausibly small.
        if (flags > 0x10000000) continue; // CLASS_* flags fit in 28 bits typically
        s_classFlagsOffset = off;
        Hydro::logInfo("EngineAPI: UClass::ClassFlags offset = 0x%X (validated via Object, flags=0x%X)", off, flags);
        return;
    }

    Hydro::logWarn("EngineAPI: UClass::ClassFlags offset not found, defaulting to 0x1C0");
    s_classFlagsOffset = 0x1C0;
}

uint32_t getClassFlags(void* cls) {
    if (!cls) return 0;
    if (s_classFlagsOffset < 0) discoverClassFlagsOffset();
    uint32_t val = 0;
    safeReadInt32((uint8_t*)cls + s_classFlagsOffset, (int32_t*)&val);
    return val;
}

// UEnum::Names offset: discovered on first call by probing candidate
// offsets for a TArray<TPair<FName, int64>> that decodes sanely.
// TArray layout in UE5 is: T* Data (8) + int32 Num (4) + int32 Max (4) = 16B
// Each TPair<FName, int64> is: FName (8 = ComparisonIdx + Number) + int64 (8) = 16B
// Validation: Num must be small (1..10000), Max >= Num, Data is a heap
// pointer (not null, not in module), and the first entry's ComparisonIndex
// must resolve to a non-empty FName via FNamePool.
static int s_enumNamesOffset = -1;

static void discoverEnumNamesOffset() {
    // Find any UEnum to probe against. Start with a well-known engine enum.
    void* enumProbe = findObject(L"/Script/Engine.ESpawnActorCollisionHandlingMethod");
    if (!enumProbe) enumProbe = findObject(L"/Script/CoreUObject.EObjectFlags");
    if (!enumProbe) {
        // Fall back to walking GUObjectArray for any UEnum
        void* enumMetaclass = findObject(L"/Script/CoreUObject.Enum");
        if (enumMetaclass) {
            int32_t total = getObjectCount();
            for (int32_t i = 0; i < total && !enumProbe; i++) {
                void* obj = getObjectAt(i);
                if (obj && getObjClass(obj) == enumMetaclass) enumProbe = obj;
            }
        }
    }

    if (!enumProbe) {
        Hydro::logWarn("EngineAPI: no UEnum found for offset probe, defaulting to 0x40");
        s_enumNamesOffset = 0x40;
        return;
    }

    // Probe offsets common in UE5: after UObject header + a few UField bits.
    static const int kOffsets[] = { 0x40, 0x48, 0x50, 0x58, 0x38, 0x60, 0x68 };
    for (int off : kOffsets) {
        void* data = nullptr;
        if (!safeReadPtr((uint8_t*)enumProbe + off, &data) || !data) continue;
        // Data must be heap (not in module)
        if ((uint8_t*)data >= s_gm.base && (uint8_t*)data < s_gm.base + s_gm.size) continue;

        int32_t num = 0, max = 0;
        if (!safeReadInt32((uint8_t*)enumProbe + off + 8, &num)) continue;
        if (!safeReadInt32((uint8_t*)enumProbe + off + 12, &max)) continue;
        if (num < 1 || num > 10000) continue;
        if (max < num) continue;

        // Validate the first entry: TPair<FName, int64> at Data[0]
        uint32_t firstComparisonIdx = 0;
        if (!safeReadInt32(data, (int32_t*)&firstComparisonIdx)) continue;
        if (firstComparisonIdx == 0) continue;
        std::string firstName = getNameString(firstComparisonIdx);
        if (firstName.empty() || firstName[0] == '<') continue;

        s_enumNamesOffset = off;
        Hydro::logInfo("EngineAPI: UEnum::Names offset = 0x%X (validated via %p, first='%s', num=%d)",
                       off, enumProbe, firstName.c_str(), num);
        return;
    }

    Hydro::logWarn("EngineAPI: UEnum::Names offset not found, defaulting to 0x40");
    s_enumNamesOffset = 0x40;
}

int readEnumNames(void* uenum, std::vector<std::pair<uint32_t, int64_t>>& out) {
    if (!uenum) return 0;
    if (s_enumNamesOffset < 0) discoverEnumNamesOffset();

    void* data = nullptr;
    if (!safeReadPtr((uint8_t*)uenum + s_enumNamesOffset, &data) || !data) return 0;

    int32_t num = 0;
    if (!safeReadInt32((uint8_t*)uenum + s_enumNamesOffset + 8, &num)) return 0;
    if (num <= 0 || num > 10000) return 0;

    out.reserve(num);
    for (int32_t i = 0; i < num; i++) {
        uint8_t* entry = (uint8_t*)data + (size_t)i * 16;
        uint32_t idx = 0;
        int64_t val = 0;
        if (!safeReadInt32(entry, (int32_t*)&idx)) break;
        // int64 via two 32-bit reads
        int32_t lo = 0, hi = 0;
        if (!safeReadInt32(entry + 8, &lo)) break;
        if (!safeReadInt32(entry + 12, &hi)) break;
        val = ((int64_t)hi << 32) | (uint32_t)lo;
        out.emplace_back(idx, val);
    }
    return (int)out.size();
}

// OuterPrivate offset: discovered once at first call by probing known objects.
// AActor's outer is the /Script/Engine package (FName = "Engine").
static int s_outerOffset = -1;

static void discoverOuterOffset() {
    // UE4SS confirms OuterPrivate at 0x20 for this engine build.
    // Validate: read AActor's outer, check its name is "Engine" (the /Script/Engine package).
    void* actorClass = findObject(L"/Script/Engine.Actor");
    if (!actorClass) {
        Hydro::logWarn("EngineAPI: OuterPrivate probe: AActor not found, defaulting to 0x%X", UOBJ_OUTER);
        s_outerOffset = UOBJ_OUTER;
        return;
    }

    // Probe offsets and log what's at each one
    static const int kOffsets[] = { 0x20, 0x28, 0x30, 0x18, 0x38, 0x40, 0x48 };
    Hydro::logInfo("EngineAPI: OuterPrivate probe on AActor at %p:", actorClass);
    for (int off : kOffsets) {
        void* candidate = nullptr;
        if (!readPtr((uint8_t*)actorClass + off, &candidate) || !candidate) {
            Hydro::logInfo("  +0x%02X: null", off);
            continue;
        }
        uint32_t idx = 0;
        safeReadInt32((uint8_t*)candidate + UOBJ_NAME, (int32_t*)&idx);
        std::string name = getNameString(idx);
        Hydro::logInfo("  +0x%02X: %p nameIdx=%u name='%s'", off, candidate, idx, name.c_str());
    }

    // Use 0x20 (confirmed by UE4SS) and log the result
    void* outer = nullptr;
    readPtr((uint8_t*)actorClass + UOBJ_OUTER, &outer);
    if (outer) {
        std::string outerName = getObjectName(outer);
        Hydro::logInfo("EngineAPI: OuterPrivate offset = 0x%X, outer name = '%s'", UOBJ_OUTER, outerName.c_str());
    }
    s_outerOffset = UOBJ_OUTER;
}

void* getOuter(void* obj) {
    if (!obj) return nullptr;
    if (s_outerOffset < 0) discoverOuterOffset();
    void* outer = nullptr;
    readPtr((uint8_t*)obj + s_outerOffset, &outer);
    return outer;
}

std::string getObjectPath(void* obj) {
    if (!obj) return {};

    // Walk the Outer chain capturing (name, isPackage) per level.
    // isPackage = the object's UClass FName == "Package".
    struct Level { std::string name; bool isPackage; };
    std::vector<Level> levels;
    void* cur = obj;
    int depth = 0;
    while (cur && depth < 32) {
        Level lv;
        lv.name = getObjectName(cur);
        void* cls = getClass(cur);
        std::string clsName = cls ? getObjectName(cls) : std::string();
        lv.isPackage = (clsName == "Package");
        levels.push_back(std::move(lv));
        cur = getOuter(cur);
        depth++;
    }
    if (levels.empty()) return {};

    // Build path mirroring UE's UObject::GetPathName (UObject.cpp:530-561):
    //   - Outermost name is emitted verbatim (UPackage names already start
    //     with '/', e.g. "/Script/AssetRegistry"; other top-level names are
    //     emitted as-is).
    //   - Separator between parent and child:
    //       ':'  when parent is non-package AND grandparent is a package
    //              (the SUBOBJECT_DELIMITER case)
    //       '.'  otherwise (the common case)
    //   - Never use '/' between non-package outers.
    // levels[size-1] = outermost, levels[0] = innermost (obj itself).
    int last = (int)levels.size() - 1;
    std::string path = levels[last].name;
    for (int i = last - 1; i >= 0; i--) {
        // parent = levels[i+1], grandparent = levels[i+2] (if exists)
        bool parentIsPkg = levels[i + 1].isPackage;
        bool grandparentIsPkg = (i + 2 <= last) ? levels[i + 2].isPackage : false;
        char sep = (!parentIsPkg && grandparentIsPkg) ? ':' : '.';
        path.push_back(sep);
        path += levels[i].name;
    }
    return path;
}


// -- FShaderCodeLibrary::OpenLibrary discovery --
// UE doesn't auto-mount shader archives from runtime-mounted paks. Without
// an explicit `FShaderCodeLibrary::OpenLibrary(name, dir)` call after pak
// mount, materials in the mod pak will load but render as default (missing
// material) because their shader maps can't bind to the unregistered
// archive.
// Discovery is the same string-xref + walk-back-to-prologue pattern used
// for StaticFindObjectFast, FName ctor, etc. UE bakes the function's own
// name as a wide-string literal in the binary (used by UE_LOG macros) -
// `"FShaderCodeLibrary::OpenLibrary"` appears verbatim. We find LEA refs
// to that string in `.text`, walk back to the function prologue.
// Validation: resilient on this build because there's exactly ONE LEA ref
// to the string in the entire `.text` section (verified empirically) - no
// ambiguity. If multiple refs ever surface in a future engine version,
// extend this with a Tier 2 (caller-side xref via `InitForRuntime` string).

static bool findOpenShaderLibrary() {
    if (!s_gm.base) return false;

    // Tier 1: locate the FShaderCodeLibrary::OpenLibrary literal string.
    // UE emits this via __FUNCTION__/__PRETTY_FUNCTION__ macros - stored as
    // ASCII, NOT UTF-16. (Wide strings come from UE_LOG TEXT() macros; the
    // function-name literal stays char* because that's what __FUNCTION__ is.)
    const char* needle = "FShaderCodeLibrary::OpenLibrary";
    size_t needleLen = strlen(needle);
    uint8_t* strAddr = nullptr;
    for (size_t i = 0; i + needleLen <= s_gm.size; i++) {
        if (memcmp(s_gm.base + i, needle, needleLen) == 0) {
            strAddr = s_gm.base + i;
            break;
        }
    }
    if (!strAddr) {
        Hydro::logWarn("EngineAPI: 'FShaderCodeLibrary::OpenLibrary' string not found");
        return false;
    }
    Hydro::logInfo("EngineAPI: 'FShaderCodeLibrary::OpenLibrary' string at exe+0x%zX",
        (size_t)(strAddr - s_gm.base));

    // Find LEA(s) to this string in the binary's code. Single-tier: one
    // anchor, walk back to the enclosing function's prologue.
    uint8_t* leaAddr = findLeaRef(s_gm.base, s_gm.size, strAddr);
    if (!leaAddr) {
        Hydro::logWarn("EngineAPI: No LEA ref to OpenLibrary string");
        return false;
    }
    Hydro::logInfo("EngineAPI: LEA at exe+0x%zX", (size_t)(leaAddr - s_gm.base));

    // Resilient containing-function lookup via the PE's exception directory
    // (.pdata). Every x64 PE binary has .pdata listing every function's
    // [BeginRVA, EndRVA) - sorted by BeginRVA. Far more reliable than the
    // prior heuristic of walking back for known prologue byte patterns,
    // which fell over on functions whose first instruction is `movaps
    // [rsp+disp32], xmmN` (SSE register save) - common in heavy renderer
    // code like FShaderLibrariesCollection::OpenLibrary, but missing from
    // the prologue whitelist so the walker overshot into a sibling
    // function (`FEngineLoop::PreInitPostStartupScreen` empirically).
    auto* dosHeader = (IMAGE_DOS_HEADER*)s_gm.base;
    auto* ntHeaders = (IMAGE_NT_HEADERS*)(s_gm.base + dosHeader->e_lfanew);
    auto& excDir = ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (excDir.VirtualAddress == 0 || excDir.Size == 0) {
        Hydro::logWarn("EngineAPI: PE has no exception directory - cannot resolve OpenLibrary");
        return false;
    }
    auto* funcs = (RUNTIME_FUNCTION*)(s_gm.base + excDir.VirtualAddress);
    size_t numFuncs = excDir.Size / sizeof(RUNTIME_FUNCTION);
    uint32_t leaRva = (uint32_t)(leaAddr - s_gm.base);

    // Binary search: .pdata is sorted by BeginAddress. Find the entry where
    // BeginAddress <= leaRva < EndAddress.
    size_t lo = 0, hi = numFuncs;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (leaRva < funcs[mid].BeginAddress) {
            hi = mid;
        } else if (leaRva >= funcs[mid].EndAddress) {
            lo = mid + 1;
        } else {
            uint32_t beginRva = funcs[mid].BeginAddress;
            uint32_t endRva   = funcs[mid].EndAddress;
            s_openShaderLibrary = s_gm.base + beginRva;
            Hydro::logInfo(
                "EngineAPI: FShaderCodeLibrary::OpenLibrary at exe+0x%X (.pdata fn 0x%X-0x%X, size 0x%X bytes)",
                beginRva, beginRva, endRva, endRva - beginRva);
            return true;
        }
    }

    Hydro::logWarn("EngineAPI: LEA at exe+0x%X has no containing .pdata function - bad anchor or stripped PE",
        leaRva);
    return false;
}

// FString layout in UE 5.5 (16 bytes):
//   +0x00: TCHAR* Data           - wide-string buffer (null-terminated)
//   +0x08: int32  ArrayNum       - character count INCLUDING null terminator
//   +0x0C: int32  ArrayMax       - capacity (>= ArrayNum)
struct FStringMinimal {
    wchar_t* Data;
    int32_t ArrayNum;
    int32_t ArrayMax;
};

// -- GMalloc discovery via patternsleuth --
// FShaderCodeLibrary::OpenLibrary takes its first FString arg by value, which
// means its Data buffer must be allocatable/freeable by UE's allocator
// (FMemory -> GMalloc). Pointing Data at .rdata or WinAPI-heap memory crashes
// when UE re-allocates the FString.
// We get GMalloc's address via patternsleuth (already linked via UE4SS) - it
// has a built-in resolver. Then GMalloc->vtable[2] is `Malloc(size, align)`.
struct PsScanConfig {
    bool guobject_array{}; bool fname_tostring{}; bool fname_ctor_wchar{};
    bool gmalloc{};
    bool static_construct_object_internal{}; bool ftext_fstring{};
    bool engine_version{}; bool fuobject_hash_tables_get{};
    bool gnatives{}; bool console_manager_singleton{}; bool gameengine_tick{};
};
struct PsCtx {
    void (*def)(wchar_t*); void (*normal)(wchar_t*); void (*verbose)(wchar_t*);
    void (*warning)(wchar_t*); void (*error)(wchar_t*);
    PsScanConfig config;
};
struct PsEngineVersion { uint16_t major; uint16_t minor; };
struct PsScanResults {
    void* guobject_array; void* fname_tostring; void* fname_ctor_wchar;
    void* gmalloc;
    void* static_construct_object_internal; void* ftext_fstring;
    PsEngineVersion engine_version;
    void* fuobject_hash_tables_get; void* gnatives;
    void* console_manager_singleton; void* gameengine_tick;
};
extern "C" bool ps_scan(PsCtx& ctx, PsScanResults& results);

static void ps_log_quiet(wchar_t*) {}

static bool findGMalloc() {
    if (s_gmalloc) return true;
    PsCtx ctx{ ps_log_quiet, ps_log_quiet, ps_log_quiet, ps_log_quiet, ps_log_quiet, {} };
    ctx.config.gmalloc = true;
    PsScanResults results{};
    bool ok = false;
#ifdef _WIN32
    __try { ok = ps_scan(ctx, results); }
    __except (1) { ok = false; }
#else
    ok = ps_scan(ctx, results);
#endif
    if (!ok || !results.gmalloc) {
        Hydro::logWarn("EngineAPI: ps_scan didn't return GMalloc");
        return false;
    }
    // results.gmalloc is the address of the GMalloc *variable* (a FMalloc**).
    // Dereference once to get the actual FMalloc* instance pointer.
    void** gmallocPtr = (void**)results.gmalloc;
    void* gmallocInstance = nullptr;
    if (!safeReadPtr(gmallocPtr, &gmallocInstance) || !gmallocInstance) {
        Hydro::logWarn("EngineAPI: GMalloc variable at %p has null instance", results.gmalloc);
        return false;
    }
    s_gmalloc = gmallocInstance;
    Hydro::logInfo("EngineAPI: GMalloc instance at %p (variable at %p)",
        s_gmalloc, results.gmalloc);
    return true;
}

// FMalloc::Malloc(SIZE_T Size, uint32 Alignment).
// Vtable index varies - FMalloc inherits from FExec so [0]=dtor, then a few
// FExec virtuals (Exec deprecated, Exec_Internal), THEN Malloc. Empirically
// often [3] in UE 5.5, but probe to handle drift.
using FMallocFn = void*(__fastcall*)(void* self, size_t size, uint32_t align);

static int s_mallocVtableIdx = -1;  // cached after first probe

static void* gmallocAlloc(size_t size, uint32_t align = 8) {
    if (!s_gmalloc) {
        if (!findGMalloc()) return nullptr;
    }
    void** vtable = nullptr;
    if (!safeReadPtr(s_gmalloc, (void**)&vtable) || !vtable) return nullptr;

    // Probe slots 2..7 to find Malloc. Per UE 5.5 source (verified
    // against MemoryBase.h + Exec.h), shipping builds have:
    //   vtable[0]  ~FMalloc dtor
    //   vtable[1..5] FExec virtuals (Exec, Exec_Runtime, Exec_Dev, Exec_Editor)
    //   vtable[6]  Malloc(SIZE_T, uint32)        <- what we want
    //   vtable[7]  TryMalloc
    //   vtable[8]  Realloc
    //   vtable[9]  TryRealloc
    //   vtable[10] Free
    //   vtable[26] GetDescriptiveName            <- sanity check anchor
    // Dev/editor builds shift each FMalloc index down by one (FExec one
    // shorter). Probing rather than hardcoding survives both build flavors
    // and modest UE-version drift.
    if (s_mallocVtableIdx < 0) {
        // Widen probe to 2..15 - Palworld's FMalloc vtable shifts the
        // Malloc slot outside the stock-UE range (5..7). The writability
        // check still rejects non-Malloc slots, so widening doesn't risk
        // false positives.
        for (int idx = 2; idx <= 15; idx++) {
            auto candidate = (FMallocFn)vtable[idx];
            if (!candidate) continue;
            void* result = nullptr;
#ifdef _WIN32
            __try { result = candidate(s_gmalloc, 16, 8); }
            __except (1) { continue; }
#else
            result = candidate(s_gmalloc, 16, 8);
#endif
            uintptr_t r = (uintptr_t)result;
            if (r <= 0x10000 || r >= 0x7FFFFFFFFFFFULL) continue;
            // Writability test - actual heap memory is RW; bool-cast values
            // pointing into random spots will fault.
            bool writable = false;
#ifdef _WIN32
            __try {
                volatile uint8_t* p = (volatile uint8_t*)result;
                uint8_t saved = p[0];
                p[0] = 0xAA;
                if (p[0] == 0xAA) writable = true;
                p[0] = saved;
            } __except (1) { writable = false; }
#endif
            if (!writable) continue;
            s_mallocVtableIdx = idx;
            Hydro::logInfo("EngineAPI: GMalloc::Malloc probed at vtable[%d] (test alloc=%p, writable)",
                idx, result);
            // Don't free the 16-byte test alloc - we haven't probed Free.
            // Permanently leaks 16 bytes. Acceptable.
            break;
        }
        if (s_mallocVtableIdx < 0) {
            Hydro::logWarn("EngineAPI: Failed to probe FMalloc::Malloc in vtable[2..7]");
            return nullptr;
        }
    }

    auto fn = (FMallocFn)vtable[s_mallocVtableIdx];
    void* p = nullptr;
#ifdef _WIN32
    __try { p = fn(s_gmalloc, size, align); }
    __except (1) { p = nullptr; }
#else
    p = fn(s_gmalloc, size, align);
#endif
    return p;
}

// Allocate + populate an FString from a wide string. Buffer is GMalloc-owned
// so OpenLibrary's FMemory::Free won't crash.
static bool buildFString(FStringMinimal& out, const wchar_t* src) {
    int len = (int)wcslen(src) + 1;  // include null terminator
    void* buf = gmallocAlloc(len * sizeof(wchar_t), 8);
    if (!buf) return false;
    memcpy(buf, src, len * sizeof(wchar_t));
    out.Data = (wchar_t*)buf;
    out.ArrayNum = len;
    out.ArrayMax = len;
    return true;
}

void* getOpenShaderLibraryFn() { return s_openShaderLibrary; }

bool openShaderLibrary(const wchar_t* libraryName, const wchar_t* mountDir) {
    if (!s_openShaderLibrary || !libraryName || !mountDir) return false;

    // Allocate FString buffers via GMalloc so UE's FMemory machinery can
    // own them. Pointing Data at .rdata or WinAPI heap caused crashes
    // (UE tried to FMemory::Free our buffers, mismatched allocator).
    FStringMinimal libString = {};
    FStringMinimal dirString = {};
    if (!buildFString(libString, libraryName) || !buildFString(dirString, mountDir)) {
        Hydro::logWarn("EngineAPI: OpenShaderLibrary skipped - couldn't allocate FString via GMalloc");
        return false;
    }

    Hydro::logInfo("EngineAPI: OpenShaderLibrary(name='%ls' @ %p, dir='%ls' @ %p)",
        libraryName, libString.Data, mountDir, dirString.Data);

    // UE 5.5 signature (verified from `Engine/Source/Runtime/RenderCore/
    // Public/ShaderCodeLibrary.h`):
    //   static RENDERCORE_API bool OpenLibrary(
    //       FString const& Name,
    //       FString const& Directory,
    //       bool bMonolithicOnly = false);
    // 3 args, both FStrings by const-ref. Calling convention:
    //   rcx = &Name (FString*)
    //   rdx = &Directory (FString*)
    //   r8b = bMonolithicOnly (zero-extended bool)
    // Earlier 2-arg attempts left r8 with garbage register state - likely
    // the crash trigger as the function read it as bool but interpreted
    // a wide value as truthy, sending it down the monolithic-only path.
    using OpenLibraryFn = bool(__fastcall*)(FStringMinimal*, FStringMinimal*, bool);
    auto fn = (OpenLibraryFn)s_openShaderLibrary;

    bool ok = false;
#ifdef _WIN32
    __try {
        ok = fn(&libString, &dirString, false);
    } __except (1) {
        Hydro::logError("EngineAPI: OpenShaderLibrary CRASHED");
        return false;
    }
#else
    ok = fn(&libString, &dirString, false);
#endif
    Hydro::logInfo("EngineAPI: OpenShaderLibrary returned %s", ok ? "true" : "false");
    return ok;
}

// -- AssetRegistry::ScanPathsSynchronous via reflection ---
// UE only auto-indexes paks discovered at engine startup. A pak mounted
// later is invisible to AssetRegistry until we explicitly request a scan.
// IAssetRegistry::ScanPathsSynchronous is reflected (UFUNCTION-decorated),
// so we can call it via ProcessEvent without depending on any pattern-
// scanned native function - the path used for everything Layer-3-and-up.
// Signature (UE 5.x):
//   virtual void ScanPathsSynchronous(const TArray<FString>& InPaths,
//                                     bool bForceRescan = false,
//                                     bool bIgnoreDenyListScanFilters = false) const;
// Param layout (derived per-host via getUFunctionParmsSize/RetOffset):
//   +0x00: TArray<FString> InPaths   (Data ptr + Num + Max = 16 bytes)
//   +0x10: bool bForceRescan
//   +0x11: bool bIgnoreDenyListScanFilters

bool scanAssetRegistryPaths(const wchar_t* virtualPath, bool forceRescan) {
    if (!s_processEvent || !virtualPath) return false;
    if (!s_assetRegImpl) {
        Hydro::logWarn("EngineAPI: scanAssetRegistryPaths skipped - AssetRegistry impl not discovered");
        return false;
    }

    // The implementing class has no reflected UFunctions on Palworld.
    // The functions live on the abstract interface UClass; we get to it via
    // any known UFunction's outer (most reliable: we already discovered
    // K2_GetAssetByObjectPath whose outer IS the interface class).
    void* arInterfaceClass = nullptr;
    if (s_getByPathFunc) {
        arInterfaceClass = getOuter(s_getByPathFunc);
    }
    if (!arInterfaceClass) {
        // Fallback: direct path lookup
        arInterfaceClass = findObject(L"/Script/AssetRegistry.AssetRegistry");
    }
    void* arImplClass = getClass(s_assetRegImpl);
    Hydro::logInfo("EngineAPI: scanAssetRegistryPaths - impl=%p implClass=%p interfaceClass=%p",
                   s_assetRegImpl, arImplClass, arInterfaceClass);

    // Diagnostic: dump every UFunction on the interface class so we can see
    // what loading primitives are exposed (e.g., GetAssetWithLoad,
    // SearchAllAssets, etc.) - not just the scan family.
    if (arInterfaceClass) {
        Hydro::logInfo("EngineAPI: AR interface class UFunctions:");
        void* child = nullptr;
        safeReadPtr((uint8_t*)arInterfaceClass + USTRUCT_CHILDREN, &child);
        int n = 0;
        while (child && n < 200) {
            uint32_t idx = 0;
            safeReadInt32((uint8_t*)child + UOBJ_NAME, (int32_t*)&idx);
            std::string nm = getNameString(idx);
            Hydro::logInfo("  AR fn[%d] = '%s'", n, nm.c_str());
            void* next = nullptr;
            safeReadPtr((uint8_t*)child + UFIELD_NEXT, &next);
            child = next;
            n++;
        }
    }
    // Also dump AssetRegistryHelpers static functions - UE often wraps
    // helpers there for things not directly on the interface.
    if (s_assetRegHelpersCDO) {
        void* helpersClass = getClass(s_assetRegHelpersCDO);
        Hydro::logInfo("EngineAPI: AR helpers class UFunctions:");
        void* child = nullptr;
        safeReadPtr((uint8_t*)helpersClass + USTRUCT_CHILDREN, &child);
        int n = 0;
        while (child && n < 200) {
            uint32_t idx = 0;
            safeReadInt32((uint8_t*)child + UOBJ_NAME, (int32_t*)&idx);
            Hydro::logInfo("  Helpers fn[%d] = '%s'", n, getNameString(idx).c_str());
            void* next = nullptr;
            safeReadPtr((uint8_t*)child + UFIELD_NEXT, &next);
            child = next;
            n++;
        }
    }

    // Try EACH class with EVERY name variant to maximize discovery.
    void* arClass = arInterfaceClass ? arInterfaceClass : arImplClass;
    void* scanFn = nullptr;
    static const wchar_t* kNames[] = {
        L"ScanPathsSynchronous",
        L"K2_ScanPathsSynchronous",
        L"ScanSynchronous",
        L"ScanFilesSynchronous",
        L"ScanModifiedAssetFiles",
        L"SearchAllAssets",
        L"AssetRegistryScanPaths",
    };
    for (const wchar_t* name : kNames) {
        if (arInterfaceClass) {
            scanFn = findFunction(arInterfaceClass, name);
            if (scanFn) { Hydro::logInfo("EngineAPI:   found %ls on interface class", name); break; }
        }
        if (arImplClass) {
            scanFn = findFunction(arImplClass, name);
            if (scanFn) { Hydro::logInfo("EngineAPI:   found %ls on impl class", name); break; }
        }
    }
    // Also try direct path lookup as a last-ditch resort (works if the
    // function is in GUObjectArray under a known full path).
    if (!scanFn) scanFn = findObject(L"/Script/AssetRegistry.AssetRegistry:ScanPathsSynchronous");
    if (!scanFn) scanFn = findObject(L"/Script/AssetRegistry.AssetRegistryImpl:ScanPathsSynchronous");
    if (!scanFn) scanFn = findObject(L"/Script/AssetRegistry.AssetRegistryHelpers:ScanPathsSynchronous");
    if (!scanFn) {
        Hydro::logWarn("EngineAPI: scanAssetRegistryPaths - Scan*Synchronous UFunction not found. Dumping AR class function names (walking up SuperStruct chain):");
        // Walk class hierarchy, dumping Children at each level.
        int superOff = (s_superOffset >= 0) ? s_superOffset : 0x40;
        void* cur = arClass;
        int depth = 0;
        while (cur && depth < 8) {
            uint32_t classIdx = 0;
            safeReadInt32((uint8_t*)cur + UOBJ_NAME, (int32_t*)&classIdx);
            Hydro::logInfo("  [class depth %d] %p '%s'", depth, cur, getNameString(classIdx).c_str());
            void* child = nullptr;
            safeReadPtr((uint8_t*)cur + USTRUCT_CHILDREN, &child);
            int n = 0;
            while (child && n < 200) {
                uint32_t idx = 0;
                safeReadInt32((uint8_t*)child + UOBJ_NAME, (int32_t*)&idx);
                std::string nm = getNameString(idx);
                Hydro::logInfo("    child[%d] = '%s'", n, nm.c_str());
                void* next = nullptr;
                safeReadPtr((uint8_t*)child + UFIELD_NEXT, &next);
                child = next;
                n++;
            }
            void* super = nullptr;
            safeReadPtr((uint8_t*)cur + superOff, &super);
            if (super == cur) break;
            cur = super;
            depth++;
        }
        return false;
    }
    Hydro::logInfo("EngineAPI: scanAssetRegistryPaths - found scan UFunction at %p", scanFn);

    // Allocate the path FString via GMalloc so the function can read it
    // safely. Don't free - UE's TArray destructor would handle this if it
    // takes ownership; if not, ~16 bytes per scan is acceptable leak.
    FStringMinimal pathStr = {};
    if (!buildFString(pathStr, virtualPath)) {
        Hydro::logWarn("EngineAPI: scanAssetRegistryPaths - failed to allocate FString");
        return false;
    }

    // Allocate the TArray's element buffer via GMalloc too. UE TArray<FString>
    // is contiguous: { FString[Num] }, no individual heap per element from
    // outside-the-buffer's perspective.
    void* arrayBuf = gmallocAlloc(sizeof(FStringMinimal), 8);
    if (!arrayBuf) {
        Hydro::logWarn("EngineAPI: scanAssetRegistryPaths - failed to allocate TArray buffer");
        return false;
    }
    *(FStringMinimal*)arrayBuf = pathStr;

    struct TArrayMinimal {
        void*   Data;
        int32_t Num;
        int32_t Max;
    };
    TArrayMinimal pathArray = { arrayBuf, 1, 1 };

    uint16_t parmsSize = getUFunctionParmsSize(scanFn);
    if (parmsSize == 0 || parmsSize > 256) {
        Hydro::logWarn("EngineAPI: scanAssetRegistryPaths - bad ParmsSize=%u", parmsSize);
        return false;
    }

    alignas(16) uint8_t params[256] = {};
    memcpy(params + 0x00, &pathArray, sizeof(pathArray));    // TArray<FString>
    *(bool*)(params + 0x10) = forceRescan;                   // bForceRescan
    // DIAGNOSTIC 2026-04-28: flip to true. Tests whether Palworld AR has a
    // deny-list filter blocking /Game/Mods/. If this changes ScanPathsSynchronous
    // behavior (i.e. the scan suddenly indexes our pak content), we found the
    // simplest possible fix: one-byte default. If no change, the bug is upstream
    // (mount point never registered with FPackageName).
    *(bool*)(params + 0x11) = true;                           // bIgnoreDenyListScanFilters (was false)

    Hydro::logInfo("EngineAPI: ScanPathsSynchronous('%ls', forceRescan=%d, bIgnoreDenyList=true) - ParmsSize=%u",
                   virtualPath, forceRescan ? 1 : 0, parmsSize);

    bool ok = callFunction(s_assetRegImpl, scanFn, params);
    Hydro::logInfo("EngineAPI: ScanPathsSynchronous returned %s", ok ? "true" : "false");
    return ok;
}

// -- AssetRegistry::ScanFilesSynchronous via reflection ---
// Sibling of scanAssetRegistryPaths but takes a filesystem .uasset path
// instead of a virtual /Game/ path. Reason: on Palworld, ScanPathsSynchronous
// drops our path during FPackageName::TryConvertLongPackageNameToFilename
// because the runtime-mounted pak's mount point was never registered with
// FPackageName (the broadcast that should have called RegisterMountPoint
// during pak mount didn't fire). ScanFilesSynchronous's filesystem-direction
// lookup may bypass that broken step entirely - UE 5.1 reads the .uasset's
// own header to extract the package name, so it doesn't need the mount tree.
// Signature (UE 5.1):
//   virtual void ScanFilesSynchronous(const TArray<FString>& Files,
//                                     bool bForceRescan = false) const;
// Param layout: same TArray<FString> at +0x00, single bool at +0x10.
// (No bIgnoreDenyListScanFilters - that was ScanPaths-only.)

bool scanAssetRegistryFiles(const wchar_t* uassetFilename, bool forceRescan) {
    if (!s_processEvent || !uassetFilename) return false;
    if (!s_assetRegImpl) {
        Hydro::logWarn("EngineAPI: scanAssetRegistryFiles skipped - AssetRegistry impl not discovered");
        return false;
    }

    // Resolve interface class (where ScanFilesSynchronous lives, same as
    // ScanPathsSynchronous). Use the cached helper-derived path.
    void* arInterfaceClass = nullptr;
    if (s_getByPathFunc) arInterfaceClass = getOuter(s_getByPathFunc);
    if (!arInterfaceClass) arInterfaceClass = findObject(L"/Script/AssetRegistry.AssetRegistry");
    void* arImplClass = getClass(s_assetRegImpl);

    // Find the UFunction. ScanFilesSynchronous is the only name variant we
    // care about here (no fallback to ScanPaths or anything else - those
    // serve different purposes).
    void* scanFn = nullptr;
    if (arInterfaceClass) scanFn = findFunction(arInterfaceClass, L"ScanFilesSynchronous");
    if (!scanFn && arImplClass) scanFn = findFunction(arImplClass, L"ScanFilesSynchronous");
    if (!scanFn) scanFn = findObject(L"/Script/AssetRegistry.AssetRegistry:ScanFilesSynchronous");
    if (!scanFn) scanFn = findObject(L"/Script/AssetRegistry.AssetRegistryImpl:ScanFilesSynchronous");
    if (!scanFn) {
        Hydro::logWarn("EngineAPI: scanAssetRegistryFiles - ScanFilesSynchronous UFunction not found");
        return false;
    }

    // Diagnostic: read the native fn ptr (UFunction+0xD8) to check whether
    // ScanFilesSynchronous has a real body or is a stripped stub in this build.
    {
        void* nativeFn = nullptr;
        if (safeReadPtr((uint8_t*)scanFn + UFUNC_FUNC, &nativeFn) && nativeFn) {
            uint8_t fnHead[16] = {};
            SIZE_T bytesRead = 0;
            if (ReadProcessMemory(GetCurrentProcess(), nativeFn, fnHead, 16, &bytesRead) && bytesRead == 16) {
                Hydro::logInfo("EngineAPI: ScanFilesSynchronous native fn=%p bytes: "
                    "%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
                    nativeFn,
                    fnHead[0],  fnHead[1],  fnHead[2],  fnHead[3],
                    fnHead[4],  fnHead[5],  fnHead[6],  fnHead[7],
                    fnHead[8],  fnHead[9],  fnHead[10], fnHead[11],
                    fnHead[12], fnHead[13], fnHead[14], fnHead[15]);
            } else {
                Hydro::logInfo("EngineAPI: ScanFilesSynchronous native fn=%p (unreadable)", nativeFn);
            }
        } else {
            Hydro::logInfo("EngineAPI: ScanFilesSynchronous native fn ptr null/unreadable at UFunction+0xD8");
        }
    }

    // Build the FString and TArray<FString>{ one element } via GMalloc, same
    // pattern as ScanPathsSynchronous - UE will read it as if we were the
    // engine's own caller.
    FStringMinimal pathStr = {};
    if (!buildFString(pathStr, uassetFilename)) {
        Hydro::logWarn("EngineAPI: scanAssetRegistryFiles - failed to allocate FString");
        return false;
    }

    void* arrayBuf = gmallocAlloc(sizeof(FStringMinimal), 8);
    if (!arrayBuf) {
        Hydro::logWarn("EngineAPI: scanAssetRegistryFiles - failed to allocate TArray buffer");
        return false;
    }
    *(FStringMinimal*)arrayBuf = pathStr;

    struct TArrayMinimal { void* Data; int32_t Num; int32_t Max; };
    TArrayMinimal pathArray = { arrayBuf, 1, 1 };

    uint16_t parmsSize = getUFunctionParmsSize(scanFn);
    if (parmsSize == 0 || parmsSize > 256) {
        Hydro::logWarn("EngineAPI: scanAssetRegistryFiles - bad ParmsSize=%u", parmsSize);
        return false;
    }

    alignas(16) uint8_t params[256] = {};
    memcpy(params + 0x00, &pathArray, sizeof(pathArray));    // TArray<FString> Files
    *(bool*)(params + 0x10) = forceRescan;                   // bForceRescan

    Hydro::logInfo("EngineAPI: ScanFilesSynchronous('%ls', forceRescan=%d) - ParmsSize=%u",
                   uassetFilename, forceRescan ? 1 : 0, parmsSize);

    bool ok = callFunction(s_assetRegImpl, scanFn, params);
    Hydro::logInfo("EngineAPI: ScanFilesSynchronous returned %s", ok ? "true" : "false");
    return ok;
}

void tryDeferredAssetRegistryDiscovery() {
    // Already discovered? Nothing to do.
    if (s_assetRegHelpersCDO && s_getAssetFunc) return;

    // Throttle: only retry every ~2 seconds. discoverAssetRegistry walks
    // GUObjectArray (via findObject path lookups) which is O(N=900k) on
    // a miss - running it every engine tick would burn the game thread.
    static uint64_t s_lastAttempt = 0;
    uint64_t now = GetTickCount64();
    if (now - s_lastAttempt < 2000) return;
    s_lastAttempt = now;

    // Skip if prerequisites aren't ready yet (FName ctor + GUObjectArray).
    if (!s_fnameConstructor || !s_guObjectArray) return;

    // Single attempt - if AR CDOs aren't in GUObjectArray yet, this returns
    // false and we'll retry in 2 seconds.
    if (discoverAssetRegistry() && s_assetRegHelpersCDO && s_getAssetFunc) {
        Hydro::logInfo("EngineAPI: AssetRegistry deferred-init succeeded after %llu ms",
                       now - s_lastAttempt);
    }
}

} // namespace Hydro::Engine
