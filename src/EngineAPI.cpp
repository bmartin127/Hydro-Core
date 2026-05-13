#include "EngineAPI.h"
#include "HydroCore.h"
#include "RawFunctions.h"
#include "SEH.h"
#include "FArchiveLoader.h"
#include "PakLoader.h"
#include "engine/Internal.h"
#include "engine/Patternsleuth.h"
#include "engine/Shaders.h"
#include "engine/Duplicate.h"
#include "engine/WidgetInit.h"
#include "engine/GMalloc.h"
#include "engine/IoStoreOnDemand.h"
#include "engine/GUObjectArray.h"
#include "engine/FName.h"
#include "engine/Layout.h"
#include "engine/Reflection.h"
#include "engine/ProcessEvent.h"
#include "engine/ObjectLookup.h"
#include "engine/AssetRegistry.h"
#include "engine/Spawn.h"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif

#include <Zydis/Zydis.h>

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

// Forward declarations for module-internal helpers (see Internal.h for the
// cross-module byte scanners, .pdata helpers, and shared globals).
static bool discoverConvNameToString();
static bool findStaticConstructObject();
// SEH-isolated test-call helper for SCO candidate validation. Lives in
// its own function because C2712 forbids combining __try with C++ types
// that require unwinding (std::vector etc. in findStaticConstructObject).
static bool tryCallSCOCandidate(uint8_t* candidate, const void* params, void** out);
static void staticLoadObjectCrashed(bool wasFromCache);

// Discovered function pointers (s_staticLoadObject and related lookup ptrs are
// defined in engine/ObjectLookup.cpp; declared via engine/ObjectLookup.h.
// s_staticDuplicateObject{,Ex} live in engine/Duplicate.cpp - see Duplicate.h.)
static void* s_staticConstructObject = nullptr;  // StaticConstructObject_Internal - UE's NewObject primitive

// Discovered engine objects
extern void* s_world;               // UWorld* (defined in engine/Spawn.cpp)
extern void* s_gameplayStaticsCDO;   // Default__GameplayStatics (defined in engine/Spawn.cpp)
extern void* s_spawnFunc;            // BeginDeferredActorSpawnFromClass UFunction* (defined in engine/Spawn.cpp)
extern void* s_finishSpawnFunc;      // FinishSpawningActor UFunction* (defined in engine/Spawn.cpp)
static void* s_getPlayerCharacterFunc = nullptr; // GameplayStatics.GetPlayerCharacter UFunction*
static void* s_getPlayerPawnFunc = nullptr;      // GameplayStatics.GetPlayerPawn UFunction*
static void* s_getAllActorsOfClassFunc = nullptr; // GameplayStatics.GetAllActorsOfClass UFunction*


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
    ptrdiff_t staticConstructObject; // StaticConstructObject_Internal offset
    ptrdiff_t staticDuplicateObjectEx; // StaticDuplicateObjectEx (params struct entry) offset
    ptrdiff_t staticDuplicateObject;   // StaticDuplicateObject 7-arg wrapper offset
    ptrdiff_t ioStoreOnDemandMount;  // UE::IoStore::OnDemand::Mount offset (0 = not present on this host)
    ptrdiff_t ioStoreOnDemandSingletonGlobal;  // Address of the global holding the IOnDemandIoStore* singleton (re-deref at call time)
    // Layer 2/4 discovered struct field offsets (-1 = not yet discovered)
    int32_t childPropsOffset;
    int32_t fieldNextOffset;
    int32_t fieldNameOffset;
    int32_t fpropOffsetInternal;
    int32_t fpropElementSize;
    int32_t fpropFlags;
    int32_t fpropArrayInner;     // Stage G - FArrayProperty::Inner
    int32_t fpropStructStruct;   // Stage H - FStructProperty::Struct
    int32_t fpropObjectClass;    // Derived - FObjectProperty::PropertyClass
    int32_t fpropEnumEnum;       // Derived - FEnumProperty::Enum
    int32_t fpropByteEnum;       // Derived - FByteProperty::Enum
    ptrdiff_t arSerializeFnOff;  // UAssetRegistryImpl::Serialize function offset from base (0 = not yet probed)
    ptrdiff_t duplicateAndInitFromWidgetTreeOff; // UUserWidget::DuplicateAndInitializeFromWidgetTree offset (0 = not yet probed)
};

// Bumped to 0x48594463 - replaces arSerializeSlot (vtable index) with
// arSerializeFnOff (direct function pointer offset from module base, since
// absolute addresses don't survive ASLR across launches). The new resolver
// finds the function directly via E8-caller-climb from two inner-callee
// __FUNCTION__ anchors (FAssetRegistryImpl::CachePathsFromState +
// FAssetRegistryState::Save), bypassing the vtable entirely.
// Caches written under earlier magics get invalidated.
// Bumped to 0x48594464 - added staticDuplicateObjectEx + staticDuplicateObject
// offsets so the WBP runtime-instantiation bridge (UE 5.6 cooked WBP fix -
// see project_wbp_init_failure memory) survives across launches without
// re-scanning. Caches written under earlier magics get invalidated.
// Bumped to 0x48594465 - added duplicateAndInitFromWidgetTreeOff so the
// WBP InitializeWidget bypass survives across launches without re-scanning.
// Bumped to 0x48594466 - D&IFWT resolver fixed (picks LAST E8 in window,
// not biggest target). Old cache stored WBPGC::StaticClass by accident.
// Bumped to 0x48594467 - D&IFWT picks E8 whose target prologue does
// gs:[0x58] TLS read (D&IFWT's TScopeCounter). "Last E8" was wrong too
// because TMap teardown destructors run after D&IFWT in the window.
static const uint32_t CACHE_MAGIC = 0x48594467;

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
                     : "wrong function from fresh scan; SLO will be unavailable this run");
    s_staticLoadObject = nullptr;
    // Do NOT clear s_staticFindObject here - SFO was confirmed independently
    // (its own test-call against a runtime address succeeded earlier in
    // findStaticLoadObject) and is used by everything downstream of SLO,
    // including PE discovery, FNamePool probes, and Hydro.World. Wiping it
    // because a different function (SLO) failed nukes capabilities we
    // already proved work.
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
        s_staticConstructObject = nullptr;
        s_staticDuplicateObjectEx = nullptr;
        s_staticDuplicateObject   = nullptr;
        s_ioStoreOnDemandMount = nullptr;
        s_ioStoreOnDemandSingletonGlobal = nullptr;
        s_arSerializeFn = nullptr;  // code pointer - moves with ASLR, must clear on cache invalidation
        s_duplicateAndInitFromWidgetTree = nullptr;
        s_poolReady            = false;  // ← critical: gates discoverFNamePool
        // s_layout.* is intentionally kept - those are struct offsets
        // (e.g. 0x4C for FPROP_OFFSET_INTERNAL), not pointers. They don't
        // shift with ASLR and stay valid across launches.
        Hydro::logInfo("EngineAPI: cleared all cached pointers + s_poolReady - full re-discovery next tick");
    }
    std::error_code ec;
    std::filesystem::remove(getCachePath(), ec);
    // SLO is one capability among many. A fresh-scan crash means we picked
    // the wrong function for SLO specifically - but FNamePool, GUObjectArray,
    // ProcessEvent, etc. were each found via independent probes with their
    // own anchors and are not implicated. Marking the whole init fatal here
    // (the prior behaviour) cascades into "Hydro.HUD does nothing because
    // SLO probe was wrong," which is wildly disproportionate. Let init keep
    // going; downstream code paths that need SLO will fail individually.
    if (wasFromCache) {
        // Fall through - cache invalidation above already re-armed everything.
    }
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
    s_staticConstructObject = cache.staticConstructObject ? (gm.base + cache.staticConstructObject) : nullptr;
    s_staticDuplicateObjectEx = cache.staticDuplicateObjectEx ? (gm.base + cache.staticDuplicateObjectEx) : nullptr;
    s_staticDuplicateObject   = cache.staticDuplicateObject   ? (gm.base + cache.staticDuplicateObject)   : nullptr;
    s_ioStoreOnDemandMount = cache.ioStoreOnDemandMount ? (gm.base + cache.ioStoreOnDemandMount) : nullptr;
    s_ioStoreOnDemandSingletonGlobal = cache.ioStoreOnDemandSingletonGlobal ? (gm.base + cache.ioStoreOnDemandSingletonGlobal) : nullptr;
    s_arSerializeFn = cache.arSerializeFnOff ? (void*)(gm.base + cache.arSerializeFnOff) : nullptr;
    s_duplicateAndInitFromWidgetTree = cache.duplicateAndInitFromWidgetTreeOff
        ? (void*)(gm.base + cache.duplicateAndInitFromWidgetTreeOff) : nullptr;
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
        // Optional Stage G/H - restore if present, leave -1 otherwise.
        s_layout.arrayInner       = cache.fpropArrayInner;
        s_layout.structStruct     = cache.fpropStructStruct;
        // Rich inner-type offsets - derived from structStruct on first
        // discovery, persisted independently so we can spot a mismatch.
        s_layout.objectPropClass  = cache.fpropObjectClass;
        s_layout.enumPropEnum     = cache.fpropEnumEnum;
        s_layout.bytePropEnum     = cache.fpropByteEnum;
        s_layout.initialized      = true;
        s_layout.succeeded        = true;
        Hydro::logInfo("EngineAPI: struct layout restored from cache "
                       "(CHILD_PROPS=0x%X FF_NEXT=0x%X FF_NAME=0x%X "
                       "OFFSET_INTERNAL=0x%X ELEMENT_SIZE=0x%X FLAGS=0x%X "
                       "ARRAY_INNER=0x%X STRUCT_STRUCT=0x%X "
                       "OBJ_CLASS=0x%X ENUM_ENUM=0x%X BYTE_ENUM=0x%X)",
                       s_layout.childPropsOffset, s_layout.fieldNextOffset, s_layout.fieldNameOffset,
                       s_layout.offsetInternal, s_layout.elementSize, s_layout.flags,
                       s_layout.arrayInner, s_layout.structStruct,
                       s_layout.objectPropClass, s_layout.enumPropEnum, s_layout.bytePropEnum);
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

void saveScanCache(const GameModule& gm) {
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
    cache.staticConstructObject = s_staticConstructObject ? ((uint8_t*)s_staticConstructObject - gm.base) : 0;
    cache.staticDuplicateObjectEx = s_staticDuplicateObjectEx ? ((uint8_t*)s_staticDuplicateObjectEx - gm.base) : 0;
    cache.staticDuplicateObject   = s_staticDuplicateObject   ? ((uint8_t*)s_staticDuplicateObject   - gm.base) : 0;
    cache.ioStoreOnDemandMount = s_ioStoreOnDemandMount ? ((uint8_t*)s_ioStoreOnDemandMount - gm.base) : 0;
    cache.ioStoreOnDemandSingletonGlobal = s_ioStoreOnDemandSingletonGlobal ? (s_ioStoreOnDemandSingletonGlobal - gm.base) : 0;
    cache.childPropsOffset     = (s_layout.succeeded ? s_layout.childPropsOffset : -1);
    cache.fieldNextOffset      = (s_layout.succeeded ? s_layout.fieldNextOffset  : -1);
    cache.fieldNameOffset      = (s_layout.succeeded ? s_layout.fieldNameOffset  : -1);
    cache.fpropOffsetInternal  = (s_layout.succeeded ? s_layout.offsetInternal   : -1);
    cache.fpropElementSize     = (s_layout.succeeded ? s_layout.elementSize      : -1);
    cache.fpropFlags           = (s_layout.succeeded ? s_layout.flags            : -1);
    // Stage G/H optional - persist whatever was discovered. -1 simply means
    // "didn't validate this run, will retry next launch."
    cache.fpropArrayInner      = s_layout.arrayInner;
    cache.fpropStructStruct    = s_layout.structStruct;
    cache.fpropObjectClass     = s_layout.objectPropClass;
    cache.fpropEnumEnum        = s_layout.enumPropEnum;
    cache.fpropByteEnum        = s_layout.bytePropEnum;
    cache.arSerializeFnOff     = s_arSerializeFn ? ((uint8_t*)s_arSerializeFn - gm.base) : 0;
    cache.duplicateAndInitFromWidgetTreeOff = s_duplicateAndInitFromWidgetTree
        ? ((uint8_t*)s_duplicateAndInitFromWidgetTree - gm.base) : 0;

    auto path = getCachePath();
    FILE* f = fopen(path.c_str(), "wb");
    if (f) {
        fwrite(&cache, sizeof(cache), 1, f);
        fclose(f);
        Hydro::logInfo("EngineAPI: Scan cache saved to %s", path.c_str());
    }
}

// StaticLoadObjectFn_t is also defined in ObjectLookup.cpp; redeclared here
// because discoverEngineObjects and refreshWorld still use it in this TU.
using StaticLoadObjectFn_t = void*(__fastcall*)(
    void* Class, void* InOuter, const wchar_t* Name, const wchar_t* Filename,
    uint32_t LoadFlags, void* Sandbox, bool bAllowReconciliation, void* InstancingContext);

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
        // over whatever findStaticLoadObject() picked above. .pdata-based
        // containing-function lookup; the prior CC-padding walkback here
        // landed on the wrong function on UE 5.6 (fake-prologue match) and
        // crashed the test call below.
        uint8_t* loadObjStr = findWideString(s_gm.base, s_gm.size, L"StaticLoadObject");
        if (loadObjStr) {
            Hydro::logInfo("EngineAPI: Found 'StaticLoadObject' string at exe+0x%zX", (size_t)(loadObjStr - s_gm.base));
            uint8_t* leaRef = findLeaRef(s_gm.base, s_gm.size, loadObjStr);
            if (leaRef) {
                uint8_t* funcStart = funcStartViaPdata(leaRef);
                if (funcStart) {
                    Hydro::logInfo("EngineAPI: ALTERNATE StaticLoadObject at exe+0x%zX (.pdata fn start)",
                        (size_t)(funcStart - s_gm.base));
                    s_staticLoadObject = funcStart;
                } else {
                    Hydro::logWarn("EngineAPI: ALTERNATE LEA at exe+0x%zX has no .pdata containing function",
                        (size_t)(leaRef - s_gm.base));
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

    // Step 2: sanity-check the discovered StaticLoadObject with a known path.
    //
    // s_sloTestedAndBroken sticks across initialize() calls so that once we
    // SEH-catch a crash here, the next tick's retry skips the test instead
    // of re-crashing in a tight loop. UE 5.6 cooks where the SLO string
    // anchor lands on a wrong function go down this path.
    static bool s_sloTestedAndBroken = false;
    void* anyObj = nullptr;
    if (s_sloTestedAndBroken) {
        Hydro::logInfo("EngineAPI: SLO previously tested-and-broken, skipping test call");
        s_staticLoadObject = nullptr;
    } else {
        Hydro::logInfo("EngineAPI: Testing StaticLoadObject call...");
        auto fn = (StaticLoadObjectFn_t)s_staticLoadObject;
#ifdef _WIN32
        __try {
            anyObj = fn(nullptr, nullptr, L"/Script/CoreUObject.Object", nullptr, 0, nullptr, true, nullptr);
        } __except(1) {
            staticLoadObjectCrashed(fromCache);
            s_sloTestedAndBroken = true;
            anyObj = nullptr;
            // Fall through - SLO is unavailable but the rest of init can
            // still run. PE will be found via findProcessEvent() (vtable
            // lookup against any GUObjectArray entry) downstream.
        }
#endif
    }
    {
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
        // PE check moved to after GUObjectArray discovery - when SLO is
        // unavailable (UE 5.6 path), the SLO test can't extract PE, so we
        // need GUObjectArray's vtable-based fallback (findProcessEvent) first.
    }

    if (!fromCache) {
    // Step 3: Find GUObjectArray (needed for finding UWorld instance)
    if (!findGUObjectArray()) {
        Hydro::logWarn("EngineAPI: GUObjectArray not found - will try alternate world discovery");
    }
    } // end !fromCache block opened earlier

    // PE fallback (always-on, not gated by fromCache): if the SLO test
    // couldn't extract PE (UE 5.6 path where SLO is broken-and-skipped),
    // try the vtable-based extraction. Runs equally on cache-restore and
    // fresh-scan paths because GUObjectArray is available either way once
    // the !fromCache block above completes (or was restored from cache).
    if (!s_processEvent && s_guObjectArray) {
        Hydro::logInfo("EngineAPI: Trying PE fallback via GUObjectArray vtable");
        findProcessEvent();
    }
    if (!s_processEvent) {
        Hydro::logError("EngineAPI: ProcessEvent not found via SLO test or GUObjectArray fallback");
        return false;
    }

    if (!fromCache) {  // re-open !fromCache block for the rest of fresh-scan-only steps

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
    //
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

                    Hydro::logInfo("EngineAPI: %d callers of StaticFindObjectFast → %d unique size-range candidates",
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
    // Both discovery paths call findObject → getObjectName → FNamePool reads;
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
    // shader archives stay invisible to the renderer → materials fall back
    // to defaults.
    if (!s_openShaderLibrary) {
        if (findOpenShaderLibrary()) {
            saveScanCache(s_gm);
        } else {
            Hydro::logWarn("EngineAPI: OpenShaderLibrary not found - mod materials may render as defaults");
        }
    }

    // StaticConstructObject_Internal - UE's NewObject primitive. UE4SS
    // patternsleuth fails to find this on UE 5.6 (the proxy-class anchors
    // it relies on aren't LEA-referenced from .text on those builds).
    // We resolve it ourselves via a string-anchored statistical-
    // convergence verifier - see project_sco_resolver_design memory.
    // Powers Hydro.UI runtime widget construction and any other
    // NewObject-shaped feature.
    if (!s_staticConstructObject) {
        if (findStaticConstructObject()) {
            saveScanCache(s_gm);
        } else {
            Hydro::logWarn("EngineAPI: StaticConstructObject not found - runtime UObject construction unavailable");
        }
    }

    // StaticDuplicateObject - UE's archive-based deep-copy primitive. The
    // pair (SDOEx + the public 7-arg wrapper SDO) is what we need for
    // cooked-WBP runtime instantiation: the standard Create() path on
    // UE 5.6 uses NewObject-with-template + an InstancingGraph filter that
    // skips `UWidgetTree::RootWidget` (no CPF_InstancedReference), leaving
    // the instance tree empty. StaticDuplicateObject bypasses that by
    // serializing source → memory archive → new object, which copies every
    // UPROPERTY regardless of Instanced/Transient flags. Same primitive
    // unlocks AnimBP templates, complex materials with subobjects, etc.
    // See project_wbp_init_failure memory for the diagnostic chain.
    if (!s_staticDuplicateObjectEx) {
        if (findStaticDuplicateObject()) {
            saveScanCache(s_gm);
        } else {
            Hydro::logWarn("EngineAPI: StaticDuplicateObject not found - WBP runtime instantiation unavailable");
        }
    }

    // UUserWidget::DuplicateAndInitializeFromWidgetTree - the direct deep-
    // copy primitive used by the cooked-WBP runtime path. On UE 5.6 the
    // `InitializeWidgetStatic` gate skips this entirely when
    // `widget->WidgetTree` is non-null on entry (which it is, in practice).
    // We resolve it via structural anchor on the gate and call it
    // directly from Lua to route around the broken init decision chain.
    // See project_wbp_init_failure memory.
    if (!s_duplicateAndInitFromWidgetTree) {
        if (findDuplicateAndInitFromWidgetTree()) {
            saveScanCache(s_gm);
        } else {
            Hydro::logWarn("EngineAPI: D&IFWT not found - WBP bypass unavailable");
        }
    }

    // UE::IoStore::OnDemand::Mount - runtime IoStore container injection
    // on UE 5.4+ IoStore-shipped hosts. Soft-fail expected on legacy
    // hosts (anchor string isn't present) - caller falls back to
    // FPakPlatformFile::Mount via PakLoader for those.
    if (!s_ioStoreOnDemandMount) {
        if (findIoStoreOnDemandMount()) {
            saveScanCache(s_gm);
        }
    }
    // IOnDemandIoStore vtable - needed to find the singleton instance at
    // mount-call time (Mount is dispatched only via vtable, no direct
    // callers). The cached vtable address is the lookup key for runtime
    // instance discovery in getIoStoreOnDemandSingleton.
    if (s_ioStoreOnDemandMount && !s_ioStoreOnDemandSingletonGlobal) {
        if (findIoStoreOnDemandVtable()) {
            saveScanCache(s_gm);
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

    // Seed and exercise the raw-function registry. Lives in a helper because
    // building Descriptor literals (with std::wstring members) inside this
    // __try-bearing function would trigger MSVC C2712.
    if (s_ready) seedAndResolveRawFunctions();

    // -- FNamePool sanity diagnostic --
    //
    // Hypothesis (confirmed by parallel research 2026-04-28): on forked
    // engines like Palworld, Strategy-2 data-section scan can latch onto a
    // structurally-valid-but-functionally-wrong FNamePool - a debug stub or
    // freshly-initialized object whose Block[0][0] happens to look like a
    // "None" entry but whose Blocks[1..N] are null/garbage. Symptom: low
    // FName indices (in Block[0]) decode correctly; higher indices return
    // empty strings, so most findObject lookups silently miss.
    //
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


// --- StaticConstructObject_Internal (NewObject primitive) -------------
//
// Public wrapper over s_staticConstructObject (resolved during bootstrap
// - see findStaticConstructObject + project_sco_resolver_design memory).
// Used by Hydro.UI for runtime widget construction; any NewObject-shaped
// caller can route through here.
//
// Class+Outer fill the first 16 bytes of FStaticConstructObjectParameters.
// The rest of the struct is zero-padded; defaults that map to "anonymous
// name + RF_NoFlags + no template" are exactly what we want for an opaque
// runtime instantiation. UE 5.5 layout has Name/SetFlags/.../Template/...
// after the leading two pointers; UE 5.6 reorders some bool fields and
// inserts SerialNumber. A 256-byte zero buffer survives both - the
// SCO test-call validator in findStaticConstructObject empirically
// confirmed this on UE 5.6 cooks.
void* staticConstructObject(void* uclass, void* outer) {
    return staticConstructObjectWithTemplate(uclass, outer, nullptr);
}

// Extended SCO with Template pointer support. Template field sits at
// offset 0x28 in FStaticConstructObjectParameters across UE 5.x (after
// Class[0]/Outer[8]/Name[0x10]/Flags[0x18..0x20]/two bools[0x20..0x22]/
// padding to 0x28). When non-null, UE deep-copies properties from the
// template into the new object - same primitive UE uses for default
// subobject construction.
//
// Use case: bypass UE 5.6's broken `InitializeWidgetStatic` path by
// constructing fresh widget instances (CanvasPanel, TextBlock, etc.) at
// runtime using the class-level archetype CanvasPanels as templates.
// See project_wbp_init_failure for the diagnostic chain.
void* staticConstructObjectWithTemplate(void* uclass, void* outer, void* tmpl) {
    if (!s_staticConstructObject) {
        Hydro::logWarn("EngineAPI: staticConstructObject called but SCO not resolved");
        return nullptr;
    }
    if (!uclass) {
        Hydro::logWarn("EngineAPI: staticConstructObject called with null uclass");
        return nullptr;
    }

    alignas(16) uint8_t paramBuf[256] = {};
    *(void**)(paramBuf + 0x00) = uclass;
    *(void**)(paramBuf + 0x08) = outer;
    // Name (+0x10) = NAME_None - zero.
    // SetFlags / InternalSetFlags (+0x18..+0x20) = 0 - defaults.
    // bCopyTransientsFromClassDefaults (+0x20) = 0.
    // bAssumeTemplateIsArchetype (+0x21) = 0 - Template is treated as a
    // regular instance to duplicate from (not a CDO archetype).
    *(void**)(paramBuf + 0x28) = tmpl;  // Template
    // InstanceGraph (+0x30) = null - engine builds its own.
    // ExternalPackage (+0x38) = null.

    using SCOFn = void* (*)(const void*);
    auto fn = (SCOFn)s_staticConstructObject;
    void* result = nullptr;
    __try {
        result = fn(paramBuf);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Hydro::logError("EngineAPI: staticConstructObjectWithTemplate faulted "
            "(uclass=%p outer=%p tmpl=%p)", uclass, outer, tmpl);
        return nullptr;
    }
    return result;
}

// `staticDuplicateObject` lives in engine/Duplicate.cpp (see Duplicate.h).

// --- UE-indexed player / actor lookups ---------------------------------
//
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
    //
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

bool callFunction(void* obj, void* func, void* params) {
    return callProcessEvent(obj, func, params);
}

// Lazy-init s_gm so callers running before EngineAPI::initialize() (e.g.
// PakLoader::initialize, which fires earlier in the bootstrap) still get
// a valid module range from RawFunctions::resolve.
static void ensureGameModuleResolved() {
    if (!s_gm.base) s_gm = findGameModule();
}
uint8_t* getGameModuleBase() { ensureGameModuleResolved(); return s_gm.base; }
size_t   getGameModuleSize() { ensureGameModuleResolved(); return s_gm.size; }



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

void* getObjectPropertyClass(void* objectProp) {
    if (!objectProp) return nullptr;
    if (!s_layout.initialized) discoverPropertyLayout();
    if (s_layout.objectPropClass < 0) return nullptr;
    void* cls = nullptr;
    safeReadPtr((uint8_t*)objectProp + s_layout.objectPropClass, &cls);
    return cls;
}

void* getEnumPropertyEnum(void* enumProp) {
    if (!enumProp) return nullptr;
    if (!s_layout.initialized) discoverPropertyLayout();
    if (s_layout.enumPropEnum < 0) return nullptr;
    void* uenum = nullptr;
    safeReadPtr((uint8_t*)enumProp + s_layout.enumPropEnum, &uenum);
    return uenum;
}

void* getBytePropertyEnum(void* byteProp) {
    if (!byteProp) return nullptr;
    if (!s_layout.initialized) discoverPropertyLayout();
    if (s_layout.bytePropEnum < 0) return nullptr;
    void* uenum = nullptr;
    safeReadPtr((uint8_t*)byteProp + s_layout.bytePropEnum, &uenum);
    return uenum;
}

// Conv_NameToString fallback -
//
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

// -- Layer 4: per-class named property offset cache ----------------------
//
// findReflectedFieldOffset(uclassPtr, "RootComponent") returns the byte
// offset of that field within instances of `uclassPtr` - discovered once
// via the property chain (which uses Layer 2's offsets), then cached.
//
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
    //
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



// -- StaticConstructObject_Internal resolver ----------------------------
//
// UE4SS's patternsleuth fails to find SCO on stock UE 5.6 cooks (the
// proxy-class anchors `UPlayMontageCallbackProxy` etc. aren't LEA-
// referenced from `.text` on those builds, so the standard "find anchor
// + intersect two consecutive near-calls" strategy returns nothing).
//
// We anchor on `"NewObject with empty name can't be used to create
// default"` instead - UE's fatal-log message inside SCO and inside every
// NewObject<T> template's inlined empty-name check. PGO/inlining means
// the string is referenced from many call sites (~700 on DMG@5.6), and
// every one of those callers immediately near-calls into SCO. Tallying
// near-call targets across all xref sites makes SCO the runaway winner
// regardless of which specific PGO/inline shape any single site has -
// statistical convergence does the verification.
//
// See `memory/project_sco_resolver_design.md` for the full rationale.
static bool findStaticConstructObject() {
    if (!s_gm.base) return false;

    // Env-var bypass - set HYDRO_SKIP_SCO=1 to skip SCO probe entirely.
    // Use when SCO test-calls crash the process on a host where SEH can't
    // catch the fault (e.g., function does ExitProcess on bad input). SCO
    // is soft-fail; loadAsset / Assets.load tests don't need it.
    char skipBuf[8] = {};
    DWORD skipLen = GetEnvironmentVariableA("HYDRO_SKIP_SCO", skipBuf, sizeof(skipBuf));
    if (skipLen > 0 && skipLen < sizeof(skipBuf) && skipBuf[0] == '1') {
        Hydro::logWarn("EngineAPI: SCO probe skipped (HYDRO_SKIP_SCO=1)");
        return false;
    }

    // Tier 1: locate the anchor string in `.rdata`. UE_LOG TEXT(...)
    // macros emit UTF-16LE wide-string literals. Truncated tail because
    // the full message is too long to need the whole thing; the prefix
    // is unique enough.
    const wchar_t* needle = L"NewObject with empty name can't be used to create default";
    uint8_t* strAddr = findWideString(s_gm.base, s_gm.size, needle);
    if (!strAddr) {
        Hydro::logWarn("EngineAPI: SCO anchor string not found");
        return false;
    }
    Hydro::logInfo("EngineAPI: SCO anchor at exe+0x%zX", (size_t)(strAddr - s_gm.base));

    // Find every LEA xref to it. Expect dozens to hundreds depending on
    // how aggressively the compiler inlined NewObject<T>.
    std::vector<uint8_t*> leas = findAllLeaRefs(s_gm.base, s_gm.size, strAddr);
    if (leas.empty()) {
        Hydro::logWarn("EngineAPI: no LEA xrefs to SCO anchor (compiler may have used a different reference form)");
        return false;
    }
    Hydro::logInfo("EngineAPI: SCO anchor has %zu LEA xrefs", leas.size());

    // Edge case: if only one site references the string, statistical
    // convergence has no signal. Fall back to "the LEA's containing
    // function IS SCO" - this is the canonical case where the empty-
    // name check wasn't inlined into callers, and the only ref is from
    // inside SCO itself.
    if (leas.size() == 1) {
        uint8_t* funcStart = funcStartViaPdata(leas[0]);
        if (funcStart) {
            s_staticConstructObject = funcStart;
            Hydro::logInfo("EngineAPI: StaticConstructObject (single-LEA fallback) at exe+0x%zX",
                (size_t)(funcStart - s_gm.base));
            return true;
        }
        Hydro::logWarn("EngineAPI: SCO single-LEA fallback failed: no .pdata function for LEA");
        return false;
    }

    // Statistical convergence - but we have to scan the WHOLE containing
    // function, not just forward from the LEA. The LEA loads the error
    // string for the FATAL log path that only executes when name==None.
    // Walking forward from the LEA hits UE_LOG and abort helpers, not
    // SCO (which lives on the success branch, before the fatal label).
    //
    // Strategy: dedupe LEAs to their containing functions via .pdata,
    // then scan the FULL function body for near-call targets. Across
    // ~700 unique NewObject<T> caller functions, SCO is the call every
    // single one of them makes - runaway statistical winner.
    auto* dosHeader = (IMAGE_DOS_HEADER*)s_gm.base;
    auto* ntHeaders = (IMAGE_NT_HEADERS*)(s_gm.base + dosHeader->e_lfanew);
    auto& excDir = ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    auto* funcs = (RUNTIME_FUNCTION*)(s_gm.base + excDir.VirtualAddress);
    size_t numFuncs = excDir.Size / sizeof(RUNTIME_FUNCTION);

    auto findFunc = [&](uint8_t* addr) -> RUNTIME_FUNCTION* {
        uint32_t rva = (uint32_t)(addr - s_gm.base);
        size_t lo = 0, hi = numFuncs;
        while (lo < hi) {
            size_t mid = (lo + hi) / 2;
            if (rva < funcs[mid].BeginAddress)        hi = mid;
            else if (rva >= funcs[mid].EndAddress)    lo = mid + 1;
            else return &funcs[mid];
        }
        return nullptr;
    };

    std::unordered_map<uint32_t, RUNTIME_FUNCTION*> uniqueFuncs;  // BeginRva → entry
    for (uint8_t* lea : leas) {
        if (auto* f = findFunc(lea)) {
            uniqueFuncs.emplace(f->BeginAddress, f);
        }
    }
    Hydro::logInfo("EngineAPI: SCO LEAs dedupe to %zu unique containing functions", uniqueFuncs.size());

    std::unordered_map<uint8_t*, int> tally;
    for (auto& [beginRva, fn] : uniqueFuncs) {
        size_t funcSize = fn->EndAddress - fn->BeginAddress;
        // Cap at 8KB - anything larger is an outlier (probably the
        // mega-function that contains SCO itself), and we want callers
        // not the giant containing-SCO function.
        if (funcSize > 8192) continue;
        uint8_t* funcStart = s_gm.base + fn->BeginAddress;
        std::vector<uint8_t*> targets = collectForwardCallTargets(funcStart, (int)funcSize);
        std::sort(targets.begin(), targets.end());
        targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
        for (uint8_t* t : targets) tally[t]++;
    }

    if (tally.empty()) {
        Hydro::logWarn("EngineAPI: SCO convergence: no near-call targets found in any xref window");
        return false;
    }

    // Sort candidates by hit-count, descending. The top candidates are
    // SCO and FStaticConstructObjectParameters::ctor - both called at
    // every site. We can't disambiguate via tally alone; test-call
    // both/all and pick the one that actually constructs a UObject.
    std::vector<std::pair<uint8_t*, int>> ranked(tally.begin(), tally.end());
    std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
        return a.second > b.second;
    });

    if (ranked.empty() || ranked[0].second < 5) {
        Hydro::logWarn("EngineAPI: SCO convergence too weak (top hits %d) - refusing",
            ranked.empty() ? 0 : ranked[0].second);
        return false;
    }

    Hydro::logInfo("EngineAPI: SCO top candidates: [0]=exe+0x%zX(%d) [1]=exe+0x%zX(%d) [2]=exe+0x%zX(%d)",
        (size_t)(ranked[0].first - s_gm.base), ranked[0].second,
        ranked.size() > 1 ? (size_t)(ranked[1].first - s_gm.base) : 0u,
        ranked.size() > 1 ? ranked[1].second : 0,
        ranked.size() > 2 ? (size_t)(ranked[2].first - s_gm.base) : 0u,
        ranked.size() > 2 ? ranked[2].second : 0);

    // Test-call validator: SCO returns a UObject*; the param-ctor returns
    // void. Call each candidate with a parameter-block that asks for a
    // UObject instance and check whether the return is a recognisable
    // UObject (class field reads back as the requested UClass).
    void* uobjectClass = findObject(L"/Script/CoreUObject.Object");
    if (!uobjectClass) {
        Hydro::logWarn("EngineAPI: SCO test-call: UObject UClass not found - falling back to top candidate without validation");
        s_staticConstructObject = ranked[0].first;
        Hydro::logInfo("EngineAPI: StaticConstructObject_Internal at exe+0x%zX (UNVALIDATED)",
            (size_t)(ranked[0].first - s_gm.base));
        return true;
    }

    // Buffer is generous - FStaticConstructObjectParameters in UE 5.5
    // is ~80 bytes; we zero a 256-byte block so any UE-5.x layout shift
    // still has zero-filled uninitialised fields. Class+Outer at the top
    // are stable across versions.
    alignas(16) uint8_t paramBuf[256] = {};
    *(void**)(paramBuf + 0) = uobjectClass;   // Class
    *(void**)(paramBuf + 8) = nullptr;        // Outer = nullptr → SCO uses transient package

    // Pre-screen: find the ExitProcess import thunk so we can skip any
    // candidate that calls it (ExitProcess bypasses SEH, killing the process).
    uint8_t* exitProcessThunk = nullptr;
    {
        auto* dosHdr = (IMAGE_DOS_HEADER*)s_gm.base;
        auto* ntHdr  = (IMAGE_NT_HEADERS*)(s_gm.base + dosHdr->e_lfanew);
        auto& impDir = ntHdr->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (impDir.VirtualAddress && impDir.Size) {
            auto* desc = (IMAGE_IMPORT_DESCRIPTOR*)(s_gm.base + impDir.VirtualAddress);
            for (; desc->Name; ++desc) {
                const char* modName = (const char*)(s_gm.base + desc->Name);
                if (_stricmp(modName, "KERNEL32.dll") != 0 &&
                    _stricmp(modName, "kernel32.dll") != 0) continue;
                auto* thunkRef = (IMAGE_THUNK_DATA*)(s_gm.base + desc->OriginalFirstThunk);
                auto* funcRef  = (IMAGE_THUNK_DATA*)(s_gm.base + desc->FirstThunk);
                for (; thunkRef->u1.AddressOfData; ++thunkRef, ++funcRef) {
                    if (IMAGE_SNAP_BY_ORDINAL(thunkRef->u1.Ordinal)) continue;
                    auto* name = (IMAGE_IMPORT_BY_NAME*)(s_gm.base + (uint32_t)thunkRef->u1.AddressOfData);
                    if (strcmp(name->Name, "ExitProcess") == 0) {
                        exitProcessThunk = (uint8_t*)(uintptr_t)funcRef->u1.Function;
                        break;
                    }
                }
                if (exitProcessThunk) break;
            }
        }
        if (exitProcessThunk)
            Hydro::logInfo("EngineAPI: SCO pre-screen: ExitProcess thunk at %p", exitProcessThunk);
    }

    auto candCallsExitProcess = [&](uint8_t* fn, uint32_t fnSize) -> bool {
        // Scan for: FF 25 [rel32] (JMP QWORD PTR [rip+disp]) landing on exitProcessThunk
        // or: E8 [rel32] CALL landing on an ExitProcess-calling stub
        // Quick heuristic: look for __fastfail pattern (CD 29) first.
        uint32_t limit = fnSize < 512 ? fnSize : 512;
        for (uint32_t k = 0; k + 1 < limit; k++) {
            if (fn[k] == 0xCD && fn[k+1] == 0x29) return true;  // int 0x29 (__fastfail)
        }
        if (!exitProcessThunk) return false;
        for (uint32_t k = 0; k + 5 < limit; k++) {
            if (fn[k] == 0xFF && fn[k+1] == 0x25) {             // JMP [rip+disp32]
                int32_t disp = 0; memcpy(&disp, fn + k + 2, 4);
                uint8_t* target = fn + k + 6 + disp;
                if (target == exitProcessThunk) return true;
            }
            if (fn[k] == 0xE8) {                                 // CALL rel32
                int32_t disp = 0; memcpy(&disp, fn + k + 1, 4);
                uint8_t* target = fn + k + 5 + disp;
                if (target == exitProcessThunk) return true;
            }
        }
        return false;
    };

    int bestIdx = -1;
    for (size_t i = 0; i < ranked.size() && i < 8; i++) {
        uint8_t* cand = ranked[i].first;

        // Size filter: SCO is hundreds of bytes; param-ctor is tiny. Skip tiny ones.
        uint32_t candSize = 0;
        if (auto* f = findFunc(cand)) candSize = f->EndAddress - f->BeginAddress;
        if (candSize < 256) {
            Hydro::logInfo("EngineAPI: SCO pre-screen: candidate [%zu] exe+0x%zX size=%u too small - skipping",
                i, (size_t)(cand - s_gm.base), candSize);
            continue;
        }

        // ExitProcess / __fastfail check - skip anything that can kill the process.
        if (candCallsExitProcess(cand, candSize)) {
            Hydro::logInfo("EngineAPI: SCO pre-screen: candidate [%zu] exe+0x%zX contains ExitProcess/__fastfail - skipping",
                i, (size_t)(cand - s_gm.base));
            continue;
        }

        void* result = nullptr;
        if (!tryCallSCOCandidate(cand, paramBuf, &result)) {
            Hydro::logInfo("EngineAPI: SCO test-call: candidate [%zu] exe+0x%zX FAULTED - not SCO",
                i, (size_t)(cand - s_gm.base));
            continue;
        }
        if (!result) {
            Hydro::logInfo("EngineAPI: SCO test-call: candidate [%zu] exe+0x%zX returned null - not SCO (likely param-ctor)",
                i, (size_t)(cand - s_gm.base));
            continue;
        }
        // Validate: result should be a UObject whose class matches what we
        // passed. ClassPrivate field is at +0x10 on every UE 5.x UObject.
        void* resultClass = nullptr;
        if (!safeReadPtr((uint8_t*)result + 0x10, &resultClass) || resultClass != uobjectClass) {
            Hydro::logInfo("EngineAPI: SCO test-call: candidate [%zu] exe+0x%zX returned %p but class mismatch - not SCO",
                i, (size_t)(cand - s_gm.base), result);
            continue;
        }
        Hydro::logInfo("EngineAPI: SCO test-call: candidate [%zu] exe+0x%zX returned valid UObject %p - confirmed SCO",
            i, (size_t)(cand - s_gm.base), result);
        bestIdx = (int)i;
        break;
    }

    if (bestIdx < 0) {
        Hydro::logWarn("EngineAPI: SCO test-call: no candidate produced a valid UObject - refusing");
        return false;
    }

    s_staticConstructObject = ranked[bestIdx].first;
    Hydro::logInfo("EngineAPI: StaticConstructObject_Internal at exe+0x%zX",
        (size_t)(ranked[bestIdx].first - s_gm.base));
    return true;
}

// SEH-isolated test-call. Returns true if the call completed without an
// access violation; the candidate's return value is written through `out`.
// Returns false (and leaves `*out` untouched) if the call faulted.
static bool tryCallSCOCandidate(uint8_t* candidate, const void* params, void** out) {
    using SCOFn = void* (*)(const void*);
    auto fn = (SCOFn)candidate;
    __try {
        *out = fn(params);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}


// -- UMG SetText via Conv_StringToText shim ------------------------------
//
// UMG widgets' SetText UFunctions take FText, not FString - and FText
// construction has no stable C-callable factory in shipping cooks
// (FText::FromString is templated/inlined). Route through the
// `UKismetTextLibrary::Conv_StringToText` BlueprintPure UFunction:
//
//   FString → ProcessEvent(Conv_StringToText) → FText
//   FText   → ProcessEvent(widget.SetText)
//
// FText is an opaque struct (TSharedPtr<ITextData> + uint32 flags) ~24
// bytes in shipping. We don't need to know its internal layout - just
// memcpy the return-value slot from the conv params block to the input
// slot of SetText. The size to copy is derived from the conv function's
// reflected param layout (parmsSize - retOffset).

bool setWidgetText(void* widget, const wchar_t* text) {
    if (!widget || !text) return false;
    if (!s_processEvent) return false;

    // Resolve Conv_StringToText once per process. The CDO is the receiver
    // for static UFunction calls - KismetTextLibrary's BP-callable surface
    // is all static, so we send ProcessEvent to Default__KismetTextLibrary.
    static void* s_kismetTextLibCDO = nullptr;
    static void* s_convStringToText = nullptr;
    static int    s_convParmsSize  = 0;
    static int    s_convRetOffset  = 0;
    static int    s_convRetSize    = 0;

    if (!s_convStringToText) {
        void* libCls = findObject(L"/Script/Engine.KismetTextLibrary");
        if (!libCls) {
            Hydro::logWarn("EngineAPI: setWidgetText: KismetTextLibrary class not found");
            return false;
        }
        s_kismetTextLibCDO = findObject(L"/Script/Engine.Default__KismetTextLibrary");
        if (!s_kismetTextLibCDO) {
            Hydro::logWarn("EngineAPI: setWidgetText: KismetTextLibrary CDO not found");
            return false;
        }
        s_convStringToText = findFunction(libCls, L"Conv_StringToText");
        if (!s_convStringToText) {
            Hydro::logWarn("EngineAPI: setWidgetText: Conv_StringToText UFunction not found");
            return false;
        }
        s_convParmsSize = (int)getUFunctionParmsSize(s_convStringToText);
        s_convRetOffset = (int)getUFunctionRetOffset(s_convStringToText);
        s_convRetSize   = s_convParmsSize - s_convRetOffset;
        Hydro::logInfo("EngineAPI: setWidgetText: Conv_StringToText resolved, parms=%d retOff=%d retSize=%d",
            s_convParmsSize, s_convRetOffset, s_convRetSize);
        if (s_convParmsSize <= 0 || s_convRetOffset <= 0 ||
            s_convRetSize  <= 0 || s_convParmsSize > 128) {
            Hydro::logWarn("EngineAPI: setWidgetText: implausible conv layout");
            s_convStringToText = nullptr;  // force re-resolve next call
            return false;
        }
    }

    // Resolve the widget's SetText UFunction. Cache per-class would be
    // marginal; the per-frame caller's hot-path cost is the ProcessEvent
    // call itself. Walk the chain so subclass SetText overrides are picked
    // up (e.g. URichTextBlock uses the same UFunction shape).
    void* widgetClass = getClass(widget);
    if (!widgetClass) return false;
    void* setTextFn = findFunction(widgetClass, L"SetText");
    if (!setTextFn) {
        Hydro::logWarn("EngineAPI: setWidgetText: widget class has no SetText UFunction");
        return false;
    }
    int setTextParmsSize = (int)getUFunctionParmsSize(setTextFn);
    if (setTextParmsSize <= 0 || setTextParmsSize > 128) {
        Hydro::logWarn("EngineAPI: setWidgetText: implausible SetText parm size %d", setTextParmsSize);
        return false;
    }

    // Step 1: Conv_StringToText - { FString InString @0, FText Ret @retOff }
    alignas(16) uint8_t convParams[128] = {};
    FStringMinimal* fstr = (FStringMinimal*)convParams;
    if (!buildFString(*fstr, text)) {
        Hydro::logWarn("EngineAPI: setWidgetText: FString GMalloc failed");
        return false;
    }

    if (!callProcessEvent(s_kismetTextLibCDO, s_convStringToText, convParams)) {
        Hydro::logWarn("EngineAPI: setWidgetText: Conv_StringToText ProcessEvent failed");
        return false;
    }

    // Step 2: SetText - { FText InText @0 } - copy the conv return slot
    // straight into SetText's input slot. UE will own the FText's
    // TSharedPtr ref count from here.
    alignas(16) uint8_t setTextParams[128] = {};
    if ((size_t)s_convRetSize > sizeof(setTextParams)) {
        Hydro::logWarn("EngineAPI: setWidgetText: FText slot larger than buffer");
        return false;
    }
    memcpy(setTextParams, convParams + s_convRetOffset, (size_t)s_convRetSize);

    return callProcessEvent(widget, setTextFn, setTextParams);
}



} // namespace Hydro::Engine
