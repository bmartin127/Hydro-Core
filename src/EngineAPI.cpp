#include "EngineAPI.h"
#include "HydroCore.h"
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
 * Bootstrap: find game module -> pattern-scan GUObjectArray / StaticLoadObject /
 * FName pool -> extract ProcessEvent from vtable[79] -> locate UWorld and
 * GameplayStatics CDO -> ready to spawn actors and call UFunctions.
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
static bool findFNameConstructor();
static bool discoverAssetRegistry();
static bool discoverConvNameToString();
static bool discoverFNamePool();
static bool findOpenShaderLibrary();
static bool callProcessEvent(void* obj, void* func, void* params);
static void* findObjectViaScan(const wchar_t* path);
static int readPoolEntryRaw(uint32_t comparisonIndex, char* buf, int bufSize, bool* isWide);
static void staticLoadObjectCrashed(bool wasFromCache);
// True after a fatal SLO crash - stops re-entry and invalidates cache.
static bool s_initFatallyFailed = false;

// Cached state

static bool s_ready = false;
static GameModule s_gm = {};

static bool s_poolReady = false;
static void* s_fnamePool = nullptr;
static int s_poolBlocksOffset = -1;

// Discovered function pointers
static void* s_guObjectArray = nullptr;     // FUObjectArray*
static void* s_staticLoadObject = nullptr;  // StaticLoadObject function

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

// UStruct::SuperStruct offset; -1 until discovered (callers fall back to 0x40).
static int s_superOffset = -1;

static void* s_fnameConstructor = nullptr;     // FName(const TCHAR*, EFindName) function
static void* s_assetRegHelpersCDO = nullptr;   // Default__AssetRegistryHelpers
static void* s_getAssetFunc = nullptr;         // AssetRegistryHelpers:GetAsset UFunction
static void* s_assetRegImpl = nullptr;         // AssetRegistry implementation object
static void* s_getByPathFunc = nullptr;        // AssetRegistry:GetAssetByObjectPath UFunction

static void* s_realStaticFindObject = nullptr;
static void* s_loadPackage = nullptr;
static void* s_staticFindObject = nullptr;
static void* s_staticFindObjectFast = nullptr;
static void* s_openShaderLibrary = nullptr;
static void* s_gmalloc = nullptr;

// Forward decl: clears UFunction parameter layout cache after Layer 2 re-derives offsets.
static void clearUFuncLayoutCache();

// Discovered struct field offsets. -1 = not yet discovered; callers fall back to hardcoded.
// FField/UStruct chain-walk offsets are probed first (UE 5.1 forks shift them by +8
// vs. stock 5.5); FProperty internal offsets are probed after the chain walk works.
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

// Pattern scan cache: stores offsets relative to module base. Invalidated
// when the binary size changes (game update). Skips the ~6s pattern scan.

struct ScanCache {
    uint32_t magic;                  // bumped when layout changes
    size_t moduleSize;               // cache key
    ptrdiff_t staticLoadObject;      // StaticLoadObject offset from base
    ptrdiff_t fnameConstructor;      // FName constructor offset from base
    ptrdiff_t guObjectArray;         // GUObjectArray offset from base
    ptrdiff_t realStaticFindObject;  // Real StaticFindObject offset
    ptrdiff_t loadPackage;           // LoadPackageInternal offset
    ptrdiff_t fnamePool;             // FNamePool offset (0 = not cached)
    int32_t   poolBlocksOffset;      // Blocks[] offset within FNamePool
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

// Called when the StaticLoadObject sanity-test crashes. Lives outside initialize()
// to avoid C2712 (std::filesystem destructors incompatible with __try).
static void staticLoadObjectCrashed(bool wasFromCache) {
    Hydro::logError("EngineAPI: StaticLoadObject CRASHED - %s",
        wasFromCache ? "stale cached pointer; will pattern-scan fresh next tick"
                     : "wrong function from fresh scan; giving up to avoid retry storm");
    s_staticLoadObject = nullptr;
    s_staticFindObject = nullptr;
    if (wasFromCache) {
        // Stale ASLR base - every restored pointer is wrong. Zero all of them
        // and clear s_poolReady so FNamePool re-scans next tick.
        // s_layout.* fields (struct offsets) are kept - they don't shift with ASLR.
        s_fnamePool            = nullptr;
        s_guObjectArray        = nullptr;
        s_fnameConstructor     = nullptr;
        s_realStaticFindObject = nullptr;
        s_loadPackage          = nullptr;
        s_staticFindObjectFast = nullptr;
        s_openShaderLibrary    = nullptr;
        s_poolReady            = false;
        Hydro::logInfo("EngineAPI: cleared all cached pointers - full re-discovery next tick");
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

    // Restore struct layout offsets (negative = not cached; re-runs lazily).
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

    // Fallback: scan writable data sections at 8-byte stride for a plausible
    // FChunkedFixedUObjectArray, accepting the candidate with the largest NumElements.

    // Find data sections via PE header
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

    // Validate a candidate TUObjectArray: NumElements must be in range, chunk
    // table must dereference, first 5 entries must contain heap UObjects.
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

void* getObjectAt(int32_t index) {
    if (!s_guObjectArray || index < 0) return nullptr;

    void** chunkTable = nullptr;
    if (!safeReadPtr((uint8_t*)s_guObjectArray + FARRAY_OBJECTS, (void**)&chunkTable) || !chunkTable)
        return nullptr;

    int32_t chunkIndex = index / CHUNK_SIZE;
    int32_t withinChunk = index % CHUNK_SIZE;

    void* chunk = nullptr;
    if (!safeReadPtr(&chunkTable[chunkIndex], &chunk) || !chunk)
        return nullptr;

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

// Internal class read; public getClass() is at the bottom of the file.
void* getObjClass(void* obj) {
    if (!obj) return nullptr;
    void* cls = nullptr;
    safeReadPtr((uint8_t*)obj + UOBJ_CLASS, &cls);
    return cls;
}

// StaticLoadObject discovery

using StaticLoadObjectFn_t = void*(__fastcall*)(
    void* Class, void* InOuter, const wchar_t* Name, const wchar_t* Filename,
    uint32_t LoadFlags, void* Sandbox, bool bAllowReconciliation, void* InstancingContext);

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

// Object UClass pointer, cached after the first GUObjectArray walk.
// Used to validate SFO/SLO candidates by pointer equality before FName resolver is up.
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

// Tier A: pointer-equality against the Object UClass anchor (preferred).
// Tier B fallback: diversity check - two different paths must return different non-null pointers.
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
    // "Failed to find object" is in StaticFindObject (find-only). Save it as
    // a bonus; the actual loader is found via loading-specific strings below.
    uint8_t* findStr = findWideString(s_gm.base, s_gm.size, L"Failed to find object");
    if (!findStr) {
        Hydro::logWarn("EngineAPI: 'Failed to find object' string not found");
        return false;
    }

    Hydro::logInfo("EngineAPI: 'Failed to find object' at exe+0x%zX", (size_t)(findStr - s_gm.base));

    for (size_t i = 0; i + 7 < s_gm.size; i++) {
        uint8_t* p = s_gm.base + i;
        if ((p[0] != 0x48 && p[0] != 0x4C) || p[1] != 0x8D) continue;
        uint8_t modrm = p[2];
        if ((modrm & 0xC7) != 0x05 && (modrm & 0xC7) != 0x0D &&
            (modrm & 0xC7) != 0x15 && (modrm & 0xC7) != 0x35) continue;
        int32_t disp = *(int32_t*)(p + 3);
        if (p + 7 + disp != findStr) continue;

        Hydro::logInfo("EngineAPI: LEA ref at exe+0x%zX", i);

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

    if (s_staticLoadObject) {
        s_staticFindObject = s_staticLoadObject;
        Hydro::logInfo("EngineAPI: StaticFindObject confirmed at %p", s_staticFindObject);
    }

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

static bool discoverEngineObjects() {
    if (!s_guObjectArray && !s_staticLoadObject) return false;

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
        // Fallback: scan 'mov rax,[rip+disp]' for a global pointing at a UWorld instance.
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
                // CDO offset varies by build; scan UClass+0x100..+0x200.
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

    bool fromCache = loadScanCache(s_gm);

    if (!fromCache) {
        if (!findStaticLoadObject()) {
            Hydro::logError("EngineAPI: StaticLoadObject not found");
            return false;
        }

        // If the exe has a "StaticLoadObject" literal, prefer that over the heuristic result.
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
            void** vtable = nullptr;
            if (safeReadPtr(anyObj, (void**)&vtable) && vtable) {
                void* pe = nullptr;
                if (safeReadPtr(&vtable[VTABLE_PROCESS_EVENT], &pe) && pe &&
                    (uint8_t*)pe >= s_gm.base && (uint8_t*)pe < s_gm.base + s_gm.size) {
                    s_processEvent = (ProcessEventFn)pe;
                    Hydro::logInfo("EngineAPI: ProcessEvent at %p", pe);
                }
            }
            bool isObject = validateStaticLoadObjectResult(anyObj, nullptr);
            if (isObject) {
                Hydro::logInfo("EngineAPI: StaticLoadObject validated (returned 'Object')");
            } else {
                Hydro::logWarn("EngineAPI: StaticLoadObject FAILED validation - clearing");
                s_staticLoadObject = nullptr;
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
    if (!findGUObjectArray()) {
        Hydro::logWarn("EngineAPI: GUObjectArray not found - will try alternate world discovery");
    }

    {
        uint8_t* lpStr = findWideString(s_gm.base, s_gm.size,
            L"Attempted to LoadPackage from empty PackagePath");
        if (lpStr) {
            Hydro::logInfo("EngineAPI: 'Attempted to LoadPackage' at exe+0x%zX", (size_t)(lpStr - s_gm.base));
            uint8_t* leaAddr = findLeaRef(s_gm.base, s_gm.size, lpStr);
            if (leaAddr) {
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

    // Find the real StaticFindObject via callers of StaticFindObjectFast.
    // Skipped by default (HYDRO_SKIP_REAL_SFO=0 to force): probing 32 candidates
    // can hang on some UE 5.5 hosts. The /Script/-only SFO from Step 4 suffices
    // for reflection; the wider variant is only needed for cooked-asset path resolution.
    bool skipRealSFO = false;
    if (char* envSkip = std::getenv("HYDRO_SKIP_REAL_SFO")) {
        skipRealSFO = (envSkip[0] == '1');
    } else {
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

                    // Collect size-range candidates; validate by behavior below.
                    // Size-only is unreliable on forked engines.
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
                        if (funcSize <= 100 || funcSize >= 2000) continue;

                        if (numCandidates < MAX_CANDIDATES)
                            candidates[numCandidates++] = {funcStart, funcSize};
                    }

                    Hydro::logInfo("EngineAPI: %d callers of StaticFindObjectFast -> %d unique size-range candidates",
                        callerCount, numCandidates);

                    // Sort descending by size; real SFO is usually larger.
                    for (int a = 0; a < numCandidates; a++) {
                        for (int b = a + 1; b < numCandidates; b++) {
                            if (candidates[b].size > candidates[a].size) {
                                Candidate t = candidates[a];
                                candidates[a] = candidates[b];
                                candidates[b] = t;
                            }
                        }
                    }

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

    if (!findFNameConstructor()) {
        Hydro::logWarn("EngineAPI: FName constructor not found - asset loading disabled");
    }

    saveScanCache(s_gm);
    } // end !fromCache

    // Recover SLO by validating callers of the validated SFO.
    // Runs in both cache-load and fresh-scan paths.
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

    if (!s_guObjectArray) {
        if (findGUObjectArray()) {
            // Update cache with the newly found address
            saveScanCache(s_gm);
        } else {
            Hydro::logWarn("EngineAPI: GUObjectArray not found - findClass/findAll/assets unavailable");
        }
    }

    // Probe struct offsets before any property-chain walks so cached UFunction
    // layouts don't embed stale fallback values.
    if (s_guObjectArray && s_fnameConstructor && !s_layout.succeeded) {
        if (discoverPropertyLayout()) {
            clearUFuncLayoutCache();
            saveScanCache(s_gm);
        }
    }

    // FName resolution must come before AR/engine-object discovery.
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

    if (s_fnameConstructor) {
        if (!discoverAssetRegistry()) {
            Hydro::logWarn("EngineAPI: AssetRegistry not fully initialized");
        }
    }

    if (!discoverEngineObjects()) {
        Hydro::logWarn("EngineAPI: Some engine objects not found - spawning may fail");
    }

    if (!s_openShaderLibrary) {
        if (findOpenShaderLibrary()) {
            saveScanCache(s_gm);
        } else {
            Hydro::logWarn("EngineAPI: OpenShaderLibrary not found - mod materials may render as defaults");
        }
    }

    if (s_realStaticFindObject && s_fnameConstructor && !s_layout.succeeded) {
        if (discoverPropertyLayout()) saveScanCache(s_gm);
    }

    s_ready = (s_processEvent != nullptr);
    Hydro::logInfo("EngineAPI: Bootstrap %s", s_ready ? "COMPLETE" : "PARTIAL");

    return s_ready;
}

bool isReady() { return s_ready; }

// FName constructor: void __fastcall FName::FName(FName*, const TCHAR*, int32 FindType)
// FindType: 0 = FNAME_Find, 1 = FNAME_Add
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

            // Found LEA rdx to our string. Check instruction patterns after it.

            // Pattern A: lea rdx,[str]; lea rcx,[fname]; call ctor
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

            // Pattern B: lea rdx,[str]; lea r8,[?]; mov r9b,1; call wrapper->ctor
            if (p[7] == 0x4C && p[8] == 0x8D && p[9] == 0x05) {
                if (p[14] == 0x41 && p[15] == 0xB1 && p[16] == 0x01 && p[17] == 0xE8) {
                    uint8_t* target = resolveRip4(p + 18);
                    if (target >= s_gm.base && target < s_gm.base + s_gm.size) {
                        // This is a wrapper - find first CALL inside it
                        for (int off = 0; off < 64; off++) {
                            if (target[off] == 0xE8) {
                                uint8_t* innerTarget = resolveRip4(target + off + 1);
                                if (innerTarget >= s_gm.base && innerTarget < s_gm.base + s_gm.size) {
                                    FName8 validate = {};
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

            // Pattern C: mov r8d,1; lea rdx,[str]; lea rcx,[fname]; jmp ctor
            if (i >= 6 && p[-6] == 0x41 && p[-5] == 0xB8 && p[-4] == 0x01 &&
                p[-3] == 0x00 && p[-2] == 0x00 && p[-1] == 0x00) {
                if (p[7] == 0x48 && p[8] == 0x8D && p[9] == 0x0D && p[14] == 0xE9) {
                    uint8_t* target = resolveRip4(p + 15);
                    if (target >= s_gm.base && target < s_gm.base + s_gm.size) {
                        FName8 validate = {};
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

// Walks UClass::Children (UField linked list) up the SuperStruct chain.
static void* findFunctionOnClass(void* uclass, const wchar_t* funcName) {
    if (!uclass || !funcName || !s_fnameConstructor) return nullptr;

    // Construct FName for the function name
    FName8 targetName = {};
    if (!safeConstructFName(&targetName, funcName)) return nullptr;

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
        if (void* r = findObject(path)) return r;
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
        if (s_staticFindObject)
            return safeCallLoadObject(s_staticFindObject, path, 0);
        return nullptr;
    };

    // Find AssetRegistryHelpers CDO
    s_assetRegHelpersCDO = find(L"/Script/AssetRegistry.Default__AssetRegistryHelpers");
    if (s_assetRegHelpersCDO)
        Hydro::logInfo("EngineAPI: AssetRegistryHelpers CDO at %p", s_assetRegHelpersCDO);

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

    auto* getRegistryFunc = find(L"/Script/AssetRegistry.AssetRegistryHelpers:GetAssetRegistry");
    if (getRegistryFunc && s_assetRegHelpersCDO && s_processEvent) {
        Hydro::logInfo("EngineAPI: GetAssetRegistry UFunction at %p - calling to get runtime instance...", getRegistryFunc);

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

// GUObjectArray-walk findObject: splits path on '.' and ':' into ordered parts
// (e.g. "/Script/Engine.Actor" -> ["/Script/Engine", "Actor"]), then walks the
// array matching FName indices against each part and checking the outer chain.

using StaticFindObjectFn = void*(__fastcall*)(void*, void*, const wchar_t*, bool);

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

    std::vector<uint32_t> partIdx;
    partIdx.reserve(parts.size());
    for (auto& s : parts) {
        FName8 fn = {};
        if (!safeConstructFName(&fn, s.c_str()) || !fn.comparisonIndex) return nullptr;
        partIdx.push_back(fn.comparisonIndex);
    }
    uint32_t targetNameIdx = partIdx.back();

    void** chunkTable = nullptr;
    int32_t count = 0;
    if (!safeReadPtr((uint8_t*)s_guObjectArray + FARRAY_OBJECTS, (void**)&chunkTable)) return nullptr;
    if (!safeReadInt32((uint8_t*)s_guObjectArray + FARRAY_NUM_ELEMS, &count)) return nullptr;
    if (!chunkTable || count <= 0) return nullptr;

    // Strict pass: full outer chain must match parts in exact order.
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

        uint32_t selfIdx = 0;
        if (!safeReadInt32((uint8_t*)obj + UOBJ_NAME, (int32_t*)&selfIdx)) continue;
        if (selfIdx != targetNameIdx) continue;

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

        // Fallback: obj.name matches last part AND outermost package matches first part
        // (case-insensitive string compare; FName index parity isn't guaranteed on forks).
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

    // Tier-3 diagnostic: on tiers 1+2 miss, dump candidates matching by string
    // to surface FName index mismatches. One-shot per unique path.
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
            // No string-name match at all: dump first 10 entries to check if
            // the GUObjectArray walk itself is broken.
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

    // Tier 3: full-path string compare (catch-all for forks where CDO outer-chain
    // shape differs from stock UE, e.g. Palworld's UE 5.1.x AssetRegistry CDOs).
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

    std::wstring key(path);
    auto it = s_findObjectCache.find(key);
    if (it != s_findObjectCache.end() && it->second) {
        void* cls = nullptr;
        if (safeReadPtr((uint8_t*)it->second + UOBJ_CLASS, &cls) && cls) {
            return it->second;
        }
        s_findObjectCache.erase(it);
    }

    if (void* result = findObjectViaScan(path)) {
        s_findObjectCache[key] = result;
        return result;
    }

    if (s_realStaticFindObject) {
        if (void* result = safeCallFindObject(s_realStaticFindObject, path)) {
            s_findObjectCache[key] = result;
            return result;
        }
    }

    return nullptr;
}

// Public API: loadAsset via AssetRegistry::GetAsset.

void* loadAsset(const wchar_t* assetPath) {
    if (!assetPath || !s_fnameConstructor || !s_processEvent) return nullptr;
    if (!s_assetRegHelpersCDO || !s_getAssetFunc) {
        // Deferred retry: AR CDOs may not be loaded at HydroCore bootstrap.
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

    uint16_t gaParmsSize = getUFunctionParmsSize(s_getAssetFunc);
    uint16_t gaRetOffset = getUFunctionRetOffset(s_getAssetFunc);

    // Probe FAssetData field offsets at runtime (compiler layout can shift between builds).
    int32_t pkgNameOffset = -1, assetNameOffset = -1;
    {
        void* firstProp = nullptr;
        safeReadPtr((uint8_t*)s_getAssetFunc + UFUNC_CHILD_PROPS, &firstProp);
        if (firstProp) {
            void* assetDataStruct = safeCallLoadObject(s_staticFindObject, L"/Script/CoreUObject.AssetData", 0);
            if (!assetDataStruct)
                assetDataStruct = safeCallLoadObject(s_staticFindObject, L"/Script/CoreUObject.ScriptStruct'/Script/CoreUObject.AssetData'", 0);

            if (assetDataStruct) {
                Hydro::logInfo("EngineAPI: FAssetData UScriptStruct at %p", assetDataStruct);
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

    if (pkgNameOffset < 0) pkgNameOffset = 0x00;
    if (assetNameOffset < 0) assetNameOffset = 0x10;

    std::wstring pkgPathStr = packageName;
    auto lastSlash = pkgPathStr.find_last_of(L'/');
    if (lastSlash != std::wstring::npos) pkgPathStr = pkgPathStr.substr(0, lastSlash);
    FName8 pkgPathFName = {};
    safeConstructFName(&pkgPathFName, pkgPathStr.c_str());

    // FAssetData: +0x00 PackageName, +0x08 PackagePath, +0x10 AssetName, +0x18 AssetClass
    uint8_t params[1024] = {};
    memcpy(params + pkgNameOffset, &pkgFName, 8);      // PackageName
    memcpy(params + 0x08,          &pkgPathFName, 8);  // PackagePath
    memcpy(params + assetNameOffset, &assetFName, 8);   // AssetName

    if (!callProcessEvent(s_assetRegHelpersCDO, s_getAssetFunc, params)) {
        Hydro::logError("EngineAPI: GetAsset ProcessEvent failed");
        return nullptr;
    }

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

        // ScanFilesSynchronous fallback: try filesystem paths for known project names.
        {
            std::wstring pkgRel = packageName;
            const wchar_t kGamePrefix[] = L"/Game/";
            if (pkgRel.compare(0, 6, kGamePrefix) == 0) pkgRel = pkgRel.substr(6);

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
            Hydro::logInfo("EngineAPI: All ScanFilesSynchronous candidates exhausted");
        }

        // AR-state diagnostic: walk GUObjectArray to see what pak content is loaded.
        // One-shot per session.
        static bool s_arDiagDumped = false;
        if (!s_arDiagDumped) {
            s_arDiagDumped = true;
            Hydro::logInfo("EngineAPI: AR-state diagnostic ── walking GUObjectArray for pak-content matches");

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
            Hydro::logInfo("  RESULT: %d object(s) under prefix", matches);

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
                Hydro::logInfo("  -> No /Game/Mods packages; base game is present. Mount broadcast likely missed.");
            } else if (gameModsPkg == 0 && gamePalPkg == 0) {
                Hydro::logInfo("  -> No /Game/* packages at all. GUObjectArray walk may be incomplete.");
            }
            Hydro::logInfo("EngineAPI: AR-state diagnostic done ──");
        }
    }

    return nullptr;
}

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

// Find AActor::DispatchBeginPlay by scanning for `call qword ptr [reg+BeginPlayVtableOffset]`
// (FF /2 m32), narrowing to small functions with multiple callers.

static uint8_t* findFunctionStartBackwards(uint8_t* at, uint8_t* modBase) {
    uint8_t* p = at;
    uint8_t* limit = (p > modBase + 4096) ? p - 4096 : modBase;
    for (; p > limit; p--) {
        if (p[0] == 0xCC) {
            uint8_t* q = p;
            while (q < at && *q == 0xCC) q++;
            uint8_t prev = (p > modBase) ? *(p - 1) : 0;
            if (prev == 0xC3 || prev == 0xCC) return q;
            if (p > modBase + 5 && *(p - 5) == 0xE9) return q;
        }
    }
    return nullptr;
}

struct DbpCandidate {
    uint8_t* funcStart;
    size_t approxSize;
    int virtualCalls;
    int flagWrites;
    int callers;
    int score;
};

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

    std::vector<DbpCandidate*> scored;
    for (auto& [start, c] : candidates) {
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

    size_t deepScan = scored.size() < 10 ? scored.size() : (size_t)10;
    for (size_t i = 0; i < deepScan; i++) {
        scored[i]->callers = countDirectCallers(scored[i]->funcStart, mod, modSize);
    }

    for (auto* cp : scored) {
        DbpCandidate& c = *cp;
        c.score = 0;
        if (c.virtualCalls == 1) c.score += 1000;
        c.score += c.callers * 200;
        if (c.approxSize >= 32 && c.approxSize <= 256) c.score += 100;
        c.score += (c.flagWrites < 3 ? c.flagWrites : 3) * 50;
        if (c.approxSize > 400) c.score -= 200;
    }

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

// BeginDeferredActorSpawnFromClass param offsets, derived from the UFunction property chain.
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

    refreshWorld();
    if (!s_world) {
        Hydro::logError("EngineAPI: No valid UWorld found");
        return nullptr;
    }

    Hydro::logInfo("EngineAPI: Spawning actor at (%.0f, %.0f, %.0f) in world %p...", x, y, z, s_world);

    resolveSpawnLayout();

    if (s_spawnLayout.resolved && s_spawnLayout.totalSize <= 256) {
        // WorldContext must be an in-world actor instance. UWorld* itself and CDOs fail
        // GetWorldFromContextObject() on some UE 5.5 builds.
        // RF_ClassDefaultObject = 0x10 (stable across UE versions).
        auto isCDO = [](void* obj) {
            if (!obj) return true;
            uint32_t flags = 0;
            if (!safeReadInt32((uint8_t*)obj + UOBJ_FLAGS, (int32_t*)&flags)) return true;
            return (flags & 0x10) != 0;  // RF_ClassDefaultObject
        };

        void* worldContext = nullptr;
        const char* wcSource = nullptr;

        if (s_guObjectArray) {
            int32_t count = getObjectCount();
            for (int32_t i = 0; i < count && !worldContext; i++) {
                void* obj = getObjectAt(i);
                if (!obj || isCDO(obj)) continue;
                void* cls = getObjClass(obj);
                if (!cls) continue;
                if (!isActorSubclass(cls)) continue;
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
            wcSource = "UWorld* (last resort)";
        }
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
        // FTransform layout (stable across UE 5.x, double-precision):
        //   FQuat at +0x00 (32 bytes), Translation at +0x20, Scale3D at +0x40
        alignas(16) uint8_t params[256] = {};
        *(void**)(params + s_spawnLayout.worldOff) = worldContext;
        *(void**)(params + s_spawnLayout.classOff) = actorClass;
        uint8_t* tx = params + s_spawnLayout.transformOff;
        *(double*)(tx + 0x00) = 0.0;  // QuatX
        *(double*)(tx + 0x08) = 0.0;  // QuatY
        *(double*)(tx + 0x10) = 0.0;  // QuatZ
        *(double*)(tx + 0x18) = 1.0;  // QuatW (identity)
        *(double*)(tx + 0x20) = x;
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

    // Fallback: hardcoded UE 5.5 layout.
    Hydro::logWarn("EngineAPI: SpawnParams reflection unavailable; using hardcoded UE 5.5 layout");
    SpawnParams sp = {};
    sp.worldContext = s_world;
    sp.actorClass = actorClass;
    sp.rotX = 0.0; sp.rotY = 0.0; sp.rotZ = 0.0; sp.rotW = 1.0;
    sp.locX = x; sp.locY = y; sp.locZ = z;
    sp.scaleX = 1.0; sp.scaleY = 1.0; sp.scaleZ = 1.0;
    sp.collisionMethod = 1;  // AlwaysSpawn
    sp.owner = nullptr;
    sp.scaleMethod = 1;  // MultiplyWithRoot
    sp.returnValue = nullptr;

    if (!callProcessEvent(s_gameplayStaticsCDO, s_spawnFunc, &sp) || !sp.returnValue) {
        Hydro::logError("EngineAPI: BeginDeferredActorSpawnFromClass failed");
        return nullptr;
    }

    if (s_finishSpawnFunc) {
        alignas(16) uint8_t fsParams[512] = {};
        *(void**)(fsParams + 0x00) = sp.returnValue;
        *(double*)(fsParams + 0x10) = 0.0;
        *(double*)(fsParams + 0x18) = 0.0;
        *(double*)(fsParams + 0x20) = 0.0;
        *(double*)(fsParams + 0x28) = 1.0;
        *(double*)(fsParams + 0x30) = x;
        *(double*)(fsParams + 0x38) = y;
        *(double*)(fsParams + 0x40) = z;
        *(double*)(fsParams + 0x50) = 1.0;
        *(double*)(fsParams + 0x58) = 1.0;
        *(double*)(fsParams + 0x60) = 1.0;
        *(uint8_t*)(fsParams + 0x70) = 1;  // MultiplyWithRoot

        bool fsOk = callProcessEvent(s_gameplayStaticsCDO, s_finishSpawnFunc, fsParams);
        if (!fsOk)
            Hydro::logWarn("EngineAPI: FinishSpawningActor failed");
    }

    return sp.returnValue;
}

// Player / actor lookups via GameplayStatics UFunctions
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

    // Params: WorldContext(0x00), ActorClass(0x08), OutActors TArray(0x10).
    // Zero TArray - UE allocates the backing buffer internally.
    alignas(16) uint8_t params[256] = {};
    *(void**)(params + 0x00) = s_world;
    *(void**)(params + 0x08) = actorClass;

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

    // Walk ChildProperties, then up the SuperStruct chain. Uses discovered Layer 2
    // offsets (fallback to hardcoded) to handle UE fork shifts.
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

bool readPtr(void* addr, void** out) {
    return safeReadPtr(addr, out);
}

bool readInt32(void* addr, int32_t* out) {
    return safeReadInt32(addr, out);
}

constexpr int FFIELD_CLASS = 0x08;  // FField::ClassPrivate (FFieldClass*)

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
//
// Three optimisations stack here:
//   1. Resolve className -> UClass* once, then compare obj->class pointers
//      instead of reading each obj->class->name every iteration.
//   2. Raw reads in the hot loop under one SEH block, not per-read. The
//      UOBJ_CLASS/UOBJ_NAME offsets are validated at bootstrap.
//   3. Cache the last found instance. A warm call only reads instance->class
//      to validate, then returns immediately.
// Cold call ~10ms, warm call ~nanoseconds (was ~1s).

struct FindCacheEntry {
    uint32_t targetNameIdx = 0;
    void* uclass = nullptr;        // resolved UClass* (set on first successful scan)
    void* lastInstance = nullptr;  // most recent successful lookup
};
static std::unordered_map<std::wstring, FindCacheEntry> s_findCache;

static bool validateCachedInstance(void* instance, void* expectedClass) {
    if (!instance || !expectedClass) return false;
    void* cls = nullptr;
    if (!safeReadPtr((uint8_t*)instance + UOBJ_CLASS, &cls)) return false;
    return cls == expectedClass;
}

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
    } __except (1) { /* fault mid-scan - return what we have */ }
#endif
    return found;
}

static void* s_actorClassPtr = nullptr;  // Cached AActor UClass*

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

// Find a non-CDO actor instance for use as UFunction WorldContext.
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

// FProperty::PropertyFlags fallback offset (stock UE 5.5; runtime-discovered value takes precedence).
constexpr int FPROP_FLAGS = 0x38;

// Reflection-driven FProperty layout discovery.
// Probes FProperty::Offset_Internal, ElementSize, and PropertyFlags offsets
// against AActor anchors (PrimaryActorTick, RootComponent, Tags). Results
// live in s_layout; hardcoded constants are fallbacks.

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

    // Use GUObjectArray scan rather than StaticFindObject - Palworld returns
    // the wrong object (/Script/AssetRegistry UPackage) for /Script/Engine.Actor.
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

    // Stage A: pre-resolve known AActor property name FName indices.
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
    Hydro::logInfo("EngineAPI: Layer 2 expected names (%d resolved):", expectedReady);
    for (int i = 0; i < expectedReady; i++) {
        Hydro::logInfo("  '%ls' = idx %u", kKnownActorProps[i], expectedNames[i]);
    }

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

    Hydro::logInfo("EngineAPI: Layer 2 actorClass=%p; full UClass slot dump:", actorClass);
    for (int off = 0x00; off <= 0x180; off += 8) {
        void* p = nullptr;
        if (!safeReadPtr((uint8_t*)actorClass + off, &p)) {
            Hydro::logInfo("  +0x%02X: <unreadable>", off);
            continue;
        }
        uintptr_t v = (uintptr_t)p;
        bool inMod = ((uint8_t*)p >= s_gm.base && (uint8_t*)p < s_gm.base + s_gm.size);
        uint32_t lo = (uint32_t)(v & 0xFFFFFFFFULL);
        uint32_t hi = (uint32_t)(v >> 32);
        bool heapLike = !inMod && hi != 0 && lo != 0 && hi < 0x10000U;
        const char* tag = inMod ? " <module>" : (heapLike ? " <heap>" : "");
        Hydro::logInfo("  +0x%02X: %p%s", off, p, tag);
    }

    // Stage B: probe (USTRUCT_CHILD_PROPS, FFIELD_NEXT, FFIELD_NAME) triples.
    // Stock UE 5.5: (0x50, 0x18, 0x20). Palworld fork: (0x58, 0x20, 0x28).
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

    // Stage C: with discovered chain offsets, find anchor properties.
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

    // Stage D: probe FPROP_OFFSET_INTERNAL - must be 4-byte aligned, < 8KB, distinct per anchor.
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

    // Stage E: probe FPROP_ELEMENT_SIZE - RootComponent=8, Tags=16.
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

    // Stage F: probe FPROP_FLAGS (uint64) - must have CPF_Edit, clear CPF_Parm, clear top byte.
    // Diversity + richness checks rule out adjacent fields that read similarly across properties.
    static const int kFlags[] = { 0x38, 0x40, 0x30, 0x48, 0x44 };
    constexpr uint64_t CPF_Edit_local         = 0x0000000000000001ULL;
    constexpr uint64_t CPF_Parm_local         = 0x0000000000000080ULL;
    constexpr uint64_t CPF_NoClear_local      = 0x0000000000100000ULL;
    auto looksLikePropertyFlags = [&](uint64_t v) {
        if (v == 0 || v == ~0ULL) return false;
        if ((v >> 56) != 0) return false;        // CPF_* fit in 56 bits
        if (!(v & CPF_Edit_local)) return false; // member must have Edit bit
        if (v & CPF_Parm_local) return false;    // not a parameter
        return true;
    };
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

        // Diversity and richness checks.
        if ((rootProp && vTick == vRoot) || (tagsProp && vTick == vTags) ||
            (rootProp && tagsProp && vRoot == vTags)) continue;
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

// FNamePool direct reading - fastest name resolution path.
// Reads directly from the engine's FNamePool block table using FNameEntry layout:
//   Header (uint16): bit 0 = bIsWide, bits [6..15] = Len; followed by char/wchar_t Name[Len].
// ComparisonIndex: Block = idx >> 16, EntryOffset = (idx & 0xFFFF) * 2.

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

    // Strategy 1: walk the FName constructor's call tree (2 levels deep).
    // The pool global is accessed via LEA or MOV [rip+disp] in a sub-function.
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

    // Strategy 2: data section scan for FNameEntryAllocator signature.
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

// Conv_NameToString fallback - used only if FNamePool direct reading fails.

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

    if (comparisonIndex > 0x7FFFFFFF) return {}; // reject garbage pointer values

    uint16_t parmsSize = getUFunctionParmsSize(s_convNameToStringFunc);
    uint16_t retOff    = getUFunctionRetOffset(s_convNameToStringFunc);

    if (parmsSize > 256 || parmsSize == 0) return {};

    alignas(16) uint8_t params[256] = {};
    memcpy(params, &comparisonIndex, 4); // FName = { ComparisonIndex, Number }
    uint32_t number = 0;
    memcpy(params + 4, &number, 4);

    bool ok = callProcessEvent(s_kismetStringCDO, s_convNameToStringFunc, params);
    if (!ok) return {};

    // Read FString (TCHAR* Data, int32 ArrayNum, int32 ArrayMax) from retOff.
    void* data = nullptr;
    int32_t arrayNum = 0;
    memcpy(&data, params + retOff, sizeof(void*));
    memcpy(&arrayNum, params + retOff + sizeof(void*), 4);

    if (!data || arrayNum <= 0) return {};

    int32_t len = arrayNum - 1; // arrayNum includes null terminator
    if (len <= 0 || len > 1024) return {};

    std::string result;
    result.reserve(len);
    const wchar_t* wdata = (const wchar_t*)data;
    for (int32_t i = 0; i < len; i++) {
        result += (char)(wdata[i] & 0x7F);
    }

    // FString Data is GMalloc-owned; not freed here (leak bounded by cache).
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

// FField name offset: discovered once at first call via PrimaryActorTick probe.
static int s_ffieldNameOffset = -1;

static void discoverFieldNameOffset() {
    void* actorClass = findObject(L"/Script/Engine.Actor");
    if (!actorClass) return;

    uint32_t expectedIdx = makeFName(L"PrimaryActorTick");
    if (expectedIdx == 0) return;

    void* prop = findProperty(actorClass, L"PrimaryActorTick");
    if (!prop) return;

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

// SuperStruct offset: probes until a candidate dereferences to name "Object".
static void discoverSuperOffset() {
    void* actorClass = findObject(L"/Script/Engine.Actor");
    if (!actorClass) { s_superOffset = 0x40; return; }

    uint32_t objectIdx = makeFName(L"Object");
    if (objectIdx == 0) { s_superOffset = 0x40; return; }

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

// Layer 3: UFunction parameter layout from the property chain.
// ParmsSize = max(offset + elementSize), ReturnValueOffset = CPF_ReturnParm property's offset.
// UFUNC_PARMS_SIZE / UFUNC_RET_VAL_OFFSET constants are bootstrap fallbacks only.

struct UFuncLayoutCache {
    uint16_t parmsSize;
    uint16_t retOffset;
    bool     hasReturn;
    bool     derived;     // true = from chain, false = raw fallback read
};
static std::unordered_map<void*, UFuncLayoutCache> s_ufuncLayouts;

static void clearUFuncLayoutCache() { s_ufuncLayouts.clear(); }

static UFuncLayoutCache computeUFunctionLayout(void* ufunc) {
    UFuncLayoutCache out = {0, 0, false, false};
    if (!ufunc) return out;

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

    void* prop = getChildProperties(ufunc);

    Hydro::logInfo("EngineAPI: computeUFunctionLayout %p chain (firstProp=%p):", ufunc, prop);

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

    // Fallback to raw hardcoded offsets if chain walk yielded nothing.
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

uint16_t getUFunctionParmsSize(void* ufunc) {
    if (!ufunc) return 0;
    return getUFuncLayoutCached(ufunc).parmsSize;
}

uint16_t getUFunctionRetOffset(void* ufunc) {
    if (!ufunc) return 0;
    return getUFuncLayoutCached(ufunc).retOffset;
}

// Layer 4: per-class named property offset cache.
// Replaces hardcoded class-level offsets (e.g., AActor::RootComponent at +0x198).
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

// UClass::ClassFlags offset: discovered once via /Script/CoreUObject.Object (must have CLASS_Native set).
static int s_classFlagsOffset = -1;

static void discoverClassFlagsOffset() {
    void* uobjectClass = findObject(L"/Script/CoreUObject.Object");
    if (!uobjectClass) { s_classFlagsOffset = 0x1C0; return; }

    static const int kOffsets[] = {
        0x1C0, 0x1B0, 0x1C8, 0x1D0, 0x1B8, 0x1A8, 0x1E0, 0x1E8, 0x1F0, 0x1F8, 0x200
    };
    for (int off : kOffsets) {
        uint32_t flags = 0;
        if (!safeReadInt32((uint8_t*)uobjectClass + off, (int32_t*)&flags)) continue;
        if ((flags & 0x1) == 0) continue;         // must have CLASS_Native
        if (flags == 0xFFFFFFFF || flags == 0) continue;
        if (flags > 0x10000000) continue;          // CLASS_* fit in 28 bits
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

// UEnum::Names offset: discovered by probing for TArray<TPair<FName, int64>> (16B stride).
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

    static const int kOffsets[] = { 0x40, 0x48, 0x50, 0x58, 0x38, 0x60, 0x68 };
    for (int off : kOffsets) {
        void* data = nullptr;
        if (!safeReadPtr((uint8_t*)enumProbe + off, &data) || !data) continue;
        if ((uint8_t*)data >= s_gm.base && (uint8_t*)data < s_gm.base + s_gm.size) continue;

        int32_t num = 0, max = 0;
        if (!safeReadInt32((uint8_t*)enumProbe + off + 8, &num)) continue;
        if (!safeReadInt32((uint8_t*)enumProbe + off + 12, &max)) continue;
        if (num < 1 || num > 10000) continue;
        if (max < num) continue;

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

// OuterPrivate offset: discovered once via AActor (outer should name "Engine").
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

    // Walk the Outer chain, then reconstruct UE's GetPathName separator rules.
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

    // Separator: ':' when parent is non-package and grandparent is package
    // (SUBOBJECT_DELIMITER); '.' otherwise.
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


// FShaderCodeLibrary::OpenLibrary discovery via "FShaderCodeLibrary::OpenLibrary"
// string xref + .pdata-based containing-function lookup.

static bool findOpenShaderLibrary() {
    if (!s_gm.base) return false;

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

    uint8_t* leaAddr = findLeaRef(s_gm.base, s_gm.size, strAddr);
    if (!leaAddr) {
        Hydro::logWarn("EngineAPI: No LEA ref to OpenLibrary string");
        return false;
    }
    Hydro::logInfo("EngineAPI: LEA at exe+0x%zX", (size_t)(leaAddr - s_gm.base));

    // Locate enclosing function via .pdata (binary search on sorted RUNTIME_FUNCTION table).
    // More reliable than prologue-pattern walk-back, which overshoots on SSE-prologue functions.
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

// FString layout: TCHAR* Data, int32 ArrayNum (including null), int32 ArrayMax.
struct FStringMinimal {
    wchar_t* Data;
    int32_t ArrayNum;
    int32_t ArrayMax;
};

// GMalloc: obtained via patternsleuth. FString Data buffers passed to OpenLibrary
// must be GMalloc-owned or UE crashes when it tries to FMemory::Free them.
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
    void** gmallocPtr = (void**)results.gmalloc; // patternsleuth gives the variable, dereference for instance
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

// FMalloc::Malloc vtable index: probe rather than hardcode (slot shifts between dev/shipping).
using FMallocFn = void*(__fastcall*)(void* self, size_t size, uint32_t align);

static int s_mallocVtableIdx = -1;  // cached after first probe

static void* gmallocAlloc(size_t size, uint32_t align = 8) {
    if (!s_gmalloc) {
        if (!findGMalloc()) return nullptr;
    }
    void** vtable = nullptr;
    if (!safeReadPtr(s_gmalloc, (void**)&vtable) || !vtable) return nullptr;

    if (s_mallocVtableIdx < 0) {
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
            break; // test alloc not freed (haven't probed Free yet); 16-byte leak acceptable
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

// Allocate and populate an FString via GMalloc (required for OpenLibrary callers).
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

    FStringMinimal libString = {};
    FStringMinimal dirString = {};
    if (!buildFString(libString, libraryName) || !buildFString(dirString, mountDir)) {
        Hydro::logWarn("EngineAPI: OpenShaderLibrary skipped - couldn't allocate FString via GMalloc");
        return false;
    }

    Hydro::logInfo("EngineAPI: OpenShaderLibrary(name='%ls' @ %p, dir='%ls' @ %p)",
        libraryName, libString.Data, mountDir, dirString.Data);

    // Signature: bool OpenLibrary(FString const& Name, FString const& Directory, bool bMonolithicOnly).
    // bMonolithicOnly must be explicit (r8b); garbage register caused monolithic-only crashes.
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

// AssetRegistry::ScanPathsSynchronous via reflected UFUNCTION (ProcessEvent).
// Param layout: TArray<FString> at +0x00 (16B), bForceRescan at +0x10, bIgnoreDenyListScanFilters at +0x11.

bool scanAssetRegistryPaths(const wchar_t* virtualPath, bool forceRescan) {
    if (!s_processEvent || !virtualPath) return false;
    if (!s_assetRegImpl) {
        Hydro::logWarn("EngineAPI: scanAssetRegistryPaths skipped - AssetRegistry impl not discovered");
        return false;
    }

    // Interface class via K2_GetAssetByObjectPath's outer (Palworld: no UFunctions on impl class).
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

    FStringMinimal pathStr = {};
    if (!buildFString(pathStr, virtualPath)) {
        Hydro::logWarn("EngineAPI: scanAssetRegistryPaths - failed to allocate FString");
        return false;
    }

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
    *(bool*)(params + 0x11) = false;                          // bIgnoreDenyListScanFilters

    Hydro::logInfo("EngineAPI: ScanPathsSynchronous('%ls', forceRescan=%d) ParmsSize=%u",
                   virtualPath, forceRescan ? 1 : 0, parmsSize);

    bool ok = callFunction(s_assetRegImpl, scanFn, params);
    Hydro::logInfo("EngineAPI: ScanPathsSynchronous returned %s", ok ? "true" : "false");
    return ok;
}

// AssetRegistry::ScanFilesSynchronous - takes a filesystem .uasset path.
// Param layout: TArray<FString> at +0x00, bool bForceRescan at +0x10.

bool scanAssetRegistryFiles(const wchar_t* uassetFilename, bool forceRescan) {
    if (!s_processEvent || !uassetFilename) return false;
    if (!s_assetRegImpl) {
        Hydro::logWarn("EngineAPI: scanAssetRegistryFiles skipped - AssetRegistry impl not discovered");
        return false;
    }

    void* arInterfaceClass = nullptr;
    if (s_getByPathFunc) arInterfaceClass = getOuter(s_getByPathFunc);
    if (!arInterfaceClass) arInterfaceClass = findObject(L"/Script/AssetRegistry.AssetRegistry");
    void* arImplClass = getClass(s_assetRegImpl);

    void* scanFn = nullptr;
    if (arInterfaceClass) scanFn = findFunction(arInterfaceClass, L"ScanFilesSynchronous");
    if (!scanFn && arImplClass) scanFn = findFunction(arImplClass, L"ScanFilesSynchronous");
    if (!scanFn) scanFn = findObject(L"/Script/AssetRegistry.AssetRegistry:ScanFilesSynchronous");
    if (!scanFn) scanFn = findObject(L"/Script/AssetRegistry.AssetRegistryImpl:ScanFilesSynchronous");
    if (!scanFn) {
        Hydro::logWarn("EngineAPI: scanAssetRegistryFiles - ScanFilesSynchronous UFunction not found");
        return false;
    }

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

    // Throttle: only retry every ~2 seconds (GUObjectArray walk is expensive).
    static uint64_t s_lastAttempt = 0;
    uint64_t now = GetTickCount64();
    if (now - s_lastAttempt < 2000) return;
    s_lastAttempt = now;

    if (!s_fnameConstructor || !s_guObjectArray) return;

    if (discoverAssetRegistry() && s_assetRegHelpersCDO && s_getAssetFunc) {
        Hydro::logInfo("EngineAPI: AssetRegistry deferred-init succeeded after %llu ms",
                       now - s_lastAttempt);
    }
}

} // namespace Hydro::Engine
