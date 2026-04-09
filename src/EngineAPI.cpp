#include "EngineAPI.h"
#include "HydroCore.h"
#include "SEH.h"
#include <cstdio>
#include <cstring>
#include <string>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif

/**
 * EngineAPI - Pure C++ engine reflection. ZERO UE4SS headers.
 *
 * Bootstrap sequence:
 *   1. Find game module (largest loaded DLL/exe)
 *   2. Pattern scan for GUObjectArray ("Unable to add more objects" string)
 *   3. Pattern scan for StaticLoadObject ("Failed to find object" string)
 *   4. Pattern scan for FName::ToString ("None" string + characteristic code)
 *   5. Get ProcessEvent from first UObject's vtable at offset 0x278
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
static bool findFNameConstructor();
static bool discoverAssetRegistry();
static bool callProcessEvent(void* obj, void* func, void* params);

// Cached state

static bool s_ready = false;
static GameModule s_gm = {};

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

// AssetRegistry loading (the PROVEN path - same as BPModLoaderMod)
static void* s_fnameConstructor = nullptr;     // FName(const TCHAR*, EFindName) function
static void* s_assetRegHelpersCDO = nullptr;   // Default__AssetRegistryHelpers
static void* s_getAssetFunc = nullptr;         // AssetRegistryHelpers:GetAsset UFunction
static void* s_assetRegImpl = nullptr;         // AssetRegistry implementation object
static void* s_getByPathFunc = nullptr;        // AssetRegistry:GetAssetByObjectPath UFunction

// Variables used by both cache code and pattern scans
static void* s_realStaticFindObject = nullptr;
static void* s_loadPackage = nullptr;
static void* s_staticFindObject = nullptr;

// Pattern scan cache
// Caches function offsets (relative to game module base) so we skip the
// expensive 6+ second pattern scan on subsequent launches.
// Cache is invalidated when the game binary size changes (i.e., game update).

struct ScanCache {
    uint32_t magic;               // 0x48594452 = "HYDR"
    size_t moduleSize;            // Game module size - cache key
    ptrdiff_t staticLoadObject;   // StaticLoadObject offset from base
    ptrdiff_t fnameConstructor;   // FName constructor offset from base
    ptrdiff_t guObjectArray;      // GUObjectArray offset from base
    ptrdiff_t realStaticFindObject; // Real StaticFindObject offset
    ptrdiff_t loadPackage;        // LoadPackageInternal offset
};

static const uint32_t CACHE_MAGIC = 0x48594452;

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

                if ((uint8_t*)resolved >= s_gm.base && (uint8_t*)resolved < s_gm.base + s_gm.size) {
                    int32_t numElems = 0;
                    if (safeReadInt32((uint8_t*)resolved + FARRAY_NUM_ELEMS, &numElems) &&
                        numElems > 1000 && numElems < 500000) {
                        s_guObjectArray = resolved;
                        Hydro::logInfo("EngineAPI: GUObjectArray at %p (%d objects) - found via caller at exe+0x%zX",
                            resolved, numElems, i);
                        return true;
                    }
                }
            }
        }
    }

    Hydro::logError("EngineAPI: GUObjectArray caller not found");
    return false;
}

// GUObjectArray iteration

// Read a UObject* from GUObjectArray at the given index
static void* getObjectAt(int32_t index) {
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

static int32_t getObjectCount() {
    if (!s_guObjectArray) return 0;
    int32_t count = 0;
    safeReadInt32((uint8_t*)s_guObjectArray + FARRAY_NUM_ELEMS, &count);
    return count;
}

// Read the FName ComparisonIndex from a UObject (offset 0x18, first 4 bytes)
static uint32_t getNameIndex(void* obj) {
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
static void* getObjClass(void* obj) {
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

static bool findStaticLoadObject() {
    // "Failed to find object" is in StaticFindObject (find-only, no disk loading).
    // Save it as a useful bonus, but we need the REAL loader.
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

    // Now find the REAL StaticLoadObject - it calls LoadPackage which
    // references "Can't find file for package" or "CreateLinker" strings.
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
    if (!s_staticLoadObject) return false;

    auto loadObj = (StaticLoadObjectFn_t)s_staticLoadObject;

    // Find the live UWorld instance by loading UWorld class and scanning.
    void* worldClass = nullptr;
#ifdef _WIN32
    __try {
        worldClass = loadObj(nullptr, nullptr, L"/Script/Engine.World", nullptr, 0, nullptr, true, nullptr);
    } __except(1) { worldClass = nullptr; }
#endif

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

    // Find GameplayStatics CDO + spawn UFunctions
    // StaticLoadObject can find UFunctions by full path - no iteration needed!
#ifdef _WIN32
    __try {
        void* gsClass = loadObj(nullptr, nullptr, L"/Script/Engine.GameplayStatics", nullptr, 0, nullptr, true, nullptr);
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

    // Load spawn UFunctions directly by path
    __try {
        s_spawnFunc = loadObj(nullptr, nullptr,
            L"/Script/Engine.GameplayStatics:BeginDeferredActorSpawnFromClass",
            nullptr, 0, nullptr, true, nullptr);
    } __except(1) { s_spawnFunc = nullptr; }

    __try {
        s_finishSpawnFunc = loadObj(nullptr, nullptr,
            L"/Script/Engine.GameplayStatics:FinishSpawningActor",
            nullptr, 0, nullptr, true, nullptr);
    } __except(1) { s_finishSpawnFunc = nullptr; }
#endif

    if (s_spawnFunc)
        Hydro::logInfo("EngineAPI: SpawnFunc at %p", s_spawnFunc);
    if (s_finishSpawnFunc)
        Hydro::logInfo("EngineAPI: FinishSpawnFunc at %p", s_finishSpawnFunc);

    bool ready = s_world && s_gameplayStaticsCDO && s_spawnFunc;
    if (!ready)
        Hydro::logWarn("EngineAPI: Missing: world=%p gs=%p spawn=%p",
            s_world, s_gameplayStaticsCDO, s_spawnFunc);
    return ready;
}

// Public API: initialize

bool initialize() {
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

    // Step 2: SAFE TEST - call StaticLoadObject with a known safe path
    Hydro::logInfo("EngineAPI: Testing StaticLoadObject call...");
    {
        auto fn = (StaticLoadObjectFn_t)s_staticLoadObject;
        void* anyObj = nullptr;
#ifdef _WIN32
        __try {
            anyObj = fn(nullptr, nullptr, L"/Script/CoreUObject.Object", nullptr, 0, nullptr, true, nullptr);
        } __except(1) {
            Hydro::logError("EngineAPI: StaticLoadObject CRASHED - wrong function found");
            s_staticLoadObject = nullptr;
            return false;
        }
#endif
        if (anyObj) {
            Hydro::logInfo("EngineAPI: StaticLoadObject works! UObject class at %p", anyObj);
            void** vtable = nullptr;
            if (safeReadPtr(anyObj, (void**)&vtable) && vtable) {
                void* pe = nullptr;
                if (safeReadPtr(&vtable[VTABLE_PROCESS_EVENT], &pe) && pe &&
                    (uint8_t*)pe >= s_gm.base && (uint8_t*)pe < s_gm.base + s_gm.size) {
                    s_processEvent = (ProcessEventFn)pe;
                    Hydro::logInfo("EngineAPI: ProcessEvent at %p", pe);
                }
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
    {
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
                    Hydro::logInfo("EngineAPI: StaticFindObjectFast at exe+0x%zX",
                        (size_t)(findObjFast - s_gm.base));

                    // StaticFindObject is the largest moderate-sized caller of
                    // StaticFindObjectFast (it also calls ResolveName).
                    int callerCount = 0;
                    struct Candidate { uint8_t* addr; size_t size; };
                    Candidate best = {nullptr, 0};

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

                        // Estimate size
                        size_t funcSize = 0;
                        for (size_t off = 0; off < 8192; off++) {
                            if (funcStart[off] == 0xCC && funcStart[off+1] == 0xCC) { funcSize = off; break; }
                        }

                        // StaticFindObject is 200-800 bytes with multiple calls
                        if (funcSize > 100 && funcSize < 1000 && funcSize > best.size) {
                            best = {funcStart, funcSize};
                        }
                    }

                    Hydro::logInfo("EngineAPI: Found %d callers of StaticFindObjectFast", callerCount);

                    if (best.addr) {
                        s_realStaticFindObject = best.addr;
                        Hydro::logInfo("EngineAPI: Real StaticFindObject at exe+0x%zX (size ~%zu)",
                            (size_t)(best.addr - s_gm.base), best.size);
                    }
                }
            }
        }
        if (!s_realStaticFindObject)
            Hydro::logWarn("EngineAPI: Real StaticFindObject not found");
    }

    // Step 6: Find FName constructor (needed for AssetRegistry calls)
    if (!findFNameConstructor()) {
        Hydro::logWarn("EngineAPI: FName constructor not found - asset loading disabled");
    }

    // Save scan cache for next launch
    saveScanCache(s_gm);
    } // end !fromCache (pattern scans)

    // Runtime discovery (always runs - heap objects change each launch)

    // Step 7: Discover AssetRegistry objects
    if (s_fnameConstructor) {
        if (!discoverAssetRegistry()) {
            Hydro::logWarn("EngineAPI: AssetRegistry not fully initialized");
        }
    }

    // Step 6: Discover engine objects (UWorld, GameplayStatics, spawn functions)
    if (!discoverEngineObjects()) {
        Hydro::logWarn("EngineAPI: Some engine objects not found - spawning may fail");
    }

    s_ready = (s_processEvent != nullptr);
    Hydro::logInfo("EngineAPI: Bootstrap %s", s_ready ? "COMPLETE" : "PARTIAL");
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

    // Walk UClass::Children (UField* linked list at offset 0x48)
    void* child = nullptr;
    safeReadPtr((uint8_t*)uclass + USTRUCT_CHILDREN, &child);

    int count = 0;
    while (child && count < 500) {
        // Read this child's FName (at +0x18, same as UObject::NamePrivate)
        uint32_t nameIdx = 0;
        safeReadInt32((uint8_t*)child + UOBJ_NAME, (int32_t*)&nameIdx);

        if (nameIdx == targetName.comparisonIndex) {
            // Verify it has a valid Func pointer (native implementation)
            void* funcPtr = nullptr;
            safeReadPtr((uint8_t*)child + UFUNC_FUNC, &funcPtr);

            Hydro::logInfo("EngineAPI: Found '%ls' on UClass at %p (Func=%p)", funcName, child, funcPtr);
            return child;
        }

        // Next UField (at offset 0x28)
        void* next = nullptr;
        safeReadPtr((uint8_t*)child + UFIELD_NEXT, &next);
        child = next;
        count++;
    }

    return nullptr;
}

// AssetRegistry loading

static bool discoverAssetRegistry() {
    if (!s_fnameConstructor) return false;

    // StaticFindObject: UObject*(UClass*, UObject*, const TCHAR*, bool)
    using FindObjFn = void*(__fastcall*)(void*, void*, const wchar_t*, bool);

    auto find = [](const wchar_t* path) -> void* {
        // Use the REAL StaticFindObject (with proper path resolution) if available
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
        // Fallback to old broken one only if real one isn't available yet
        if (s_staticFindObject)
            return safeCallLoadObject(s_staticFindObject, path, 0);
        return nullptr;
    };

    // Find AssetRegistryHelpers CDO
    s_assetRegHelpersCDO = find(L"/Script/AssetRegistry.Default__AssetRegistryHelpers");
    if (s_assetRegHelpersCDO)
        Hydro::logInfo("EngineAPI: AssetRegistryHelpers CDO at %p", s_assetRegHelpersCDO);

    // Find GetAsset UFunction via UClass function chain (same as UE4SS's GetFunctionByNameInChain)
    // This finds the CORRECT UFunction with the proper native Func pointer.
    if (s_assetRegHelpersCDO) {
        void* cdoClass = nullptr;
        safeReadPtr((uint8_t*)s_assetRegHelpersCDO + UOBJ_CLASS, &cdoClass);
        if (cdoClass) {
            void* oldGetAsset = find(L"/Script/AssetRegistry.AssetRegistryHelpers:GetAsset");
            s_getAssetFunc = findFunctionOnClass(cdoClass, L"GetAsset");
            if (s_getAssetFunc) {
                uint16_t ps = *(uint16_t*)((uint8_t*)s_getAssetFunc + UFUNC_PARMS_SIZE);
                uint16_t ro = *(uint16_t*)((uint8_t*)s_getAssetFunc + UFUNC_RET_VAL_OFFSET);
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

void* findObject(const wchar_t* path) {
    if (!path) return nullptr;

    // Try the REAL StaticFindObject first (handles /Game/ paths with path resolution)
    if (s_realStaticFindObject) {
        auto fn = (StaticFindObjectFn)s_realStaticFindObject;
        void* result = nullptr;
#ifdef _WIN32
        __try { result = fn(nullptr, nullptr, path, false); }
        __except(1) { result = nullptr; }
#endif
        if (result) return result;
    }

    // Fallback to old function (works for /Script/ paths)
    if (s_staticFindObject)
        return safeCallLoadObject(s_staticFindObject, path, 0);

    return nullptr;
}

// Public API: loadAsset (via AssetRegistry GetAsset)
//
// Uses the DOCUMENTED FAssetData contract: PackageName + AssetName uniquely
// identify an asset (operator==, GetTypeHash, IsValid all confirm this).
// FastGetAsset only uses these two fields - no other state needed.
//
// This is the same approach BPModLoaderMod uses in production.

void* loadAsset(const wchar_t* assetPath) {
    if (!assetPath || !s_fnameConstructor || !s_processEvent) return nullptr;
    if (!s_assetRegHelpersCDO || !s_getAssetFunc) {
        Hydro::logError("EngineAPI: AssetRegistry not initialized");
        return nullptr;
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

    // Read GetAsset's exact param layout from the UFunction
    uint16_t gaParmsSize = *(uint16_t*)((uint8_t*)s_getAssetFunc + UFUNC_PARMS_SIZE);
    uint16_t gaRetOffset = *(uint16_t*)((uint8_t*)s_getAssetFunc + UFUNC_RET_VAL_OFFSET);

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

                // Walk the struct's property chain (ChildProperties at +0x50)
                void* prop = nullptr;
                safeReadPtr((uint8_t*)assetDataStruct + UFUNC_CHILD_PROPS, &prop);
                int fieldIdx = 0;
                while (prop && fieldIdx < 15) {
                    int32_t offset = 0, size = 0;
                    uint32_t nameIdx = 0;
                    if (!safeReadInt32((uint8_t*)prop + FPROP_OFFSET_INTERNAL, &offset)) break;
                    safeReadInt32((uint8_t*)prop + FPROP_ELEMENT_SIZE, &size);
                    // FField::NamePrivate at +0x20 in UE 5.5
                    safeReadInt32((uint8_t*)prop + 0x20, (int32_t*)&nameIdx);

                    Hydro::logInfo("EngineAPI:   Field[%d]: nameIdx=%u offset=0x%X size=%d",
                        fieldIdx, nameIdx, offset, size);

                    fieldIdx++;
                    // FField::Next at +0x18 in UE 5.5
                    void* next = nullptr;
                    safeReadPtr((uint8_t*)prop + 0x18, &next);
                    prop = next;
                }
            } else {
                Hydro::logWarn("EngineAPI: Could not find FAssetData UScriptStruct");
            }
        }
    }

    // Use hardcoded offsets (verified against C++ source) unless we find better
    if (pkgNameOffset < 0) pkgNameOffset = 0x00;
    if (assetNameOffset < 0) assetNameOffset = 0x10;

    uint8_t params[1024] = {};
    memcpy(params + pkgNameOffset, &pkgFName, 8);      // PackageName
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
    } else {
        // Diagnostic: did LoadPackage load the package even though GetAsset returned null?
        void* pkg = safeCallLoadObject(s_staticFindObject, packageName.c_str(), 0);
        Hydro::logInfo("EngineAPI: GetAsset null. Package '%ls' in memory: %p", packageName.c_str(), pkg);
        if (pkg && !assetName.empty()) {
            // Check for asset inside package
            void* obj = safeCallLoadObject(s_staticFindObject, assetPath, 0);
            Hydro::logInfo("EngineAPI: Full path '%ls' in memory: %p", assetPath, obj);
        }
    }

    return loadedAsset;
}

// Legacy loadObject (renamed to findObject internally)

void* loadObject(const wchar_t* path) {
    if (!path) return nullptr;

    // Try StaticLoadObject with different load flags
    // LOAD_NoVerify(0x2) | LOAD_NoWarn(0x8) bypasses engine version checks
    // that silently reject editor-cooked assets in shipping builds
    if (s_staticLoadObject) {
        uint32_t flagSets[] = { 0x0A, 0x00, 0x02, 0x2000 }; // NoVerify|NoWarn, None, NoVerify, Quiet
        for (auto flags : flagSets) {
            void* result = safeCallLoadObject(s_staticLoadObject, path, flags);
            if (result) {
                if (flags != 0) Hydro::logInfo("EngineAPI: Loaded with flags 0x%X", flags);
                return result;
            }
        }
    }

    // Fallback to StaticFindObject (in-memory only)
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
#ifdef _WIN32
    __try {
        pe(obj, func, params);
    } __except(1) {
        crashed = true;
    }
#else
    pe(obj, func, params);
#endif

    if (crashed) {
        Hydro::logError("EngineAPI: ProcessEvent crashed");
        return false;
    }
    return true;
}

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
        uint16_t fsRetOffset = *(uint16_t*)((uint8_t*)s_finishSpawnFunc + UFUNC_RET_VAL_OFFSET);

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

    void* prop = nullptr;
    safeReadPtr((uint8_t*)uclass + USTRUCT_CHILD_PROPS, &prop);

    int count = 0;
    while (prop && count < 500) {
        uint32_t nameIdx = 0;
        safeReadInt32((uint8_t*)prop + FFIELD_NAME, (int32_t*)&nameIdx);

        if (nameIdx == targetName.comparisonIndex) {
            return prop;
        }

        void* next = nullptr;
        safeReadPtr((uint8_t*)prop + FFIELD_NEXT, &next);
        prop = next;
        count++;
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
    int32_t off = 0;
    safeReadInt32((uint8_t*)prop + FPROP_OFFSET_INTERNAL, &off);
    return off;
}

int32_t getPropertyElementSize(void* prop) {
    if (!prop) return 0;
    int32_t sz = 0;
    safeReadInt32((uint8_t*)prop + FPROP_ELEMENT_SIZE, &sz);
    return sz;
}

void* getNextProperty(void* prop) {
    if (!prop) return nullptr;
    void* next = nullptr;
    safeReadPtr((uint8_t*)prop + FFIELD_NEXT, &next);
    return next;
}

void* getChildProperties(void* ustruct) {
    if (!ustruct) return nullptr;
    void* props = nullptr;
    safeReadPtr((uint8_t*)ustruct + USTRUCT_CHILD_PROPS, &props);
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

void* findFirstOf(const wchar_t* className) {
    if (!s_guObjectArray || !s_fnameConstructor || !className) return nullptr;

    FName8 targetName = {};
    if (!safeConstructFName(&targetName, className)) return nullptr;

    int32_t count = getObjectCount();
    for (int32_t i = 0; i < count; i++) {
        void* obj = getObjectAt(i);
        if (!obj) continue;

        void* cls = nullptr;
        safeReadPtr((uint8_t*)obj + UOBJ_CLASS, &cls);
        if (!cls) continue;

        uint32_t classNameIdx = 0;
        safeReadInt32((uint8_t*)cls + UOBJ_NAME, (int32_t*)&classNameIdx);

        if (classNameIdx == targetName.comparisonIndex)
            return obj;
    }
    return nullptr;
}

int findAllOf(const wchar_t* className, void** outArray, int maxResults) {
    if (!s_guObjectArray || !s_fnameConstructor || !className || !outArray || maxResults <= 0)
        return 0;

    FName8 targetName = {};
    if (!safeConstructFName(&targetName, className)) return 0;

    int found = 0;
    int32_t count = getObjectCount();
    for (int32_t i = 0; i < count && found < maxResults; i++) {
        void* obj = getObjectAt(i);
        if (!obj) continue;

        void* cls = nullptr;
        safeReadPtr((uint8_t*)obj + UOBJ_CLASS, &cls);
        if (!cls) continue;

        uint32_t classNameIdx = 0;
        safeReadInt32((uint8_t*)cls + UOBJ_NAME, (int32_t*)&classNameIdx);

        if (classNameIdx == targetName.comparisonIndex)
            outArray[found++] = obj;
    }
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

// FProperty::PropertyFlags offset (uint64 at +0x38 in FProperty)
constexpr int FPROP_FLAGS = 0x38;

uint64_t getPropertyFlags(void* prop) {
    if (!prop) return 0;
    uint64_t flags = 0;
    memcpy(&flags, (uint8_t*)prop + FPROP_FLAGS, 8);
    return flags;
}

} // namespace Hydro::Engine
