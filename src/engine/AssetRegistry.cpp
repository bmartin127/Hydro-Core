#include "AssetRegistry.h"
#include "../EngineAPI.h"
#include "../HydroCore.h"
#include "../SEH.h"
#include "../FArchiveLoader.h"
#include "../PakLoader.h"
#include "Internal.h"
#include "FName.h"
#include "GMalloc.h"
#include "GUObjectArray.h"
#include "Layout.h"
#include "Reflection.h"
#include "ProcessEvent.h"
#include "ObjectLookup.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

#include <Zydis/Zydis.h>

namespace Hydro::Engine {

// Forward decl: defined in EngineAPI.cpp. The AR.bin merge path calls this
// whenever a resolver flips s_ar*/s_fbufferReader* globals so the next
// launch skips the multi-second pattern scan.
void saveScanCache(const GameModule& gm);

// AssetRegistry loading (same dispatch path BPModLoaderMod uses).
void* s_assetRegHelpersCDO = nullptr;   // Default__AssetRegistryHelpers
void* s_getAssetFunc = nullptr;         // AssetRegistryHelpers:GetAsset UFunction
void* s_assetRegImpl = nullptr;         // AssetRegistry implementation object
void* s_getByPathFunc = nullptr;        // AssetRegistry:GetAssetByObjectPath UFunction
void* s_arSerializeFn = nullptr;                  // UAssetRegistryImpl::Serialize(FArchive&) function pointer (entry of MSVC adjustor thunk on UE 5.6 - see s_arSerializeThisOffset)

// Runtime pak mount support. Discovered after FPackageStore live inspection
// completes (which finds FFilePackageStoreBackend via lea-scan + smart filter).
// FPakPlatformFile owns the backend as a member, so reverse-scanning heap
// memory for the backend pointer locates the FPakPlatformFile instance -
// bypassing PakLoader's L"PakFile" anchor (which can pick a wrong vtable on
// stripped UE 5.6 shipping builds, leading to instance-scan returning 0 candidates).
void* s_filePackageStoreBackend = nullptr;  // FFilePackageStoreBackend* - captured during FPackageStore inspection
void* s_fpakPlatformFile        = nullptr;  // FPakPlatformFile* - reverse-discovered via backend pointer
void* s_fpakPlatformFileVtable  = nullptr;  // = *(void**)s_fpakPlatformFile (its first qword)
uint16_t s_arSerializeThisOffset = 0;     // offset within s_assetRegImpl to pass as `this` (IAssetRegistry secondary-base offset; the thunk does `sub rcx, this_offset` to get back to UObject base)

// Bypass path: directly call FAssetRegistryImpl::Serialize(FArchive&, FEventContext&).
//
// UAssetRegistryImpl::Serialize(FArchive&) (slot 124) does:
//   if (Ar.IsObjectReferenceCollector()) return;
//   FEventContext EventContext;       // stack-allocated, zero-init
//   { FInterfaceWriteScopeLock lock(InterfaceLock);
//     GuardedData.Serialize(Ar, EventContext); }   // ← THE actual work
//   Broadcast(EventContext);
//
// We replicate this directly: skip the lock (we're the only thread), skip
// the broadcast (we don't need asset-event listeners). Just synthesize a
// zero-init FEventContext and invoke `GuardedData.Serialize(Ar, &ctx)`.
//
//   void FAssetRegistryImpl::Serialize(FArchive& Ar,
//                                      Impl::FEventContext& EventContext);
//
// Resolved by picking the LARGEST E8-caller of SerializeHeader that has a
// 3-arg signature (uses r8). On UE 5.6 shipping, FAssetRegistryState::Load
// and Save are PGO-inlined into this function, making it ~2.5 KB; among
// the 5 SerializeHeader-caller candidates, only one is that large.
//
// FAssetRegistryState::Load (the originally-targeted public API) doesn't
// exist as a separate callable function in shipping due to PGO/LTO inlining.
void* s_arImplSerializeFn = nullptr;      // FAssetRegistryImpl::Serialize(FArchive&, FEventContext&)
uint16_t s_arGuardedDataOffset = 0;       // offset within s_assetRegImpl to GuardedData (the FAssetRegistryImpl member)

bool discoverAssetRegistry() {
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

// -- AssetRegistry::ScanPathsSynchronous via reflection ------------------
//
// UE only auto-indexes paks discovered at engine startup. A pak mounted
// later is invisible to AssetRegistry until we explicitly request a scan.
// IAssetRegistry::ScanPathsSynchronous is reflected (UFUNCTION-decorated),
// so we can call it via ProcessEvent without depending on any pattern-
// scanned native function - the path used for everything Layer-3-and-up.
//
// Signature (UE 5.x):
//   virtual void ScanPathsSynchronous(const TArray<FString>& InPaths,
//                                     bool bForceRescan = false,
//                                     bool bIgnoreDenyListScanFilters = false) const;
//
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

// IAssetRegistry::SearchAllAssets(bool bSynchronousSearch) - virtual UFUNCTION
// (BlueprintCallable). On UE 5.6, AR has a `bSearchAllAssets` flag that gates
// the asset-gatherer path inside `OnContentPathMounted` (the broadcast that
// fires when a pak mounts). In shipping cooks the flag defaults to false,
// which is why our LogicMods/ paks mount but their .uasset files never get
// indexed even after `ScanPathsSynchronous("/Game/Mods")`. Calling
// `SearchAllAssets(true)` flips that flag AND forces a synchronous gather of
// every mounted content path, which IS supposed to pick up our mod content.
//
// Heavier than ScanPathsSynchronous (walks the whole game's .uasset tree, not
// just /Game/Mods), but on stock UE 5.6 it's the documented escape hatch when
// the gather-on-mount path is gated off. Caller should run this once at
// startup (post-pak-mount), not per loadAsset miss.
bool searchAllAssets(bool bSynchronousSearch) {
    if (!s_processEvent) return false;
    if (!s_assetRegImpl) {
        Hydro::logWarn("EngineAPI: searchAllAssets skipped - AssetRegistry impl not discovered");
        return false;
    }

    // Same dispatch shape as scanAssetRegistryPaths above: the UFunction lives
    // on the abstract interface UClass, not on the impl. Reach it via any
    // already-discovered IAssetRegistry UFunction's Outer.
    void* arInterfaceClass = nullptr;
    if (s_getByPathFunc) arInterfaceClass = getOuter(s_getByPathFunc);
    if (!arInterfaceClass) arInterfaceClass = findObject(L"/Script/AssetRegistry.AssetRegistry");
    void* arImplClass = getClass(s_assetRegImpl);

    void* fn = nullptr;
    if (arInterfaceClass) fn = findFunction(arInterfaceClass, L"SearchAllAssets");
    if (!fn && arImplClass) fn = findFunction(arImplClass, L"SearchAllAssets");
    if (!fn) fn = findObject(L"/Script/AssetRegistry.AssetRegistry:SearchAllAssets");
    if (!fn) {
        Hydro::logWarn("EngineAPI: searchAllAssets - UFunction not found");
        return false;
    }

    uint16_t parmsSize = getUFunctionParmsSize(fn);
    if (parmsSize == 0 || parmsSize > 32) {
        Hydro::logWarn("EngineAPI: searchAllAssets - bad ParmsSize=%u", parmsSize);
        return false;
    }

    alignas(16) uint8_t params[32] = {};
    *(bool*)(params + 0x00) = bSynchronousSearch;

    Hydro::logInfo("EngineAPI: SearchAllAssets(synchronous=%d) - ParmsSize=%u",
                   bSynchronousSearch ? 1 : 0, parmsSize);
    uint64_t t0 = GetTickCount64();
    bool ok = callFunction(s_assetRegImpl, fn, params);
    uint64_t dt = GetTickCount64() - t0;
    Hydro::logInfo("EngineAPI: SearchAllAssets returned %s (took %llu ms)",
                   ok ? "true" : "false", (unsigned long long)dt);
    return ok;
}

// -- AssetRegistry::ScanFilesSynchronous via reflection ------------------
//
// Sibling of scanAssetRegistryPaths but takes a filesystem .uasset path
// instead of a virtual /Game/ path. Reason: on Palworld, ScanPathsSynchronous
// drops our path during FPackageName::TryConvertLongPackageNameToFilename
// because the runtime-mounted pak's mount point was never registered with
// FPackageName (the broadcast that should have called RegisterMountPoint
// during pak mount didn't fire). ScanFilesSynchronous's filesystem-direction
// lookup may bypass that broken step entirely - UE 5.1 reads the .uasset's
// own header to extract the package name, so it doesn't need the mount tree.
//
// Signature (UE 5.1):
//   virtual void ScanFilesSynchronous(const TArray<FString>& Files,
//                                     bool bForceRescan = false) const;
//
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

// -- UAssetRegistryImpl::Serialize(FArchive&) bridge -------------------------
//
// On UE 5.6 the engine-startup auto-mount of LogicMods/pakchunk*.{utoc,ucas,pak}
// triples mounts the IoStore container but DOES NOT auto-merge the sidecar
// AssetRegistry.bin into the live AR (this side of the mount worked in 5.5
// but became opt-in / driven by FPackageStore in 5.6 - we cross-deployed a
// 5.5 pak into 5.6 and Assets.load still returned nil, so it's an engine
// regression, not a cook-side issue).
//
// The fix: deserialize the sidecar AR.bin directly into the live AR via
// `UAssetRegistryImpl::Serialize(FArchive&)` - the same entry point ModSkeleton
// (UE 4.15+) and any in-tree runtime-AR-merge feature uses.
//
// Resolver design - single-anchor + size-heuristic + verify-call:
//
//   UAssetRegistryImpl::Serialize           ← what we want (virtual override)
//     → FAssetRegistryImpl::Serialize        ← inner non-virtual; on shipping
//                                              cooks this is typically inlined
//                                              into the outer override
//          → if loading: State.Load() + CachePathsFromState() + ...
//          → if saving:  State.Save()         ← shipping cooks dead-code
//                                              eliminate this entire branch
//                                              (saving only happens at cook
//                                              time; Ar.IsSaving() proves
//                                              statically false at runtime).
//
// Earlier resolver iterations tried two disambiguators that both failed on
// UE 5.6 shipping cooks:
//   (a) Intersect callers of CachePathsFromState with callers of
//       FAssetRegistryState::Save. LTO drops the save branch entirely on
//       shipping, so the intersection is empty.
//   (b) Filter cache_callers by membership in s_assetRegImpl's vtable.
//       UAssetRegistryImpl is multiple-inheritance (UObject + IAssetRegistry).
//       The object stores two vtables: a UObject primary at the head and an
//       IAssetRegistry sub-vtable at some offset. UE 5.6 keeps Serialize as
//       the IAssetRegistry::Serialize override only, so the UObject vtable
//       we read contains UObject::Serialize, not UAssetRegistryImpl::Serialize.
//       Membership comes up empty even when the right function is right
//       there in cache_callers.
//
// What ships: cache_callers reliably contains the AR.Serialize wrapper plus
// at most a handful of unrelated cachers (LoadPremadeAssetRegistry-shaped
// helpers). Their .pdata sizes differ by an order of magnitude. The verify
// call (SEH-wrapped Serialize call against FArchiveLoader(nullptr, 0)
// checking that the shim's TotalSize/Serialize counters tick) is itself a
// disambiguator - we never reached it before because the vtable filter ate
// the candidates first.
//
// Algorithm:
//   1. Single anchor: "FAssetRegistryImpl::CachePathsFromState".
//      anchor → LEA xrefs → funcStartViaPdata → cache_fn_set.
//   2. cache_callers = ⋃ findE8CallersOf(fn) for fn in cache_fn_set.
//   3. Filter cache_callers: keep entries with .pdata size in (0, 500].
//      Wrappers are tiny (Serialize was 139 bytes on DMG@5.6); functions
//      with state machines (LoadPremadeAssetRegistry-shaped) are >500 bytes.
//      Entries with .pdata size 0 are thunks and can't be the wrapper.
//   4. Sort survivors by .pdata size ascending.
//   5. For each candidate in order, run the SEH-wrapped verify call with
//      FArchiveLoader(nullptr, 0). On the first candidate where the shim's
//      Serialize or TotalSize counter ticks: that's the wrapper. Cache and
//      return.
//   6. If no candidate verifies: log + bail. No fallback sweeps.
//
// SEH constraint: __try cannot share a function with C++ unwinding objects
// (MSVC C2712), so the call itself lives in a tiny helper that takes raw
// pointers. The caller builds FArchiveLoader instances and reads counters
// from outside the SEH region.

using ArSerializeFn = void(__fastcall*)(void* this_, void* archive);

// Diagnostic: last SEH-caught fault info from sehCallArSerialize. Reset
// each call. Filled by the EXCEPTION filter so the caller can log the
// fault site even on a quiet-probe path.
static DWORD64 s_lastArSerializeFaultAddr = 0;
static DWORD   s_lastArSerializeFaultCode = 0;
// UE 5.6 AssertExceptionCode (= 0x4000, see WindowsPlatformCrashContext.cpp:109).
// Raised by `::RaiseException(AssertExceptionCode, 0, 1, &Args)` where
// Args[0] = (ULONG_PTR)&FAssertInfo. We capture the message + program-counter
// so the caller can log which `check()` fired in the bridge call.
constexpr DWORD UE_ASSERT_EXCEPTION_CODE = 0x4000;
static wchar_t s_lastAssertMessage[1024] = {};
static DWORD64 s_lastAssertProgramCounter = 0;

static int sehArSerializeFilter(EXCEPTION_POINTERS* ep) {
    if (ep && ep->ExceptionRecord) {
        s_lastArSerializeFaultAddr = (DWORD64)ep->ExceptionRecord->ExceptionAddress;
        s_lastArSerializeFaultCode = ep->ExceptionRecord->ExceptionCode;
        s_lastAssertMessage[0] = 0;
        s_lastAssertProgramCounter = 0;
        // FAssertInfo capture: code 0x4000 with NumberParameters=1 carries
        // a pointer to FAssertInfo in ExceptionInformation[0]. The struct
        // layout (UE 5.6 WindowsPlatformCrashContext.cpp:192) is:
        //   const TCHAR* ErrorMessage;   // offset 0
        //   void* ProgramCounter;        // offset 8
        if (ep->ExceptionRecord->ExceptionCode == UE_ASSERT_EXCEPTION_CODE &&
            ep->ExceptionRecord->NumberParameters >= 1) {
            const ULONG_PTR* args = ep->ExceptionRecord->ExceptionInformation;
            // Validate the FAssertInfo* before reading. SEH inside SEH is
            // tricky; we use IsBadReadPtr-equivalent (VirtualQuery) for safety.
            const struct FAssertInfoLayout {
                const wchar_t* ErrorMessage;
                void*          ProgramCounter;
            }* info = (const FAssertInfoLayout*)args[0];
            MEMORY_BASIC_INFORMATION mbi = {};
            if (VirtualQuery(info, &mbi, sizeof(mbi)) == sizeof(mbi) &&
                (mbi.State & MEM_COMMIT) &&
                !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
                const wchar_t* msg = info->ErrorMessage;
                s_lastAssertProgramCounter = (DWORD64)info->ProgramCounter;
                if (msg && VirtualQuery(msg, &mbi, sizeof(mbi)) == sizeof(mbi) &&
                    (mbi.State & MEM_COMMIT) &&
                    !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
                    // Copy up to 1023 chars; the buffer is null-terminated by zero-init.
                    for (size_t i = 0; i < (sizeof(s_lastAssertMessage)/sizeof(wchar_t)) - 1; i++) {
                        wchar_t c = msg[i];
                        s_lastAssertMessage[i] = c;
                        if (c == 0) break;
                    }
                }
            }
        }
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

static bool sehCallArSerialize(void* impl, void* fn, void* archive) {
#ifdef _WIN32
    s_lastArSerializeFaultAddr = 0;
    s_lastArSerializeFaultCode = 0;
    __try {
        ((ArSerializeFn)fn)(impl, archive);
        return true;
    } __except(sehArSerializeFilter(GetExceptionInformation())) { return false; }
#else
    ((ArSerializeFn)fn)(impl, archive);
    return true;
#endif
}

// Scan the module for an ASCII string. Returns the address of the first
// occurrence, or nullptr. Used for the two AR.Serialize anchor needles.
static uint8_t* findAsciiString(uint8_t* base, size_t size, const char* str) {
    size_t len = strlen(str);
    if (len == 0) return nullptr;
    for (size_t i = 0; i + len <= size; i++) {
        if (memcmp(base + i, str, len) == 0) return base + i;
    }
    return nullptr;
}

// Climb LEA xrefs of a __FUNCTION__-style anchor string to the unique set
// of containing functions (deduped via .pdata). Returns the set of function-
// start addresses. On a healthy anchor string we expect exactly one entry.
static std::unordered_set<uint8_t*> resolveAnchorFunctions(const char* needle) {
    std::unordered_set<uint8_t*> fns;
    uint8_t* strAddr = findAsciiString(s_gm.base, s_gm.size, needle);
    if (!strAddr) {
        Hydro::logWarn("EngineAPI: AR.Serialize resolver - anchor not found: %s", needle);
        return fns;
    }
    Hydro::logInfo("EngineAPI: AR.Serialize anchor found: %s at exe+0x%zX",
        needle, (size_t)(strAddr - s_gm.base));

    std::vector<uint8_t*> leas = findAllLeaRefs(s_gm.base, s_gm.size, strAddr);
    for (uint8_t* lea : leas) {
        uint8_t* fs = funcStartViaPdata(lea);
        if (fs) fns.insert(fs);
    }
    if (fns.empty()) {
        Hydro::logWarn("EngineAPI: AR.Serialize resolver - anchor %s had no LEA xrefs "
                       "with resolvable .pdata containers", needle);
    }
    return fns;
}

// Union of E8 callers across an arbitrary set of target functions.
static std::unordered_set<uint8_t*> findE8CallersOfAny(const std::unordered_set<uint8_t*>& targets) {
    std::unordered_set<uint8_t*> callers;
    for (uint8_t* t : targets) {
        auto sub = findE8CallersOf(s_gm.base, s_gm.size, t);
        callers.insert(sub.begin(), sub.end());
    }
    return callers;
}

// Look up the .pdata RUNTIME_FUNCTION entry containing `addr` and return
// its raw byte size (EndAddress - BeginAddress). 0 if not found. Used to
// disambiguate when multiple cache_callers land in the vtable: Serialize
// is a tiny wrapper; LoadPremadeAssetRegistry is huge.
static uint32_t funcSizeViaPdata(uint8_t* addr) {
    if (!s_gm.base || addr < s_gm.base || addr >= s_gm.base + s_gm.size) return 0;
    auto* dosHeader = (IMAGE_DOS_HEADER*)s_gm.base;
    auto* ntHeaders = (IMAGE_NT_HEADERS*)(s_gm.base + dosHeader->e_lfanew);
    auto& excDir = ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (excDir.VirtualAddress == 0 || excDir.Size == 0) return 0;
    auto* funcs = (RUNTIME_FUNCTION*)(s_gm.base + excDir.VirtualAddress);
    size_t numFuncs = excDir.Size / sizeof(RUNTIME_FUNCTION);
    uint32_t targetRva = (uint32_t)(addr - s_gm.base);
    size_t lo = 0, hi = numFuncs;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (targetRva < funcs[mid].BeginAddress)        hi = mid;
        else if (targetRva >= funcs[mid].EndAddress)    lo = mid + 1;
        else return funcs[mid].EndAddress - funcs[mid].BeginAddress;
    }
    return 0;
}

// Vtable-slot probe for IAssetRegistry::Serialize(FArchive&). Walks every
// 8-byte-aligned slot in the first 0x100 bytes of s_assetRegImpl looking
// for vtable-shaped pointers (target points into the module, first few
// slots also point into the module). For each candidate vtable, probes
// slots [0..kMaxSlots] by issuing a verify-call against an empty
// FArchiveLoader; the slot whose call ticks our Serialize/TotalSize
// counters is the winner.
//
// Why this works on UE 5.6 shipping where the anchor-climb path doesn't:
//   - We hit IAssetRegistry's interface vtable (the secondary base of
//     UAssetRegistryImpl), not just UObject's vtable. The earlier
//     anchor-climb only found near-callers of CachePathsFromState - none
//     of them was Serialize.
//   - PGO/LTO function splitting is handled by funcStartViaPdata's
//     CHAININFO chain-walk (see project_pdata_chaininfo_fix.md).
//   - Mismatched-signature calls are caught by SEH; FArchiveLoader's
//     stubs return safe defaults, so a misdirected Serialize-shaped
//     virtual won't corrupt state - it just doesn't satisfy the
//     verify counters.
//   - For secondary-base vtables (offset != 0 within the impl object),
//     the call passes `this = s_assetRegImpl + offset` so the function
//     gets the correct subobject pointer.
static bool findArSerializeFnViaVtable() {
    if (s_arSerializeFn) return true;
    if (!s_assetRegImpl || !s_gm.base) return false;

    // -- Phase 1: collect candidate vtable pointers from the impl object
    struct VTC { uint16_t off; void** vt; };
    std::vector<VTC> candidates;
    for (uint16_t objOff = 0; objOff < 0x100; objOff += 8) {
        void* vt = nullptr;
        if (!safeReadPtr((uint8_t*)s_assetRegImpl + objOff, &vt)) continue;
        if (!vt) continue;
        uint8_t* vtPtr = (uint8_t*)vt;
        if (vtPtr < s_gm.base || vtPtr >= s_gm.base + s_gm.size) continue;
        // Look at first 4 slots - all should be in-module, non-null. UE
        // pure-virtual stubs aren't null; they point to __purecall, which
        // is in-module too.
        bool looksLikeVtable = true;
        for (int s = 0; s < 4; s++) {
            void* fn = nullptr;
            if (!safeReadPtr(vtPtr + s * 8, &fn) || !fn) {
                looksLikeVtable = false; break;
            }
            uint8_t* fnPtr = (uint8_t*)fn;
            if (fnPtr < s_gm.base || fnPtr >= s_gm.base + s_gm.size) {
                looksLikeVtable = false; break;
            }
        }
        if (looksLikeVtable) {
            candidates.push_back({objOff, (void**)vt});
        }
    }
    Hydro::logInfo("EngineAPI: AR.Serialize vtable probe - %zu vtable candidates "
                   "in s_assetRegImpl[0..0x100]", candidates.size());
    for (auto& c : candidates) {
        Hydro::logInfo("EngineAPI:   candidate vtable at obj+0x%X = exe+0x%zX",
                       c.off, (size_t)((uint8_t*)c.vt - s_gm.base));
    }
    if (candidates.empty()) return false;

    // -- Phase 2: probe each vtable's first kMaxSlots virtuals
    constexpr int  kMaxSlots          = 60;
    constexpr DWORD kWallTimeBudgetMs = 8000;  // hard cap: 8s

    // Probe the SECONDARY vtables first - the IAssetRegistry interface
    // vtable is the secondary base of UAssetRegistryImpl (offset != 0).
    // The UObject vtable at offset 0 contains methods like Tick / Serialize
    // (UObject's own) that may do real work and hang. Find IAssetRegistry
    // first; only fall back to offset-0 if no winner.
    std::sort(candidates.begin(), candidates.end(),
              [](const VTC& a, const VTC& b) { return a.off > b.off; });

    Hydro::FArchiveLoader::setQuietProbe(true);

    DWORD probeT0 = GetTickCount();

    uint8_t* winner = nullptr;
    void*    winnerThis = nullptr;
    uint16_t winnerOff = 0;
    int      winnerSlot = -1;
    int      winnerScore = 0;
    int      probeCount = 0;
    int      faultCount = 0;
    int      missCount  = 0;
    bool     budgetExpired = false;

    for (auto& cand : candidates) {
        if (budgetExpired) break;
        void** vtable = cand.vt;
        // Adjust `this` for secondary-base vtables - the function expects
        // a pointer to the subobject the vtable belongs to.
        void* thisPtr = (void*)((uint8_t*)s_assetRegImpl + cand.off);
        Hydro::logInfo("EngineAPI: AR.Serialize probing vtable obj+0x%X "
                       "(this=%p, %d slots)", cand.off, thisPtr, kMaxSlots);
        for (int slot = 0; slot < kMaxSlots; slot++) {
            // Wall-clock budget: any single function we call could hang
            // (real engine work executing on FArchiveLoader-as-FArchive),
            // and SEH won't catch a hang. Bail with whatever we found.
            if (GetTickCount() - probeT0 > kWallTimeBudgetMs) {
                Hydro::logWarn("EngineAPI: AR.Serialize vtable probe - "
                               "wall-time budget (%u ms) expired at "
                               "obj+0x%X slot %d (%d probes done)",
                               kWallTimeBudgetMs, cand.off, slot, probeCount);
                budgetExpired = true;
                break;
            }
            void* fn = nullptr;
            if (!safeReadPtr((uint8_t*)&vtable[slot], &fn)) break;
            if (!fn) continue;  // possible padding; don't assume end
            uint8_t* fnPtr = (uint8_t*)fn;
            if (fnPtr < s_gm.base || fnPtr >= s_gm.base + s_gm.size) continue;

            uint8_t* entry = funcStartViaPdata(fnPtr);
            if (!entry) entry = fnPtr;

            Hydro::FArchiveLoader shim(nullptr, 0);
            DWORD callT0 = GetTickCount();
            bool callOk = sehCallArSerialize(thisPtr, entry, &shim);
            DWORD callDt = GetTickCount() - callT0;
            int sCalls = shim.getSerializeCalls();
            int tCalls = shim.getTotalSizeCalls();
            int score  = sCalls + tCalls;
            probeCount++;
            if (!callOk) faultCount++;
            else if (score == 0) missCount++;

            // Always log so progress is visible - long calls are a smell.
            const char* tag = !callOk ? "FAULT"
                            : score > 0 ? "MATCH"
                            : "MISS";
            Hydro::logInfo("EngineAPI: AR.Serialize vtable[obj+0x%X][%2d] "
                           "fn=exe+0x%zX (%u ms) ser=%d tot=%d → %s",
                           cand.off, slot, (size_t)(entry - s_gm.base),
                           callDt, sCalls, tCalls, tag);

            if (callOk && score > winnerScore) {
                winner      = entry;
                winnerThis  = thisPtr;
                winnerOff   = cand.off;
                winnerSlot  = slot;
                winnerScore = score;
            }
        }
    }
    Hydro::FArchiveLoader::setQuietProbe(false);

    Hydro::logInfo("EngineAPI: AR.Serialize vtable probe - %d slots probed "
                   "(%d fault, %d miss)",
                   probeCount, faultCount, missCount);

    if (!winner) {
        Hydro::logWarn("EngineAPI: AR.Serialize vtable probe - no slot matched");
        return false;
    }

    s_arSerializeFn = winner;
    // Stash thisPtr for the eventual loadAssetRegistryBin call so it uses
    // the correct subobject pointer for the chosen vtable.
    Hydro::logInfo("EngineAPI: AR.Serialize resolved via vtable probe: "
                   "obj+0x%X vtable[%d] = exe+0x%zX (score=%d, this=%p)",
                   winnerOff, winnerSlot,
                   (size_t)(winner - s_gm.base), winnerScore, winnerThis);
    // Note: the existing loadAssetRegistryBin call site uses s_assetRegImpl
    // as `this`; that's correct only for offset-0 vtables. For
    // non-zero-offset winners, future work needs to pass `winnerThis`.
    // Phase 4 will plumb this through; for now we log it so the right
    // pointer is visible if Phase 7 lands a non-zero offset.
    return true;
}

// IAssetRegistry::Serialize(FArchive&) lives at vtable slot 124 of the
// IAssetRegistry interface vtable on UE 5.6. Derived from counting
// virtual declarations in
// `Engine/Source/Runtime/AssetRegistry/Public/AssetRegistry/IAssetRegistry.h`
// plus the 2 virtuals injected by GENERATED_IINTERFACE_BODY() at the
// top of the class:
//   slot 0  = virtual ~IAssetRegistry()        (UHT-injected)
//   slot 1  = virtual UObject* _getUObject()   (UHT-injected)
//   slot 2  = virtual bool HasAssets(...)      (user #1, line 264)
//   ...
//   slot 124 = virtual void Serialize(FArchive& Ar)  (user #123, line 1037)
//
// The C++ ABI guarantees declaration-order vtable layout regardless of
// PGO/LTO/inlining, so this index is stable across UE 5.6 hosts (DMG, SN2,
// any other UE 5.6 cook). It only changes if the IAssetRegistry header
// declares additional virtuals between the start of the class and the
// Serialize line - verified zero `#if` blocks in that range, so build
// configuration cannot perturb the count.
static constexpr int kIAssetRegistrySerializeSlot = 124;

// Static-disassembly slot finder for IAssetRegistry::Serialize(FArchive&).
//
// Finds the IAssetRegistry interface vtable as a 8-byte field within the
// first 0x100 bytes of s_assetRegImpl (verified earlier: obj+0x28 on
// DMG@5.6), then resolves slot kIAssetRegistrySerializeSlot directly.
//
// Validates by static analysis on the resolved function - we still
// disassemble the first 80 bytes and confirm the prologue dereferences
// rdx at least once (Serialize MUST touch the archive). If the static
// validation fails (count of [rdx+disp] memory operands is 0), we bail
// rather than calling a function that might be a different vtable.
//
// Read-only - never CALLS the candidates. Cannot crash the game.
static bool findArSerializeFnViaStaticAnalysis() {
    if (s_arSerializeFn) return true;
    if (!s_assetRegImpl || !s_gm.base) return false;

    ZydisDecoder decoder;
    if (ZYAN_FAILED(ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64,
                                     ZYDIS_STACK_WIDTH_64))) {
        Hydro::logWarn("EngineAPI: AR.Serialize static-analysis - Zydis init failed");
        return false;
    }

    // -- Phase 1: collect candidate vtable pointers, prefer secondary-base.
    // The IAssetRegistry interface vtable is the SECONDARY base of
    // UAssetRegistryImpl (offset != 0). The UObject vtable at offset 0
    // contains UObject's own virtuals - different layout, different slot
    // numbering. We MUST pick a non-zero offset.
    struct VTC { uint16_t off; void** vt; };
    std::vector<VTC> vtables;
    for (uint16_t objOff = 0; objOff < 0x100; objOff += 8) {
        void* vt = nullptr;
        if (!safeReadPtr((uint8_t*)s_assetRegImpl + objOff, &vt)) continue;
        if (!vt) continue;
        uint8_t* vtPtr = (uint8_t*)vt;
        if (vtPtr < s_gm.base || vtPtr >= s_gm.base + s_gm.size) continue;
        bool looksLikeVtable = true;
        for (int s = 0; s < 4; s++) {
            void* fn = nullptr;
            if (!safeReadPtr(vtPtr + s * 8, &fn) || !fn) { looksLikeVtable = false; break; }
            uint8_t* fnPtr = (uint8_t*)fn;
            if (fnPtr < s_gm.base || fnPtr >= s_gm.base + s_gm.size) {
                looksLikeVtable = false; break;
            }
        }
        if (looksLikeVtable) vtables.push_back({objOff, (void**)vt});
    }
    if (vtables.empty()) {
        Hydro::logWarn("EngineAPI: AR.Serialize static-analysis - no vtables found");
        return false;
    }

    // Pick the first non-zero-offset vtable (the IAssetRegistry interface).
    VTC* arVtable = nullptr;
    for (auto& v : vtables) {
        if (v.off != 0) { arVtable = &v; break; }
    }
    if (!arVtable) {
        Hydro::logWarn("EngineAPI: AR.Serialize static-analysis - no secondary-base "
                       "vtable on s_assetRegImpl (UAssetRegistryImpl is supposed to "
                       "inherit IAssetRegistry as secondary base)");
        return false;
    }
    Hydro::logInfo("EngineAPI: AR.Serialize static-analysis - IAssetRegistry vtable "
                   "at obj+0x%X = exe+0x%zX",
                   arVtable->off, (size_t)((uint8_t*)arVtable->vt - s_gm.base));

    // -- Phase 2: read slot kIAssetRegistrySerializeSlot directly.
    void* fn = nullptr;
    if (!safeReadPtr((uint8_t*)&arVtable->vt[kIAssetRegistrySerializeSlot], &fn) ||
        !fn) {
        Hydro::logWarn("EngineAPI: AR.Serialize static-analysis - slot %d unreadable",
                       kIAssetRegistrySerializeSlot);
        return false;
    }
    uint8_t* fnPtr = (uint8_t*)fn;
    if (fnPtr < s_gm.base || fnPtr >= s_gm.base + s_gm.size) {
        Hydro::logWarn("EngineAPI: AR.Serialize static-analysis - slot %d points "
                       "outside module (%p)", kIAssetRegistrySerializeSlot, fnPtr);
        return false;
    }
    uint8_t* entry = funcStartViaPdata(fnPtr);
    if (!entry) entry = fnPtr;

    Hydro::logInfo("EngineAPI: AR.Serialize static-analysis - slot %d resolved to "
                   "exe+0x%zX (chain head: exe+0x%zX, size=%u)",
                   kIAssetRegistrySerializeSlot,
                   (size_t)(fnPtr - s_gm.base), (size_t)(entry - s_gm.base),
                   funcSizeViaPdata(entry));

    // Dump first 64 bytes of the resolved function so we can verify by
    // eye that the prologue looks like Serialize (not __purecall, not a
    // wildly-different function that would imply our slot count is wrong).
    if (entry + 64 <= s_gm.base + s_gm.size) {
        auto hexLine = [](uint8_t* p, int n) {
            static thread_local char buf[3 * 16 + 1];
            int o = 0;
            for (int i = 0; i < n; i++) {
                o += std::snprintf(buf + o, sizeof(buf) - o, "%02X ", p[i]);
            }
            return std::string(buf, o > 0 ? o - 1 : 0);
        };
        Hydro::logInfo("EngineAPI: AR.Serialize first 64 bytes:");
        for (int b = 0; b < 64; b += 16) {
            Hydro::logInfo("EngineAPI:   [+%02X..%02X]: %s",
                           b, b + 15, hexLine(entry + b, 16).c_str());
        }
    }

    // Sanity: __purecall is a generic stub used for unimplemented pure
    // virtuals. It's tiny - a near-jump to the runtime helper. If our
    // resolved address is in such a stub, our slot count must be wrong.
    // Detect: if size < 16 bytes AND first byte is E9 (near-jump), bail.
    uint32_t fnSize = funcSizeViaPdata(entry);
    if (fnSize > 0 && fnSize < 16 && entry[0] == 0xE9) {
        Hydro::logWarn("EngineAPI: AR.Serialize static-analysis - slot %d looks "
                       "like __purecall stub (size=%u, jmp); slot count likely off",
                       kIAssetRegistrySerializeSlot, fnSize);
        return false;
    }

    // MSVC adjustor-thunk detector. Pattern: `48 83 E9 imm8; E9 rel32`
    // (sub rcx, imm8; jmp body). When the slot points at a thunk, .pdata
    // has no entry (size==0), and the body lives at `entry + 9 + rel32`.
    // Follow the jump to the real body, derive the this-adjustment from
    // the thunk's `sub rcx, imm8` (ground truth), and validate that the
    // body has its own .pdata entry. If neither, reject - the caller will
    // surface a discovery miss instead of crashing on a wrongly-shaped call.
    if (funcSizeViaPdata(entry) == 0 &&
        entry + 9 <= s_gm.base + s_gm.size &&
        entry[0] == 0x48 && entry[1] == 0x83 && entry[2] == 0xE9 &&
        entry[4] == 0xE9) {
        int8_t thisAdjust = (int8_t)entry[3];
        int32_t rel = *(int32_t*)(entry + 5);
        uint8_t* body = entry + 9 + rel;
        if (body < s_gm.base || body >= s_gm.base + s_gm.size) {
            Hydro::logWarn("EngineAPI: AR.Serialize static-analysis - slot %d is "
                           "adjustor thunk but jmp target out of module",
                           kIAssetRegistrySerializeSlot);
            return false;
        }
        uint8_t* bodyStart = funcStartViaPdata(body);
        if (!bodyStart) {
            Hydro::logWarn("EngineAPI: AR.Serialize static-analysis - slot %d is "
                           "adjustor thunk → exe+0x%zX (no .pdata); rejecting",
                           kIAssetRegistrySerializeSlot,
                           (size_t)(body - s_gm.base));
            return false;
        }
        Hydro::logInfo("EngineAPI: AR.Serialize static-analysis - slot %d is "
                       "adjustor thunk; following to exe+0x%zX (this-adjust=0x%X)",
                       kIAssetRegistrySerializeSlot,
                       (size_t)(bodyStart - s_gm.base), (uint8_t)thisAdjust);
        entry = bodyStart;
        s_arSerializeFn = entry;
        s_arSerializeThisOffset = (uint8_t)thisAdjust;
        return true;
    }

    s_arSerializeFn = entry;
    s_arSerializeThisOffset = arVtable->off;
    Hydro::logInfo("EngineAPI: AR.Serialize resolved at exe+0x%zX (vtable obj+0x%X "
                   "slot %d, source-counted; will pass this=s_assetRegImpl+0x%X "
                   "to satisfy MSVC adjustor thunk)",
                   (size_t)(entry - s_gm.base), arVtable->off,
                   kIAssetRegistrySerializeSlot, arVtable->off);
    return true;
}

// Legacy heuristic-scan static-analysis (Phase 10). Disabled - superseded
// by the source-counted slot index in findArSerializeFnViaStaticAnalysis
// above. Kept for reference; the heuristic was too biased to pick up
// thin forwarding wrappers like the real Serialize.
[[maybe_unused]] static bool findArSerializeFnViaStaticAnalysisHeuristic() {
    if (s_arSerializeFn) return true;
    if (!s_assetRegImpl || !s_gm.base) return false;

    ZydisDecoder decoder;
    if (ZYAN_FAILED(ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64,
                                     ZYDIS_STACK_WIDTH_64))) {
        return false;
    }

    struct VTC { uint16_t off; void** vt; };
    std::vector<VTC> vtables;
    for (uint16_t objOff = 0; objOff < 0x100; objOff += 8) {
        void* vt = nullptr;
        if (!safeReadPtr((uint8_t*)s_assetRegImpl + objOff, &vt)) continue;
        if (!vt) continue;
        uint8_t* vtPtr = (uint8_t*)vt;
        if (vtPtr < s_gm.base || vtPtr >= s_gm.base + s_gm.size) continue;
        bool looksLikeVtable = true;
        for (int s = 0; s < 4; s++) {
            void* fn = nullptr;
            if (!safeReadPtr(vtPtr + s * 8, &fn) || !fn) { looksLikeVtable = false; break; }
            uint8_t* fnPtr = (uint8_t*)fn;
            if (fnPtr < s_gm.base || fnPtr >= s_gm.base + s_gm.size) {
                looksLikeVtable = false; break;
            }
        }
        if (looksLikeVtable) vtables.push_back({objOff, (void**)vt});
    }
    if (vtables.empty()) return false;

    // -- Phase 2: score every slot in every secondary-base vtable.
    // The IAssetRegistry interface vtable is the secondary base of
    // UAssetRegistryImpl (offset != 0). The UObject vtable at offset 0
    // contains AActor/UObject methods; we still score it but expect the
    // IAssetRegistry vtable to contain the highest-scoring slot.
    constexpr int  kMaxSlots = 64;
    constexpr int  kProbeBytes = 80;
    constexpr int  kBaseRdxBonus    = 5;   // [rdx+...] memory operand
    constexpr int  kBaseRdxIndirect = 8;   // call/jmp [rdx+...] (vtable dispatch)
    constexpr int  kRdxAsValue      = -2;  // rdx used as plain register value
                                           //   (penalize functions that don't deref)

    struct SlotResult {
        uint16_t vtOff;
        int      slot;
        uint8_t* fnEntry;
        int      score;
        int      rdxMem;
        int      rdxIndirect;
        int      rdxValue;
    };
    std::vector<SlotResult> all;
    all.reserve(vtables.size() * kMaxSlots);

    // Track which registers currently hold a value derived from rdx (the
    // FArchive parameter). MSVC PGO commonly emits `mov rsi, rdx` early so
    // the rest of the function dereferences via rsi rather than rdx - we
    // need to follow that flow to score correctly.
    auto isArchiveReg = [](uint64_t mask, ZydisRegister r) {
        if (r == ZYDIS_REGISTER_NONE) return false;
        // Map register to a bit. Use the largest-enclosing 64-bit reg.
        ZydisRegister r64 = ZydisRegisterGetLargestEnclosing(
            ZYDIS_MACHINE_MODE_LONG_64, r);
        if (r64 == ZYDIS_REGISTER_NONE || r64 < ZYDIS_REGISTER_RAX ||
            r64 > ZYDIS_REGISTER_R15) return false;
        return (mask & (1ULL << (r64 - ZYDIS_REGISTER_RAX))) != 0;
    };
    auto setArchiveReg = [](uint64_t& mask, ZydisRegister r, bool on) {
        ZydisRegister r64 = ZydisRegisterGetLargestEnclosing(
            ZYDIS_MACHINE_MODE_LONG_64, r);
        if (r64 == ZYDIS_REGISTER_NONE || r64 < ZYDIS_REGISTER_RAX ||
            r64 > ZYDIS_REGISTER_R15) return;
        uint64_t bit = 1ULL << (r64 - ZYDIS_REGISTER_RAX);
        if (on) mask |= bit;
        else    mask &= ~bit;
    };

    for (auto& v : vtables) {
        for (int slot = 0; slot < kMaxSlots; slot++) {
            void* fn = nullptr;
            if (!safeReadPtr((uint8_t*)&v.vt[slot], &fn)) break;
            if (!fn) continue;
            uint8_t* fnPtr = (uint8_t*)fn;
            if (fnPtr < s_gm.base || fnPtr >= s_gm.base + s_gm.size) continue;

            uint8_t* entry = funcStartViaPdata(fnPtr);
            if (!entry) entry = fnPtr;

            int rdxMem = 0;       // any [archiveReg+disp] memory operand
            int rdxIndirect = 0;  // call/jmp through reg loaded from archiveReg
            int rdxValue = 0;     // archiveReg mentioned as plain register
            int pos = 0;
            // Bitmask: archiveRegs[i] = 1 iff (RAX+i) holds an archive-derived
            // value. Initialized to RDX bit (the parameter).
            uint64_t archiveRegs = 1ULL << (ZYDIS_REGISTER_RDX - ZYDIS_REGISTER_RAX);
            // Vtable-loaded registers: regs whose value is `[archiveReg]`,
            // used to detect the FArchive vtable dispatch pattern.
            uint64_t archiveVtableRegs = 0;

            while (pos < kProbeBytes) {
                ZydisDecodedInstruction inst;
                ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
                if (ZYAN_FAILED(ZydisDecoderDecodeFull(&decoder, entry + pos,
                                                       kProbeBytes - pos,
                                                       &inst, ops))) break;
                bool sawArchiveDeref = false;
                for (int i = 0; i < inst.operand_count_visible; i++) {
                    auto& op = ops[i];
                    if (op.type == ZYDIS_OPERAND_TYPE_MEMORY) {
                        if (isArchiveReg(archiveRegs, op.mem.base)) {
                            rdxMem++;
                            sawArchiveDeref = true;
                        }
                        if ((inst.mnemonic == ZYDIS_MNEMONIC_CALL ||
                             inst.mnemonic == ZYDIS_MNEMONIC_JMP) &&
                            isArchiveReg(archiveVtableRegs, op.mem.base)) {
                            rdxIndirect++;
                        }
                    } else if (op.type == ZYDIS_OPERAND_TYPE_REGISTER) {
                        if (isArchiveReg(archiveRegs, op.reg.value) &&
                            !sawArchiveDeref) {
                            rdxValue++;
                        }
                    }
                }

                // Update register taint:
                // 1. `mov reg, archiveReg`         → reg becomes archiveReg
                // 2. `mov reg, [archiveReg+disp]`  → reg becomes archiveVtableReg
                // 3. Any other write to reg        → clears taint on reg
                if ((inst.mnemonic == ZYDIS_MNEMONIC_MOV ||
                     inst.mnemonic == ZYDIS_MNEMONIC_LEA) &&
                    inst.operand_count_visible >= 2 &&
                    ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER) {
                    ZydisRegister dst = ops[0].reg.value;
                    bool tainted = false;
                    bool vtableLoaded = false;
                    if (ops[1].type == ZYDIS_OPERAND_TYPE_REGISTER &&
                        isArchiveReg(archiveRegs, ops[1].reg.value)) {
                        tainted = true;  // mov dst, archiveReg
                    } else if (ops[1].type == ZYDIS_OPERAND_TYPE_MEMORY &&
                               isArchiveReg(archiveRegs, ops[1].mem.base)) {
                        // mov dst, [archiveReg+disp] - dst now holds an
                        // archive field. If disp==0 (or is the FArchive
                        // vtable offset, which is 0 because vtable is at
                        // the start of the FArchive object), dst is the
                        // FArchive vtable.
                        if (ops[1].mem.disp.value == 0) {
                            vtableLoaded = true;
                        }
                    }
                    if (tainted)        setArchiveReg(archiveRegs, dst, true);
                    else                setArchiveReg(archiveRegs, dst, false);
                    if (vtableLoaded)   setArchiveReg(archiveVtableRegs, dst, true);
                    else                setArchiveReg(archiveVtableRegs, dst, false);
                }
                if (inst.mnemonic == ZYDIS_MNEMONIC_RET) break;
                pos += inst.length;
            }

            int score = rdxMem * kBaseRdxBonus
                      + rdxIndirect * kBaseRdxIndirect
                      + rdxValue * kRdxAsValue;
            all.push_back({v.off, slot, entry, score,
                           rdxMem, rdxIndirect, rdxValue});
        }
    }

    if (all.empty()) {
        Hydro::logWarn("EngineAPI: AR.Serialize static-analysis - no slots scored");
        return false;
    }

    // Pick the highest-scoring slot. Log the top 5 + dump first 64 bytes
    // of the top 3 so we can sanity-check the winner by eye.
    std::sort(all.begin(), all.end(),
              [](const SlotResult& a, const SlotResult& b) { return a.score > b.score; });
    Hydro::logInfo("EngineAPI: AR.Serialize static-analysis - top 5 of %zu slots:",
                   all.size());
    for (size_t i = 0; i < (std::min)((size_t)5, all.size()); i++) {
        const auto& r = all[i];
        Hydro::logInfo("EngineAPI:   #%zu obj+0x%X vtable[%2d] = exe+0x%zX  "
                       "score=%d (rdxMem=%d rdxIndirect=%d rdxValue=%d)",
                       i, r.vtOff, r.slot, (size_t)(r.fnEntry - s_gm.base),
                       r.score, r.rdxMem, r.rdxIndirect, r.rdxValue);
    }
    auto hexLine = [](uint8_t* p, int n) {
        static thread_local char buf[3 * 16 + 1];
        int o = 0;
        for (int i = 0; i < n; i++) {
            o += std::snprintf(buf + o, sizeof(buf) - o, "%02X ", p[i]);
        }
        return std::string(buf, o > 0 ? o - 1 : 0);
    };
    for (size_t i = 0; i < (std::min)((size_t)3, all.size()); i++) {
        const auto& r = all[i];
        if (r.fnEntry + 64 > s_gm.base + s_gm.size) continue;
        Hydro::logInfo("EngineAPI:   #%zu first 64 bytes:", i);
        for (int b = 0; b < 64; b += 16) {
            Hydro::logInfo("EngineAPI:     [+%02X..%02X]: %s",
                           b, b + 15, hexLine(r.fnEntry + b, 16).c_str());
        }
    }

    // Threshold: top score must be ≥ 10 (any real Serialize touches the
    // archive at least twice) AND must beat runner-up by ≥ 4 points
    // (one extra rdxMem). The original "within 50% = ambiguous" guard
    // was too strict - score=18 vs score=13 is a clear differential
    // when the metric is "discrete count of memory accesses through rdx".
    if (all[0].score < 10) {
        Hydro::logWarn("EngineAPI: AR.Serialize static-analysis - top score %d "
                       "below threshold 10; no confident winner",
                       all[0].score);
        return false;
    }
    if (all.size() > 1 && all[0].score - all[1].score < 4) {
        Hydro::logWarn("EngineAPI: AR.Serialize static-analysis - top score %d "
                       "only %d points above runner-up %d; no confident winner",
                       all[0].score, all[0].score - all[1].score, all[1].score);
        return false;
    }
    // Prefer winners on a non-zero offset vtable (the secondary base
    // is IAssetRegistry; offset 0 is UObject's vtable). If our top is
    // on offset 0 but a slot on a non-zero offset scores within 4
    // points, prefer the non-zero one.
    size_t winnerIdx = 0;
    if (all[0].vtOff == 0) {
        for (size_t i = 1; i < all.size(); i++) {
            if (all[i].vtOff != 0 && all[0].score - all[i].score < 4) {
                winnerIdx = i;
                Hydro::logInfo("EngineAPI: AR.Serialize static-analysis - "
                               "preferring obj+0x%X #%zu over obj+0x0 #0 "
                               "(score gap %d acceptable)",
                               all[i].vtOff, i, all[0].score - all[i].score);
                break;
            }
        }
    }

    s_arSerializeFn = all[winnerIdx].fnEntry;
    Hydro::logInfo("EngineAPI: AR.Serialize resolved at exe+0x%zX via static analysis "
                   "(vtable obj+0x%X slot %d, score=%d)",
                   (size_t)(all[winnerIdx].fnEntry - s_gm.base),
                   all[winnerIdx].vtOff, all[winnerIdx].slot, all[winnerIdx].score);
    return true;
}

// Find UAssetRegistryImpl::Serialize via single-anchor + size-heuristic +
// verify-call. See header comment for rationale (the vtable-membership
// approach fails because UE 5.6 keeps Serialize as IAssetRegistry::Serialize
// only - the UObject vtable we read doesn't contain it).
static bool findArSerializeFn() {
    // Static-disassembly slot finder - the right approach. Empirically
    // ruled out the alternatives 2026-05-09:
    //   - Anchor-driven E8 climb (CachePathsFromState): wrong call graph;
    //     1-hop AND 2-hop expansions both yielded zero candidates that
    //     interact with the FArchive (verified with widened heuristic
    //     across all 7 FArchiveLoader counters, score=0 every candidate).
    //   - Vtable-slot verify-call probe: unsafe - calling random AR
    //     interface methods with an unrelated arg shape side-effects AR
    //     state and triggers delayed crashes (no SEH-catchable signal).
    // Static disassembly never CALLS anything; it only reads bytes.
    if (findArSerializeFnViaStaticAnalysis()) return true;

    // NOTE: vtable-slot probe (findArSerializeFnViaVtable) is DISABLED.
    // Empirically validated 2026-05-09 morning on DMG@5.6: even
    // SEH-survived MISS calls side-effect AR state because the called
    // engine functions touch `this`, and a few probes in the AR
    // crashed the process from a delayed corruption (no SEH-catchable
    // signal, no minidump). The verify-call approach is only safe
    // when restricted to known-good candidates - which is what the
    // anchor-driven E8 climb below produces (functions in AR's own
    // call graph reachable from CachePathsFromState).
    (void)&findArSerializeFnViaVtable;  // keep symbol live for diag use

    if (s_arSerializeFn) return true;
    if (!s_gm.base) return false;
    if (!s_assetRegImpl) {
        Hydro::logWarn("EngineAPI: AR.Serialize resolver - s_assetRegImpl not discovered yet; "
                       "deferring resolve until loadAssetRegistryBin");
        return false;
    }

    DWORD t0 = GetTickCount();

    // -- Step 1: anchor → cache_fn_set -----------------------------------
    auto cacheFns = resolveAnchorFunctions("FAssetRegistryImpl::CachePathsFromState");
    if (cacheFns.empty()) {
        Hydro::logWarn("EngineAPI: AR.Serialize resolver - CachePathsFromState anchor missing; "
                       "AR.bin merge unavailable");
        return false;
    }
    Hydro::logInfo("EngineAPI: AR.Serialize resolver - cache_fn_set size=%zu", cacheFns.size());

    {
        int idx = 0;
        for (uint8_t* fn : cacheFns) {
            uint32_t sz = funcSizeViaPdata(fn);
            Hydro::logInfo("EngineAPI: AR.Serialize: cache_fn[%d] = exe+0x%zX, size=%u bytes",
                           idx++, (size_t)(fn - s_gm.base), sz);
        }
    }

    // -- Step 2: E8-callers of cache_fn_set ------------------------------
    auto cacheCallers = findE8CallersOfAny(cacheFns);
    if (cacheCallers.empty()) {
        Hydro::logWarn("EngineAPI: AR.Serialize resolver - no E8 callers of cache_fn; "
                       "shipping build may have no near-call sites");
        return false;
    }

    {
        int idx = 0;
        for (uint8_t* c : cacheCallers) {
            uint32_t sz = funcSizeViaPdata(c);
            Hydro::logInfo("EngineAPI: AR.Serialize: cache_caller[%d] = exe+0x%zX, size=%u bytes",
                           idx++, (size_t)(c - s_gm.base), sz);
        }
    }

    // -- Step 2.5: resolve callers to true function entries --------------
    // PGO/LTO chain-splits functions into multiple .pdata regions; the raw
    // addresses in cacheCallers may land in continuation regions whose
    // BeginAddress is mid-function (and whose body uses r11 as a frame
    // pointer set up by the parent's prologue). Calling those directly
    // crashes. funcStartViaPdata now follows UNW_FLAG_CHAININFO to the
    // primary region - the real prologue. Dedupe across multiple
    // continuations of the same function.
    std::vector<uint8_t*> hop1Resolved;
    {
        std::unordered_set<uint8_t*> seen;
        for (uint8_t* c : cacheCallers) {
            uint8_t* entry = funcStartViaPdata(c);
            if (!entry) entry = c;  // fall back to raw addr if no .pdata
            if (seen.insert(entry).second) {
                hop1Resolved.push_back(entry);
            }
        }
        int idx = 0;
        for (uint8_t* e : hop1Resolved) {
            uint32_t sz = funcSizeViaPdata(e);
            Hydro::logInfo("EngineAPI: AR.Serialize: hop1_caller[%d] = exe+0x%zX, "
                           "size=%u bytes (entry of chain)",
                           idx++, (size_t)(e - s_gm.base), sz);
        }
    }

    // -- Step 2.75: 2-hop E8 expansion -----------------------------------
    // 1-hop verify proved all 3 hop1 candidates were not Serialize
    // (DMG@5.6, 2026-05-09 morning): two FAULT, one MISS. The actual
    // IAssetRegistry::Serialize wrapper sits one more hop up - it
    // calls into a hop1 helper that calls CachePathsFromState. Climb
    // one more level: union of E8 callers of every hop1 entry, then
    // chain-resolve and dedupe.
    //
    // Safety: the candidate pool stays in AR's call graph - these are
    // real callers of AR helpers, not arbitrary engine functions. Same
    // verify-call pattern (FArchiveLoader instrumentation + SEH) the
    // 1-hop run already proved safe.
    std::vector<uint8_t*> resolvedCallers;
    {
        std::unordered_set<uint8_t*> seenEntry;
        // Seed with hop1 entries - Serialize might be 1-hop in some
        // hosts even if it isn't on DMG@5.6.
        for (uint8_t* e : hop1Resolved) {
            if (seenEntry.insert(e).second) resolvedCallers.push_back(e);
        }
        std::unordered_set<uint8_t*> hop1Set(hop1Resolved.begin(), hop1Resolved.end());
        auto hop2Raw = findE8CallersOfAny(hop1Set);
        for (uint8_t* c : hop2Raw) {
            uint8_t* entry = funcStartViaPdata(c);
            if (!entry) entry = c;
            if (seenEntry.insert(entry).second) {
                resolvedCallers.push_back(entry);
            }
        }
        Hydro::logInfo("EngineAPI: AR.Serialize 2-hop expansion - "
                       "%zu hop2 callers → %zu unique candidates after dedupe",
                       hop2Raw.size(), resolvedCallers.size());
        int idx = 0;
        for (uint8_t* e : resolvedCallers) {
            uint32_t sz = funcSizeViaPdata(e);
            const char* tag = (idx < (int)hop1Resolved.size()) ? "hop1" : "hop2";
            Hydro::logInfo("EngineAPI: AR.Serialize: candidate[%d] (%s) = exe+0x%zX, "
                           "size=%u bytes",
                           idx, tag, (size_t)(e - s_gm.base), sz);
            idx++;
        }
    }

    // -- Diagnostic dump (HYDRO_AR_SERIALIZE_DUMP=1) ---------------------
    // PGO/LTO splits functions into multiple .pdata entries on UE 5.6
    // shipping. The size-heuristic verify call crashes because the picked
    // address is mid-function (not a valid call target). This dump prints,
    // for every cache_caller:
    //   - addr vs funcStartViaPdata(addr): if they differ, addr is mid-fn
    //   - 32 bytes BEFORE addr (so a disassembler can find the prologue)
    //   - 64 bytes AT addr        (instruction stream at the caller site)
    //   - 64 bytes AT funcStart   (the real function entry per .pdata)
    // After dumping, returns false so the verify-call (which crashes) is
    // skipped. Use the dump to identify the true Serialize entry.
    {
        char dumpBuf[8] = {};
        DWORD dumpLen = GetEnvironmentVariableA("HYDRO_AR_SERIALIZE_DUMP",
                                                dumpBuf, sizeof(dumpBuf));
        if (dumpLen > 0 && dumpLen < sizeof(dumpBuf) && dumpBuf[0] == '1') {
            Hydro::logInfo("EngineAPI: AR.Serialize DUMP - diagnostic mode "
                           "(HYDRO_AR_SERIALIZE_DUMP=1); skipping verify-call");
            auto hexLine = [](uint8_t* p, int n) {
                static thread_local char buf[3 * 64 + 1];
                int o = 0;
                for (int i = 0; i < n; i++) {
                    o += std::snprintf(buf + o, sizeof(buf) - o, "%02X ", p[i]);
                }
                return std::string(buf, o > 0 ? o - 1 : 0);
            };
            int idx = 0;
            for (uint8_t* c : resolvedCallers) {
                uint8_t* fnStart = funcStartViaPdata(c);
                uint32_t sz      = funcSizeViaPdata(c);
                bool isEntry     = (fnStart == c);
                Hydro::logInfo(
                    "EngineAPI: AR.Serialize DUMP[%d]: addr=exe+0x%zX  "
                    "pdataStart=exe+0x%zX  size=%u  isEntry=%s",
                    idx,
                    (size_t)(c - s_gm.base),
                    fnStart ? (size_t)(fnStart - s_gm.base) : (size_t)0,
                    sz,
                    isEntry ? "yes" : "no (FRAGMENT)");

                // 32 bytes before addr (clamped to module base)
                uint8_t* pre = c - 32;
                if (pre >= s_gm.base) {
                    Hydro::logInfo("EngineAPI:   pre[-32..-17]: %s",
                                   hexLine(pre, 16).c_str());
                    Hydro::logInfo("EngineAPI:   pre[-16..-01]: %s",
                                   hexLine(pre + 16, 16).c_str());
                }
                // 64 bytes at addr
                if (c + 64 <= s_gm.base + s_gm.size) {
                    Hydro::logInfo("EngineAPI:   at[+00..+15]:  %s",
                                   hexLine(c, 16).c_str());
                    Hydro::logInfo("EngineAPI:   at[+16..+31]:  %s",
                                   hexLine(c + 16, 16).c_str());
                    Hydro::logInfo("EngineAPI:   at[+32..+47]:  %s",
                                   hexLine(c + 32, 16).c_str());
                    Hydro::logInfo("EngineAPI:   at[+48..+63]:  %s",
                                   hexLine(c + 48, 16).c_str());
                }
                // 64 bytes at .pdata BeginAddress (the real function entry)
                if (fnStart && !isEntry &&
                    fnStart + 64 <= s_gm.base + s_gm.size) {
                    Hydro::logInfo("EngineAPI:   fnStart[+00..+15]: %s",
                                   hexLine(fnStart, 16).c_str());
                    Hydro::logInfo("EngineAPI:   fnStart[+16..+31]: %s",
                                   hexLine(fnStart + 16, 16).c_str());
                    Hydro::logInfo("EngineAPI:   fnStart[+32..+47]: %s",
                                   hexLine(fnStart + 32, 16).c_str());
                    Hydro::logInfo("EngineAPI:   fnStart[+48..+63]: %s",
                                   hexLine(fnStart + 48, 16).c_str());
                }
                idx++;
            }
            Hydro::logInfo("EngineAPI: AR.Serialize DUMP - complete (%zu callers); "
                           "feed into disassembler. Resolver returning false.",
                           resolvedCallers.size());
            return false;
        }
    }

    // -- Step 3: size-filter resolved_callers ----------------------------
    // Wrappers are tiny; LoadPremadeAssetRegistry-shaped helpers are huge.
    // 1500-byte ceiling: the chained-region size for DMG@5.6 was 139, but
    // the actual function (sum of all chained regions) is larger. Use a
    // generous bound; the verify-call disambiguates among survivors.
    static constexpr uint32_t kMaxWrapperBytes = 1500;
    std::vector<std::pair<uint32_t, uint8_t*>> candidates;  // (size, addr)
    candidates.reserve(resolvedCallers.size());
    for (uint8_t* c : resolvedCallers) {
        uint32_t sz = funcSizeViaPdata(c);
        // Functions chained across many regions still report the entry
        // region's size. That's fine - we just want to drop thunks (size 0)
        // and obvious giants. Don't over-filter.
        if (sz == 0 || sz > kMaxWrapperBytes) continue;
        candidates.emplace_back(sz, c);
    }
    if (candidates.empty()) {
        Hydro::logWarn("EngineAPI: AR.Serialize resolver - size filter eliminated all "
                       "%zu resolved_callers (none in (0, %u] bytes); AR.bin merge unavailable",
                       resolvedCallers.size(), kMaxWrapperBytes);
        return false;
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const auto& a, const auto& b) {
                  if (a.first != b.first) return a.first < b.first;
                  return a.second < b.second;
              });

    Hydro::logInfo("EngineAPI: AR.Serialize resolver - size filter kept %zu of %zu "
                   "resolved_callers in (0, %u] bytes",
                   candidates.size(), resolvedCallers.size(), kMaxWrapperBytes);

    // -- Step 4: verify-call disambiguation ------------------------------
    // Wider match heuristic: ANY interaction with FArchiveLoader counts as
    // a Serialize signature. Original heuristic only checked Serialize+
    // TotalSize counts and missed real Serialize-shaped functions that
    // primarily use << operators (m_stubHits) or SerializeBits/Bool/Int
    // (m_otherCalls) or seeking (m_seekCalls/m_tellCalls/m_atEndCalls).
    // Total-interaction score breaks ties - the real Serialize wrapper
    // touches the archive far more than an unrelated function that
    // happens to take an FArchive&.
    auto totalInteractions = [](const Hydro::FArchiveLoader& s) {
        return s.getSerializeCalls() + s.getTotalSizeCalls()
             + s.getAtEndCalls()    + s.getSeekCalls()
             + s.getTellCalls()     + s.getStubHits()
             + s.getOtherCalls();
    };
    uint8_t* winner = nullptr;
    int winnerIdx = -1;
    int winnerScore = 0;
    int winnerS = 0, winnerT = 0, winnerStub = 0, winnerOther = 0;
    Hydro::FArchiveLoader::setQuietProbe(true);
    for (size_t i = 0; i < candidates.size(); i++) {
        uint8_t* cand = candidates[i].second;
        uint32_t sz   = candidates[i].first;
        Hydro::FArchiveLoader shim(nullptr, 0);
        bool callOk = sehCallArSerialize(s_assetRegImpl, cand, &shim);
        int sCalls    = shim.getSerializeCalls();
        int tCalls    = shim.getTotalSizeCalls();
        int aCalls    = shim.getAtEndCalls();
        int seCalls   = shim.getSeekCalls();
        int tlCalls   = shim.getTellCalls();
        int stubHits  = shim.getStubHits();
        int otherCalls= shim.getOtherCalls();
        int score     = callOk ? totalInteractions(shim) : 0;
        const char* tag = !callOk ? "FAULT"
                        : score > 0 ? "MATCH"
                        : "MISS";
        Hydro::logInfo("EngineAPI: AR.Serialize verify: candidate exe+0x%zX (size=%u) "
                       "→ ser=%d tot=%d atEnd=%d seek=%d tell=%d stub=%d other=%d "
                       "score=%d → %s",
                       (size_t)(cand - s_gm.base), sz,
                       sCalls, tCalls, aCalls, seCalls, tlCalls,
                       stubHits, otherCalls, score, tag);
        // Pick the candidate with the highest interaction score (don't
        // break on first match - score lets us disambiguate when multiple
        // candidates touch the archive).
        if (callOk && score > winnerScore) {
            winner       = cand;
            winnerIdx    = (int)i;
            winnerScore  = score;
            winnerS      = sCalls;
            winnerT      = tCalls;
            winnerStub   = stubHits;
            winnerOther  = otherCalls;
        }
    }
    Hydro::FArchiveLoader::setQuietProbe(false);

    if (!winner) {
        Hydro::logWarn("EngineAPI: AR.Serialize resolver - no candidate verified "
                       "(tried %zu); AR.bin merge unavailable", candidates.size());
        return false;
    }

    s_arSerializeFn = winner;
    Hydro::logInfo("EngineAPI: AR.Serialize resolved at exe+0x%zX via wide verify "
                   "(candidate %d of %zu, ser=%d tot=%d stub=%d other=%d score=%d, "
                   "total %lu ms)",
                   (size_t)(winner - s_gm.base), winnerIdx + 1, candidates.size(),
                   winnerS, winnerT, winnerStub, winnerOther, winnerScore,
                   GetTickCount() - t0);
    return true;
}

// FAssetRegistryImpl::Serialize resolver. Anchors on the 16-byte
// FAssetRegistryVersion::GUID literal in .rdata and climbs:
//
//   GUID literal (.rdata)
//     ↓ LEA refs                     (loaded by SerializeVersion's body)
//   FAssetRegistryVersion::SerializeVersion
//     ↓ E8 callers                   (typically 1 - SerializeHeader)
//   FAssetRegistryHeader::SerializeHeader
//     ↓ E8 callers                   (2: Save, Load - disambiguate by r9 use)
//   FAssetRegistryState::Load
//
// Why this works: every step is built on the previous resolver primitives
// (findAllLeaRefs, findE8CallersOf{,Any}, funcStartViaPdata) that are
// already proven on UE 5.6 PGO/LTO shipping. The disambiguator is robust:
// `Load` writes through `OutVersion` (4th arg = r9) very early; `Save`
// has only 2 args.
//
// Source-validated counts:
//   Engine/Source/Runtime/CoreUObject/Private/AssetRegistry/AssetData.cpp:35
//     const FGuid FAssetRegistryVersion::GUID(
//         0x717F9EE7, 0xE9B0493A, 0x88B39132, 0x1B388107);
//   16 bytes little-endian:
//     E7 9E 7F 71 3A 49 B0 E9 32 91 B3 88 07 81 38 1B
//
// The State offset within s_assetRegImpl: GuardedData is the first
// non-base data member of UAssetRegistryImpl, immediately after the
// IAssetRegistry secondary-base vtable at obj+0x28. State is the first
// data member of FAssetRegistryImpl (= GuardedData) at offset 0 within
// it. So State should be at obj+0x30 - verified dynamically by
// disassembling IsLoadingAssets at IAssetRegistry vtable slot 120
// (typically `add rcx, OFFSET; jmp impl`).
static bool findArStateLoadFn() {
    if (s_arImplSerializeFn) return true;
    if (!s_gm.base) return false;

    // Early-exit override: HYDRO_AR_SERIALIZE_OVERRIDE_RVA=0xHEX bypasses the
    // entire candidate-scoring chain and forces the chosen RVA. Useful when
    // the GUID/SerializeVersion resolver picks the wrong tie-breaker (e.g.
    // unordered_map iteration order in step 2). The override at the bottom
    // of this function only fires AFTER the chain succeeds, which is fragile.
    // Auto-detects this-offset based on whether the picked function is
    // UAR::Serialize (1-arg, no offset) or FAssetRegistryImpl::Serialize
    // (3-arg, offset 0x30). Heuristic: UAR::Serialize starts with
    // `test byte ptr [rdx+0x2B], 1` (IsObjectReferenceCollector check) - if
    // those bytes match, treat as UAR::Serialize.
    {
        char rvaBuf[32] = {};
        DWORD rvaLen = GetEnvironmentVariableA("HYDRO_AR_SERIALIZE_OVERRIDE_RVA",
                                               rvaBuf, sizeof(rvaBuf));
        if (rvaLen > 0 && rvaLen < sizeof(rvaBuf)) {
            uint64_t requestedRva = strtoull(rvaBuf, nullptr, 0);
            uint8_t* requested = s_gm.base + requestedRva;
            if (requested >= s_gm.base && requested < s_gm.base + s_gm.size) {
                // Detect call shape from the prologue. Search the first 32
                // bytes for `F6 42 2B 01` (test byte ptr [rdx+0x2B], 1) which
                // uniquely identifies UAR::Serialize (the public 1-arg API).
                static const uint8_t kIsObjRefColl[4] = { 0xF6, 0x42, 0x2B, 0x01 };
                bool isUar = false;
                // Scan the first 96 bytes - UE 5.6 PGO emits a substantial
                // prologue (stack-cookie save, 7 callee-saved regs, frame
                // pointer setup) before the IsObjectReferenceCollector check.
                // Empirically the `F6 42 2B 01` lives at file offset +0x20
                // of UAR::Serialize on DMG@5.6, past a 32-byte boundary.
                size_t scanLen = (s_gm.base + s_gm.size - requested);
                if (scanLen > 96) scanLen = 96;
                for (size_t i = 0; i + sizeof(kIsObjRefColl) <= scanLen; i++) {
                    if (std::memcmp(requested + i, kIsObjRefColl,
                                    sizeof(kIsObjRefColl)) == 0) {
                        isUar = true;
                        break;
                    }
                }
                s_arImplSerializeFn   = requested;
                s_arGuardedDataOffset = isUar ? 0 : 0x30;
                Hydro::logInfo("EngineAPI: AR.ImplSerialize EARLY-OVERRIDE at exe+0x%llX, "
                               "this-offset=0x%X (UAR::Serialize=%d)",
                               (unsigned long long)requestedRva,
                               s_arGuardedDataOffset, isUar ? 1 : 0);
                return true;
            }
            Hydro::logWarn("EngineAPI: HYDRO_AR_SERIALIZE_OVERRIDE_RVA=0x%llX out of "
                           "module bounds; falling through to scoring",
                           (unsigned long long)requestedRva);
        }
    }

    // Step 1: scan for 16-byte GUID literal
    static const uint8_t kArVersionGuid[16] = {
        0xE7, 0x9E, 0x7F, 0x71,
        0x3A, 0x49, 0xB0, 0xE9,
        0x32, 0x91, 0xB3, 0x88,
        0x07, 0x81, 0x38, 0x1B,
    };
    uint8_t* guidLoc = nullptr;
    for (size_t i = 0; i + 16 <= s_gm.size; i++) {
        if (s_gm.base[i] == kArVersionGuid[0] &&
            std::memcmp(s_gm.base + i, kArVersionGuid, 16) == 0) {
            guidLoc = s_gm.base + i;
            break;
        }
    }
    if (!guidLoc) {
        Hydro::logWarn("EngineAPI: AR.State.Load - GUID literal not found in module");
        return false;
    }
    Hydro::logInfo("EngineAPI: AR.State.Load - GUID literal at exe+0x%zX",
                   (size_t)(guidLoc - s_gm.base));

    // Step 2: RIP-relative refs → resolve each to its containing function.
    // MSVC PGO loads the 16-byte FGuid via SSE (movups/movaps) rather than
    // LEA on UE 5.6 - empirically zero LEA refs but several SSE refs.
    // findAllRipRelativeRefs catches LEA + MOV r64 + SSE-load forms.
    auto refs = findAllRipRelativeRefs(s_gm.base, s_gm.size, guidLoc);
    if (refs.empty()) {
        Hydro::logWarn("EngineAPI: AR.State.Load - 0 RIP-relative refs to GUID");
        return false;
    }
    Hydro::logInfo("EngineAPI: AR.State.Load - %zu RIP-relative refs to GUID",
                   refs.size());
    std::unordered_map<uint8_t*, int> fnRefCounts;
    for (uint8_t* ref : refs) {
        uint8_t* fs = funcStartViaPdata(ref);
        if (fs) fnRefCounts[fs]++;
    }
    if (fnRefCounts.empty()) {
        Hydro::logWarn("EngineAPI: AR.State.Load - no GUID refs resolved to functions");
        return false;
    }
    {
        int idx = 0;
        for (auto& kv : fnRefCounts) {
            Hydro::logInfo("EngineAPI: AR.State.Load - GUID-ref fn[%d] = exe+0x%zX (%d refs)",
                           idx++, (size_t)(kv.first - s_gm.base), kv.second);
        }
    }
    // SerializeVersion has multiple LEA refs (Guid load + comparison);
    // pick the function with the most.
    uint8_t* serializeVersionFn = nullptr;
    int bestCount = 0;
    for (auto& kv : fnRefCounts) {
        if (kv.second > bestCount) {
            bestCount = kv.second;
            serializeVersionFn = kv.first;
        }
    }
    Hydro::logInfo("EngineAPI: AR.State.Load - SerializeVersion at exe+0x%zX "
                   "(%d LEA refs; %zu other LEA-ref functions)",
                   (size_t)(serializeVersionFn - s_gm.base),
                   bestCount, fnRefCounts.size() - 1);

    // Step 3: E8 callers of SerializeVersion → SerializeHeader candidates.
    //
    // On UE 5.6 PGO/LTO shipping, SerializeVersion is typically inlined into
    // every caller - so it has 0 E8 callers, even though the GUID-ref scan
    // resolved it correctly. When that happens, the GUID-ref functions
    // themselves ARE the SerializeHeader bodies (the GUID-load merged into
    // them by inlining). Fall through to using every GUID-ref function as
    // a SerializeHeader candidate so step 4 can still find Save/Load.
    auto serializeHeaderCallers =
        findE8CallersOf(s_gm.base, s_gm.size, serializeVersionFn);
    if (serializeHeaderCallers.empty()) {
        Hydro::logInfo("EngineAPI: AR.State.Load - no E8 callers of SerializeVersion "
                       "(likely PGO-inlined); falling through to GUID-ref functions "
                       "as SerializeHeader candidates");
        for (auto& kv : fnRefCounts) {
            serializeHeaderCallers.insert(kv.first);
        }
    } else {
        Hydro::logInfo("EngineAPI: AR.State.Load - %zu callers of SerializeVersion "
                       "(SerializeHeader candidates)", serializeHeaderCallers.size());
    }

    // Step 4: E8 callers of SerializeHeader → {Save, Load}
    auto loadAndSaveCallers = findE8CallersOfAny(serializeHeaderCallers);
    if (loadAndSaveCallers.empty()) {
        Hydro::logWarn("EngineAPI: AR.State.Load - no E8 callers of SerializeHeader "
                       "(tried %zu candidates)", serializeHeaderCallers.size());
        return false;
    }
    Hydro::logInfo("EngineAPI: AR.State.Load - %zu callers of SerializeHeader "
                   "(Save/Load candidates)", loadAndSaveCallers.size());

    // Step 5: Disambiguate Load by the SPECIFIC pattern that Load emits and
    // Save doesn't:
    //   if (OutVersion != nullptr) *OutVersion = Header.Version;
    // → `test r9, r9; jz X; mov [r9], reg` (or similar). The bare "uses r9"
    // signal was too noisy on UE 5.6 - 3 of 5 candidates used r9, including
    // chained continuation regions whose r9 use isn't a Load tell.
    //
    // Score each candidate by:
    //   +5  test/cmp r9, r9
    //   +5  jz/jnz immediately after a test r9 r9
    //   +3  mov [r9+disp], reg  (writing through r9)
    //   +1  any other r9 use
    //   -10 fnSize > 1500 bytes (Load is ~50 lines source → <800 bytes
    //                            compiled; Save and outer wrappers are
    //                            larger)
    ZydisDecoder decoder;
    if (ZYAN_FAILED(ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64,
                                     ZYDIS_STACK_WIDTH_64))) {
        Hydro::logWarn("EngineAPI: AR.State.Load - Zydis init failed");
        return false;
    }
    auto hexLine = [](uint8_t* p, int n) {
        static thread_local char buf[3 * 16 + 1];
        int o = 0;
        for (int i = 0; i < n; i++) {
            o += std::snprintf(buf + o, sizeof(buf) - o, "%02X ", p[i]);
        }
        return std::string(buf, o > 0 ? o - 1 : 0);
    };
    struct ScoredCand {
        uint8_t* entry;
        uint32_t size;
        int      score;
        bool     usesR8AsPointer;
        bool     isUarSerialize;          // [rdx+0x2B] check
        bool     isSaveCtor;               // mov [...], 0x15 early
        // Lazy-init discriminator: early write to [rcx+disp] before any
        // [rdx+disp] read indicates this function lazy-initializes a
        // member field (NOT Serialize). Real FAssetRegistryImpl::Serialize
        // reads from rdx (the FArchive&) within the first ~10 instructions
        // (Ar.IsLoading() / Ar.IsObjectReferenceCollector() bitfield
        // checks compile to `test [rdx+offset], imm` or similar).
        int firstRcxLikeWrite;             // -1 if none in first N inst
        int firstRdxLikeRead;              // -1 if none in first N inst
    };
    // Helpers for the lazy-init discriminator. Track which 64-bit
    // registers currently hold a value derived from rcx (the `this`
    // pointer) or rdx (the FArchive*). MSVC commonly does
    // `mov r12, rcx` and then writes via `[r12+disp]` - that's still a
    // write to "this" for our purposes. Same for rdx → rsi etc.
    auto regBit = [](ZydisRegister r) -> uint64_t {
        ZydisRegister r64 = ZydisRegisterGetLargestEnclosing(
            ZYDIS_MACHINE_MODE_LONG_64, r);
        if (r64 < ZYDIS_REGISTER_RAX || r64 > ZYDIS_REGISTER_R15) return 0;
        return 1ULL << (r64 - ZYDIS_REGISTER_RAX);
    };
    std::vector<ScoredCand> scored;
    for (uint8_t* cand : loadAndSaveCallers) {
        uint8_t* entry = funcStartViaPdata(cand);
        if (!entry) entry = cand;
        uint32_t fnSize = funcSizeViaPdata(entry);
        ScoredCand sc{entry, fnSize, 0, false, false, false, -1, -1};
        int pos = 0;
        constexpr int kProbeBytes = 200;
        constexpr int kDiscrimMaxInst = 30;  // discriminator window (UE 5.6 PGO prologues
                                              // can save 7 callee-saved regs + stack cookie
                                              // before the first [Ar+disp] read; the old 20
                                              // missed the rdx-read on the winning candidate.)
        int instIdx = 0;
        // Initial taint: rcx holds `this`, rdx holds Ar.
        uint64_t thisRegs = regBit(ZYDIS_REGISTER_RCX);
        uint64_t archiveRegs = regBit(ZYDIS_REGISTER_RDX);
        while (pos < kProbeBytes) {
            ZydisDecodedInstruction inst;
            ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
            if (ZYAN_FAILED(ZydisDecoderDecodeFull(&decoder, entry + pos,
                                                   kProbeBytes - pos, &inst, ops))) break;

            // -- Lazy-init discriminator (within first kDiscrimMaxInst) --
            if (instIdx < kDiscrimMaxInst) {
                // Detect first read of [archiveRegs+disp] (= Ar.field) and
                // first write to [thisRegs+disp] (= this->field).
                for (int i = 0; i < inst.operand_count_visible; i++) {
                    auto& op = ops[i];
                    if (op.type == ZYDIS_OPERAND_TYPE_MEMORY) {
                        bool baseIsArchive  = (regBit(op.mem.base) & archiveRegs) != 0;
                        bool baseIsThis     = (regBit(op.mem.base) & thisRegs) != 0;
                        bool isWrite = (op.actions & ZYDIS_OPERAND_ACTION_MASK_WRITE) != 0;
                        bool isRead  = (op.actions & ZYDIS_OPERAND_ACTION_MASK_READ) != 0;
                        if (baseIsArchive && isRead && sc.firstRdxLikeRead < 0) {
                            sc.firstRdxLikeRead = instIdx;
                        }
                        if (baseIsThis && isWrite && sc.firstRcxLikeWrite < 0) {
                            sc.firstRcxLikeWrite = instIdx;
                        }
                    }
                }
                // Update register taint:
                //   `mov reg, archiveReg`  → reg becomes archiveReg
                //   `mov reg, thisReg`     → reg becomes thisReg
                //   any other write to reg → clears taint
                if ((inst.mnemonic == ZYDIS_MNEMONIC_MOV ||
                     inst.mnemonic == ZYDIS_MNEMONIC_LEA) &&
                    inst.operand_count_visible >= 2 &&
                    ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER) {
                    ZydisRegister dst = ops[0].reg.value;
                    uint64_t dstBit = regBit(dst);
                    if (ops[1].type == ZYDIS_OPERAND_TYPE_REGISTER) {
                        uint64_t srcBit = regBit(ops[1].reg.value);
                        if (srcBit & thisRegs)         thisRegs    |= dstBit;
                        else if (srcBit & archiveRegs) archiveRegs |= dstBit;
                        else { thisRegs &= ~dstBit; archiveRegs &= ~dstBit; }
                    } else {
                        // dst written from non-register; clears taint.
                        thisRegs    &= ~dstBit;
                        archiveRegs &= ~dstBit;
                    }
                }
            }
            instIdx++;

            // r8 used as pointer: `mov reg, r8` (save) or `mov [r8+disp], reg`
            // or `mov reg, [r8+disp]` (deref).
            for (int i = 0; i < inst.operand_count_visible; i++) {
                auto& op = ops[i];
                if (op.type == ZYDIS_OPERAND_TYPE_REGISTER &&
                    ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64,
                        op.reg.value) == ZYDIS_REGISTER_R8) {
                    sc.usesR8AsPointer = true;
                } else if (op.type == ZYDIS_OPERAND_TYPE_MEMORY &&
                           (op.mem.base == ZYDIS_REGISTER_R8 ||
                            op.mem.index == ZYDIS_REGISTER_R8)) {
                    sc.usesR8AsPointer = true;
                }
            }
            // test [rdx+0x2B], imm - UAR::Serialize signature
            if (inst.mnemonic == ZYDIS_MNEMONIC_TEST &&
                inst.operand_count_visible >= 2 &&
                ops[0].type == ZYDIS_OPERAND_TYPE_MEMORY &&
                ops[0].mem.base == ZYDIS_REGISTER_RDX &&
                ops[0].mem.disp.value == 0x2B) {
                sc.isUarSerialize = true;
            }
            // mov [reg+disp], 0x15 - Save initializing Header.Version
            if (inst.mnemonic == ZYDIS_MNEMONIC_MOV &&
                inst.operand_count_visible >= 2 &&
                ops[0].type == ZYDIS_OPERAND_TYPE_MEMORY &&
                ops[1].type == ZYDIS_OPERAND_TYPE_IMMEDIATE &&
                ops[1].imm.value.s == 0x15) {
                sc.isSaveCtor = true;
            }
            if (inst.mnemonic == ZYDIS_MNEMONIC_RET) break;
            pos += inst.length;
        }
        // Score:
        //   - Reads Ar (rdx-derived) early in body  → Serialize-shaped, +10
        //   - Writes this (rcx-derived) before any rdx-read → lazy-init, -20
        //   - Uses r8 (FEventContext or 3rd arg)    → +3
        //   - 3-arg shape AND mid-size              → consistent with Serialize
        //   - Has the UAR::Serialize bitfield-test  → -10 (slot 124 wrapper, NOT inner)
        //   - Has Save's Header.Version=21 init     → -10
        sc.score = 0;
        // Lazy-init detector (HIGHEST priority): writes [this+...] BEFORE
        // any [Ar+...] read = NOT Serialize.
        if (sc.firstRcxLikeWrite >= 0 &&
            (sc.firstRdxLikeRead < 0 ||
             sc.firstRcxLikeWrite < sc.firstRdxLikeRead)) {
            sc.score -= 20;
        }
        // Serialize signature: reads Ar (rdx-derived) early. Window widened
        // from 10 → 25 because UE 5.6 PGO emits a long prologue (callee-saved
        // reg spills + stack cookie) before the first [Ar+disp] read on the
        // winning ~2.5KB candidate.
        if (sc.firstRdxLikeRead >= 0 && sc.firstRdxLikeRead < 25) {
            sc.score += 10;
        }
        if (sc.usesR8AsPointer)    sc.score += 3;
        if (sc.isUarSerialize)     sc.score -= 10;
        if (sc.isSaveCtor)         sc.score -= 10;
        // PGO-inlining signal - the real FAssetRegistryImpl::Serialize on
        // UE 5.6 is ~2.5KB because State.Load + State.Save are both inlined
        // into it. The thin slot-124 wrapper (UAR::Serialize) and the
        // continuation chunks are all < 500 bytes.
        if (sc.size > 1500)        sc.score += 5;
        scored.push_back(sc);
    }
    // Log all candidates with bytes for visual debugging.
    std::sort(scored.begin(), scored.end(),
              [](const ScoredCand& a, const ScoredCand& b) { return a.score > b.score; });
    // Refined ImplSerialize disambiguator. Among the 5 SerializeHeader
    // callers on DMG@5.6 (decoded session 2026-05-09):
    //   0x224A350  size=48     UAR::Serialize (slot 124 impl) - has
    //                            ArIsObjectReferenceCollector check
    //   0x223C650  size=36     chained continuation
    //   0x227DEB0  size=425    LoadFromDisk (calls IFileManager::Get)
    //   0x22855D0  size=488    Save (initializes Header.Version=21)
    //   0x396A560  size=2574   FAssetRegistryImpl::Serialize ← we want this
    //                            (huge because State.Load + State.Save are
    //                            both PGO-inlined into it)
    //
    // FAssetRegistryImpl::Serialize takes (this, FArchive&, FEventContext&).
    // r8 is FEventContext. Distinguishing pattern: 3-arg shape (uses r8 as
    // a pointer / saves r8) AND large size (>1500 bytes due to inlining).
    //
    // Score each candidate by:
    //   +5  size > 1500 (inlining signal)
    //   +3  uses r8 as pointer (mov reg, r8 / [r8+disp])
    //   -10 starts with `test [rdx+0x2B], 1` (UAR::Serialize signature)
    //   -5  starts with `mov [rbp+disp], 0x15` early (Save initializing
    //                                                  Header.Version=21)
    //   -5  has IFileManager-shaped first call (LoadFromDisk)
    //   1. Load takes rdx = FArchive&, used as the 2nd arg to
    //      Header.SerializeHeader(Ar) - emitted as `mov rdx, rdx_save` or
    //      direct rdx use in the call setup.
    //   2. LoadFromDisk takes rcx = InPath (wide string), opens a file via
    //      IFileManager::Get(). Its body calls IFileManager::Get() (a
    //      tiny accessor returning a global pointer) - emitted as a near
    //      call early in the prologue.
    //   3. Load constructs FAssetRegistryHeader on stack (8 bytes) and
    //      passes its address. LoadFromDisk doesn't.
    //
    // We can't easily decode (3) without symbols, but (2) helps: count
    // E8 near-calls in first 80 bytes. LoadFromDisk has more (file open
    // path) than Load (one SerializeHeader call). Load typically has
    // 1-2 calls in the first ~60 bytes (Header init optional inline +
    // SerializeHeader); LoadFromDisk has 3+ (IFileManager::Get,
    // CreateFileReader, TUniquePtr ctor).
    auto countE8Calls = [&](uint8_t* entry, int probeBytes) -> int {
        int n = 0;
        int pos = 0;
        while (pos + 5 <= probeBytes) {
            ZydisDecodedInstruction inst;
            ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
            if (ZYAN_FAILED(ZydisDecoderDecodeFull(&decoder, entry + pos,
                                                   probeBytes - pos, &inst, ops))) break;
            if (inst.mnemonic == ZYDIS_MNEMONIC_CALL &&
                (inst.attributes & ZYDIS_ATTRIB_IS_RELATIVE)) {
                n++;
            }
            if (inst.mnemonic == ZYDIS_MNEMONIC_RET) break;
            pos += inst.length;
        }
        return n;
    };
    Hydro::logInfo("EngineAPI: AR.State.Load - candidate scoring (top first):");
    for (auto& sc : scored) {
        Hydro::logInfo("EngineAPI:   exe+0x%zX (size=%u) score=%d "
                       "[usesR8=%d isUAR=%d isSaveCtor=%d "
                       "firstRcxWrite@%d firstRdxRead@%d]",
                       (size_t)(sc.entry - s_gm.base), sc.size, sc.score,
                       sc.usesR8AsPointer ? 1 : 0,
                       sc.isUarSerialize ? 1 : 0,
                       sc.isSaveCtor ? 1 : 0,
                       sc.firstRcxLikeWrite,
                       sc.firstRdxLikeRead);
        if (sc.entry + 32 <= s_gm.base + s_gm.size) {
            Hydro::logInfo("EngineAPI:     [+00..0F]: %s", hexLine(sc.entry, 16).c_str());
            Hydro::logInfo("EngineAPI:     [+10..1F]: %s", hexLine(sc.entry + 16, 16).c_str());
        }
    }
    // Manual override: HYDRO_AR_SERIALIZE_OVERRIDE_RVA=0xHEX picks a
    // specific candidate by its RVA. Useful when iterating on the
    // discriminator without rebuilding. The RVA must match one of the
    // candidates exactly; otherwise the override is rejected.
    {
        char rvaBuf[32] = {};
        DWORD rvaLen = GetEnvironmentVariableA("HYDRO_AR_SERIALIZE_OVERRIDE_RVA",
                                               rvaBuf, sizeof(rvaBuf));
        if (rvaLen > 0 && rvaLen < sizeof(rvaBuf)) {
            uint64_t requestedRva = strtoull(rvaBuf, nullptr, 0);
            uint8_t* requested = s_gm.base + requestedRva;
            for (auto& sc : scored) {
                if (sc.entry == requested) {
                    Hydro::logInfo("EngineAPI: AR.State.Load - OVERRIDE_RVA=0x%llX "
                                   "matches candidate; forcing pick",
                                   (unsigned long long)requestedRva);
                    s_arImplSerializeFn = sc.entry;
                    // Auto-detect call shape: UAR::Serialize takes `this = UAR*`
                    // (no GuardedData offset). The inlined-inner FAssetRegistry-
                    // Impl::Serialize takes `this = &GuardedData` (= s_assetRegImpl + 0x30).
                    s_arGuardedDataOffset = sc.isUarSerialize ? 0 : 0x30;
                    Hydro::logInfo("EngineAPI: AR.ImplSerialize OVERRIDE-resolved at "
                                   "exe+0x%zX, this-offset=0x%X (UAR::Serialize=%d)",
                                   (size_t)(sc.entry - s_gm.base),
                                   s_arGuardedDataOffset,
                                   sc.isUarSerialize ? 1 : 0);
                    return true;
                }
            }
            Hydro::logWarn("EngineAPI: AR.State.Load - OVERRIDE_RVA=0x%llX does "
                           "NOT match any candidate; falling back to scoring",
                           (unsigned long long)requestedRva);
        }
    }
    // Require: positive score AND a clear gap over the runner-up. The
    // firstRdxLikeRead constraint is already in the +10 bonus condition;
    // double-using it as a hard reject double-penalizes PGO-emit shapes.
    // Gap-based threshold mirrors the legacy heuristic resolver's pattern.
    const int kMinScoreGap = 5;
    if (scored.empty() || scored[0].score <= 0) {
        Hydro::logWarn("EngineAPI: AR.State.Load - no positive-score candidate "
                       "matches FAssetRegistryImpl::Serialize shape");
        return false;
    }
    if (scored.size() >= 2 && scored[0].score - scored[1].score < kMinScoreGap) {
        Hydro::logWarn("EngineAPI: AR.State.Load - top candidate score %d only "
                       "%d ahead of runner-up (need >= %d); ambiguous, refusing",
                       scored[0].score,
                       scored[0].score - scored[1].score, kMinScoreGap);
        return false;
    }
    uint8_t* loadFn = scored[0].entry;

    // Step 6: State offset. Default 0x30 (high-confidence guess from
    // layout reasoning). Try to confirm by disassembling IsLoadingAssets
    // at IAssetRegistry vtable slot 120 - typical body:
    //   add rcx, GuardedData_offset
    //   jmp FAssetRegistryImpl::IsLoadingAssets
    uint16_t stateOffset = 0x30;
    if (s_assetRegImpl) {
        void* vt = nullptr;
        if (safeReadPtr((uint8_t*)s_assetRegImpl + 0x28, &vt) && vt) {
            void** arVtable = (void**)vt;
            constexpr int kIsLoadingAssetsSlot = 120;  // user #119 + 2 injected
            void* fn = nullptr;
            if (safeReadPtr((uint8_t*)&arVtable[kIsLoadingAssetsSlot], &fn) && fn) {
                uint8_t* fnPtr = (uint8_t*)fn;
                if (fnPtr >= s_gm.base && fnPtr < s_gm.base + s_gm.size) {
                    uint8_t* entry = funcStartViaPdata(fnPtr);
                    if (!entry) entry = fnPtr;
                    if (entry + 12 <= s_gm.base + s_gm.size) {
                        Hydro::logInfo("EngineAPI: AR.State.Load - IsLoadingAssets at "
                                       "exe+0x%zX, first 12 bytes: "
                                       "%02X %02X %02X %02X %02X %02X %02X %02X "
                                       "%02X %02X %02X %02X",
                                       (size_t)(entry - s_gm.base),
                                       entry[0], entry[1], entry[2], entry[3],
                                       entry[4], entry[5], entry[6], entry[7],
                                       entry[8], entry[9], entry[10], entry[11]);
                        // Scan first 12 bytes for `48 83 C1 imm8` (add rcx, imm8)
                        // or `48 81 C1 imm32` (add rcx, imm32).
                        for (int i = 0; i < 9; i++) {
                            if (entry[i] == 0x48 && entry[i+1] == 0x83 &&
                                entry[i+2] == 0xC1) {
                                stateOffset = entry[i+3];
                                Hydro::logInfo("EngineAPI: AR.State.Load - decoded "
                                               "GuardedData offset = 0x%X (imm8)",
                                               stateOffset);
                                break;
                            }
                            if (i + 6 < 12 && entry[i] == 0x48 && entry[i+1] == 0x81 &&
                                entry[i+2] == 0xC1) {
                                uint32_t imm32 = *(uint32_t*)(entry + i + 3);
                                stateOffset = (uint16_t)imm32;
                                Hydro::logInfo("EngineAPI: AR.State.Load - decoded "
                                               "GuardedData offset = 0x%X (imm32)",
                                               stateOffset);
                                break;
                            }
                        }
                    }
                }
            }
        }
    }
    s_arGuardedDataOffset = stateOffset;

    s_arImplSerializeFn = loadFn;
    Hydro::logInfo("EngineAPI: AR.ImplSerialize resolved at exe+0x%zX, "
                   "GuardedData at obj+0x%X",
                   (size_t)(loadFn - s_gm.base), s_arGuardedDataOffset);

    // Print Zydis-decoded first 50 instructions of the picked function so
    // we can verify by eye whether it's actually FAssetRegistryImpl::Serialize.
    // Source body:
    //   check(!Ar.IsObjectReferenceCollector());
    //   if (Ar.IsLoading()) { State.Load(Ar); CachePathsFromState(...); UpdatePersistentMountPoints(); }
    //   else if (Ar.IsSaving()) { State.Save(Ar, SerializationOptions); }
    // We expect to see early reads of [rdx+...] (FArchive bitfields) for
    // the IsLoading/IsSaving checks. If instead we see writes to [rcx+...]
    // or weird control flow, the picked function is wrong.
    {
        ZydisFormatter formatter;
        ZydisFormatterInit(&formatter, ZYDIS_FORMATTER_STYLE_INTEL);
        Hydro::logInfo("EngineAPI: AR.ImplSerialize picked function decoded "
                       "first ~50 instructions:");
        int instCount = 0;
        constexpr int kMaxInsts = 50;
        constexpr int kMaxBytes = 600;
        int pos = 0;
        while (instCount < kMaxInsts && pos < kMaxBytes) {
            ZydisDecodedInstruction inst;
            ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
            if (ZYAN_FAILED(ZydisDecoderDecodeFull(&decoder, loadFn + pos,
                                                   kMaxBytes - pos, &inst, ops))) break;
            char buf[256];
            ZydisFormatterFormatInstruction(&formatter, &inst, ops,
                inst.operand_count_visible, buf, sizeof(buf),
                (ZyanU64)(loadFn + pos), nullptr);
            Hydro::logInfo("EngineAPI:   [+0x%03X] %s", pos, buf);
            instCount++;
            if (inst.mnemonic == ZYDIS_MNEMONIC_RET) break;
            pos += inst.length;
        }
    }
    return true;
}

// FAssetRegistryImpl::Serialize signature: (this, FArchive&, FEventContext&).
// Returns void; we don't observe its return value - success is measured by
// shim counters incrementing (proof the function read the archive).
using ArImplSerializeFnSig = void(__fastcall*)(void* this_, void* archive,
                                               void* eventContext);

static bool sehCallArImplSerialize(void* this_, void* fn, void* archive,
                                   void* eventContext) {
#ifdef _WIN32
    s_lastArSerializeFaultAddr = 0;
    s_lastArSerializeFaultCode = 0;
    __try {
        ((ArImplSerializeFnSig)fn)(this_, archive, eventContext);
        return true;
    } __except(sehArSerializeFilter(GetExceptionInformation())) { return false; }
#else
    ((ArImplSerializeFnSig)fn)(this_, archive, eventContext);
    return true;
#endif
}

// -- FBufferReader resolver ------------------------------------------------
//
// Instead of a hand-rolled FArchive shim (FArchiveLoader), discover and call
// Epic's own FBufferReader constructor. A real engine archive has every
// invariant satisfied by definition - vtable, FFastPathLoadBuffer, all
// bitfields, CustomVersionContainer, GetCustomVersions, every virtual slot
// in the right order. We just allocate sizeof(FBufferReader) bytes and call
// the discovered ctor; Epic's code does the rest.
//
// Layout (UE 5.6 shipping, sizeof = 0xB0 = 176 bytes):
//   [+0x00] vtable                       (FBufferReader's, set by ctor)
//   [+0x08..+0x8F] FArchive base (FFastPathLoadBuffer + bitfields + ...)
//   [+0x90] ReaderData      (void*)
//   [+0x98] ReaderPos       (int64)
//   [+0xA0] ReaderSize      (int64)
//   [+0xA8] bFreeOnClose    (uint8)
//
// Ctor signature:
//   FBufferReader(void* Data, int64 Size, bool bFreeOnClose, bool bIsPersistent)
// Win64 ABI: rcx=this, rdx=Data, r8=Size, r9b=bFreeOnClose, [rsp+0x20]=bIsPersistent.
//
// Discovery: anchor on the literal wide-string "FBufferReader" returned by
// FBufferReader::GetArchiveName(). LEA-ref → the GetArchiveName function.
// Its absolute address appears as a qword in FBufferReader's vtable (slot 3).
// LEA-refs to the vtable in .text identify the ctor + dtor + (maybe) copy ctor;
// the ctor uniquely contains `mov [rcx+0xA0], r8` (writes ReaderSize from arg2).
void* s_fbufferReaderCtor   = nullptr;  // FBufferReader::FBufferReader (out-of-line on UE 5.6)
void* s_fbufferReaderVtable = nullptr;  // FBufferReader vtable base (in .rdata)
size_t s_fbufferReaderSize  = 0xB0;     // sizeof - verified at discovery time

static bool findFBufferReader() {
    if (s_fbufferReaderCtor && s_fbufferReaderVtable) return true;
    if (!s_gm.base) return false;

    // Step 1: anchor on the wide string "FBufferReader". The linker may emit
    // MULTIPLE copies in .rdata (one per .obj that compiled GetArchiveName) -
    // FBufferReader and its base FBufferReaderBase both return "FBufferReader",
    // and the linker doesn't always merge the literals. Find ALL occurrences.
    static const wchar_t kStr[] = L"FBufferReader";
    const size_t kStrBytes = sizeof(kStr) - sizeof(wchar_t);  // exclude null
    std::vector<uint8_t*> stringLocs;
    for (size_t i = 0; i + kStrBytes <= s_gm.size; i++) {
        if (std::memcmp(s_gm.base + i, kStr, kStrBytes) == 0) {
            // Skip if this is part of a longer string (e.g. "FBufferReaderBase").
            // Check the wchar AFTER the literal - must be either null or a
            // non-letter (linker pads strings with null terminators).
            if (i + kStrBytes + 2 <= s_gm.size) {
                wchar_t next = *(const wchar_t*)(s_gm.base + i + kStrBytes);
                if ((next >= L'A' && next <= L'Z') ||
                    (next >= L'a' && next <= L'z') ||
                    (next >= L'0' && next <= L'9')) {
                    continue;  // longer name like FBufferReaderBase
                }
            }
            stringLocs.push_back(s_gm.base + i);
        }
    }
    if (stringLocs.empty()) {
        Hydro::logWarn("EngineAPI: FBufferReader - string anchor not found");
        return false;
    }
    Hydro::logInfo("EngineAPI: FBufferReader - found %zu copies of string in .rdata",
                   stringLocs.size());

    // Step 2: LEA-refs. GetArchiveName loads the literal into rax via
    // `lea rax, [rip+disp]`. Each string copy is referenced by exactly one
    // GetArchiveName; collect all unique containing functions.
    std::unordered_set<uint8_t*> getArchiveNameFns;
    for (uint8_t* sLoc : stringLocs) {
        auto leas = findAllLeaRefs(s_gm.base, s_gm.size, sLoc);
        for (uint8_t* lea : leas) {
            uint8_t* fs = funcStartViaPdata(lea);
            if (fs) getArchiveNameFns.insert(fs);
        }
    }
    if (getArchiveNameFns.empty()) {
        Hydro::logWarn("EngineAPI: FBufferReader - no .pdata-resolved GetArchiveName candidates");
        return false;
    }
    Hydro::logInfo("EngineAPI: FBufferReader - %zu candidate GetArchiveName function(s)",
                   getArchiveNameFns.size());

    // Step 3 + 4: for EACH GetArchiveName candidate, find its containing
    // vtable and scan LEA-refs for the ctor's `mov [rcx+0xA0], r8` (ReaderSize
    // store from arg2 = r8). Whichever vtable yields a matching function is
    // FBufferReader's (FBufferReaderBase has different field layout and
    // doesn't store at [rcx+0xA0]).
    //
    // ReaderSize-store pattern: any 64-bit `mov [reg+0xA0], reg` with mod=10
    // disp32 = 0x000000A0. MSVC commonly saves `this` to a callee-saved
    // register (e.g. r14) early in the function, then writes via that
    // register. Real encoding observed at FBufferReader::FBufferReader on
    // UE 5.6 DMG: `mov [r14+0xA0], rdi` = 49 89 BE A0 00 00 00, where r14
    // holds `this` (was rcx) and rdi holds Size (was r8).
    auto isReaderSizeStore = [](const uint8_t* p) -> bool {
        // REX prefix with W=1 (64-bit op): 0x48, 0x49, 0x4C, 0x4D
        if ((p[0] & 0xF8) != 0x48) return false;  // 0100 1xxx
        if ((p[0] & 0x08) == 0)    return false;  // W bit must be set
        if (p[1] != 0x89)          return false;  // MOV r/m64, r64
        uint8_t modrm = p[2];
        if ((modrm & 0xC0) != 0x80) return false; // mod=10 (disp32)
        uint8_t rm = modrm & 0x07;
        if (rm == 4) return false;  // SIB byte form - not the simple case
        if (rm == 5) return false;  // RIP-relative - not a stack write
        // disp32 must equal 0xA0
        return *(uint32_t*)(p + 3) == 0xA0;
    };

    uint8_t* winningVtable = nullptr;
    uint8_t* winningCtor   = nullptr;
    for (uint8_t* gan : getArchiveNameFns) {
        // Find the vtable that contains this GetArchiveName. Scan 8-byte-
        // aligned qwords in the module; at most ~1 hit for each gan.
        uint64_t ganVA = (uint64_t)gan;
        uint8_t* gansSlot = nullptr;
        for (size_t i = 0; i + 8 <= s_gm.size; i += 8) {
            if (*(uint64_t*)(s_gm.base + i) == ganVA) {
                gansSlot = s_gm.base + i;
                break;
            }
        }
        if (!gansSlot) continue;
        // GetArchiveName is at FArchive vtable slot 3 (offset 0x18). Walk back.
        uint8_t* vtableBase = gansSlot - 0x18;
        Hydro::logInfo("EngineAPI: FBufferReader -   trying gan exe+0x%zX → vtable exe+0x%zX",
                       (size_t)(gan - s_gm.base),
                       (size_t)(vtableBase - s_gm.base));

        auto vtLeas = findAllLeaRefs(s_gm.base, s_gm.size, vtableBase);
        if (vtLeas.empty()) continue;
        // Look for any LEA-to-vtable function that contains the ReaderSize
        // store pattern within its first ~0x180 bytes.
        for (uint8_t* lea : vtLeas) {
            uint8_t* fs = funcStartViaPdata(lea);
            if (!fs) continue;
            size_t maxScan = (std::min)((size_t)0x180,
                                        (size_t)(s_gm.base + s_gm.size - fs));
            for (size_t i = 0; i + 7 <= maxScan; i++) {
                if (isReaderSizeStore(fs + i)) {
                    winningVtable = vtableBase;
                    winningCtor   = fs;
                    break;
                }
            }
            if (winningCtor) break;
        }
        if (winningCtor) break;
    }
    if (!winningCtor) {
        Hydro::logWarn("EngineAPI: FBufferReader - no candidate vtable had a LEA-ref "
                       "function containing [rcx+0xA0]=r8 (ReaderSize store); "
                       "discovery failed");
        return false;
    }

    s_fbufferReaderVtable = winningVtable;
    s_fbufferReaderCtor   = winningCtor;
    Hydro::logInfo("EngineAPI: FBufferReader - ctor at exe+0x%zX, vtable at exe+0x%zX",
                   (size_t)((uint8_t*)winningCtor - s_gm.base),
                   (size_t)(winningVtable - s_gm.base));
    return true;
}

// FBufferReader ctor signature:
//   void __fastcall ctor(void* this, void* Data, int64 Size,
//                        uint8 bFreeOnClose, uint8 bIsPersistent)
// 5th arg goes on stack at [rsp+0x20] per Win64 ABI (MSVC handles automatically).
using FBufferReaderCtorSig = void(__fastcall*)(void* self, void* data, int64_t size,
                                               uint8_t bFreeOnClose, uint8_t bIsPersistent);

static DWORD64 s_lastFBRCtorFaultAddr = 0;
static DWORD   s_lastFBRCtorFaultCode = 0;

#ifdef _WIN32
static int sehFBRCtorFilter(EXCEPTION_POINTERS* ep) {
    s_lastFBRCtorFaultAddr = (DWORD64)ep->ExceptionRecord->ExceptionAddress;
    s_lastFBRCtorFaultCode = ep->ExceptionRecord->ExceptionCode;
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

static bool sehCallFBRCtor(void* self, void* fn, void* data, int64_t size,
                           bool bFreeOnClose, bool bIsPersistent) {
#ifdef _WIN32
    s_lastFBRCtorFaultAddr = 0;
    s_lastFBRCtorFaultCode = 0;
    __try {
        ((FBufferReaderCtorSig)fn)(self, data, size,
                                   bFreeOnClose ? 1 : 0,
                                   bIsPersistent ? 1 : 0);
        return true;
    } __except(sehFBRCtorFilter(GetExceptionInformation())) { return false; }
#else
    ((FBufferReaderCtorSig)fn)(self, data, size,
                               bFreeOnClose ? 1 : 0,
                               bIsPersistent ? 1 : 0);
    return true;
#endif
}

// -- AR.AppendState bridge (UE 5.6 shipping mod-load path) -----------------
//
// UAR::Serialize crashes on a populated AR with `Can only load into empty
// asset registry states. Load into temporary and append using
// InitializeFromExisting() instead.` (AssetRegistryState.cpp:2405). UE's own
// fix: load AR.bin into a TEMPORARY FAssetRegistryState, then merge via
// UAR::AppendState which calls State.InitializeFromExisting on the live AR.
//
// LoadFromDisk @ exe+0x227DEB0 (425 bytes, per existing scoring research at
// EngineAPI.cpp:8720). UAR::AppendState: TBD via offline analysis (string
// anchor "FAssetRegistryImpl::CachePathsFromState" → 2-hop E8 climb).
//
// Both RVAs are configurable via env vars to avoid baking into source until
// we have a robust resolver:
//   HYDRO_AR_LOADFROMDISK_RVA   = e.g. 0x227DEB0
//   HYDRO_AR_APPENDSTATE_RVA    = TBD
void* s_loadFromDiskFn   = nullptr;
void* s_uarAppendStateFn = nullptr;

static bool resolveAppendStatePath() {
    if (s_loadFromDiskFn && s_uarAppendStateFn) return true;
    if (!s_gm.base) return false;

    auto resolveEnvRva = [](const char* envVar, void*& outFn, const char* label) {
        char buf[32] = {};
        DWORD len = GetEnvironmentVariableA(envVar, buf, sizeof(buf));
        if (len > 0 && len < sizeof(buf)) {
            uint64_t rva = strtoull(buf, nullptr, 0);
            if (rva && rva < s_gm.size) {
                outFn = s_gm.base + rva;
                Hydro::logInfo("EngineAPI: AppendState path - %s = exe+0x%llX",
                               label, (unsigned long long)rva);
            }
        }
    };
    resolveEnvRva("HYDRO_AR_LOADFROMDISK_RVA", s_loadFromDiskFn,   "LoadFromDisk");
    resolveEnvRva("HYDRO_AR_APPENDSTATE_RVA",  s_uarAppendStateFn, "UAR::AppendState");

    // Re-resolve AppendState via GFP "Mounting" anchor (the canonical
    // LoadFromDisk → AppendState pair in Epic's GameFeaturePluginStateMachine).
    // If the env-var-derived AppendState differs from the GFP-derived one, the
    // env-var value was likely a .pdata CHAININFO mid-function miss.
    if (s_loadFromDiskFn) {
        // Anchor strings (try wide + ANSI). Per UE 5.6 source review:
        //   - "Failed_To_Load_Plugin_AssetRegistry"   (CSV stat tag, GFP Mounting)
        //   - "GFP_Mounting_AR"                       (CPU profiler scope literal)
        //   - "LoadPremadeAssetRegistry"              (SCOPED_BOOT_TIMING tag, AssetRegistry.cpp:945)
        std::vector<uint8_t*> anchorAddrs;
        const wchar_t* wideAnchors[] = {
            L"Failed_To_Load_Plugin_AssetRegistry",
            L"GFP_Mounting_AR",
            L"LoadPremadeAssetRegistry",
        };
        for (const wchar_t* w : wideAnchors) {
            uint8_t* a = findWideString(s_gm.base, s_gm.size, w);
            if (a) {
                Hydro::logInfo("EngineAPI: GFP-anchor (wide) '%ls' @ exe+0x%zX",
                               w, (size_t)(a - s_gm.base));
                anchorAddrs.push_back(a);
            }
        }
        const char* ansiAnchors[] = {
            "Failed_To_Load_Plugin_AssetRegistry",
            "GFP_Mounting_AR",
            "LoadPremadeAssetRegistry",
        };
        for (const char* a : ansiAnchors) {
            size_t alen = std::strlen(a);
            // Naive ANSI search; modules are O(100MB) so still ~ms.
            for (size_t off = 0; off + alen + 1 < s_gm.size; off++) {
                if (s_gm.base[off] != (uint8_t)a[0]) continue;
                if (std::memcmp(s_gm.base + off, a, alen) == 0 &&
                    s_gm.base[off + alen] == 0) {
                    Hydro::logInfo("EngineAPI: GFP-anchor (ANSI) '%s' @ exe+0x%zX",
                                   a, (size_t)off);
                    anchorAddrs.push_back(s_gm.base + off);
                    break;
                }
            }
        }

        if (anchorAddrs.empty()) {
            Hydro::logWarn("EngineAPI: GFP-anchor - no anchor strings found in module");
        } else {
            // Each anchor → all LEA refs → containing fns. Dedupe.
            std::unordered_set<uint8_t*> candidateFns;
            for (uint8_t* sa : anchorAddrs) {
                auto refs = findAllLeaRefs(s_gm.base, s_gm.size, sa);
                for (uint8_t* r : refs) {
                    uint8_t* fn = funcStartViaPdata(r);
                    if (fn) candidateFns.insert(fn);
                }
            }
            Hydro::logInfo("EngineAPI: GFP-anchor - %zu candidate fn(s) contain anchor strings",
                           candidateFns.size());

            // For each candidate fn: enumerate ALL E8 calls and pick the
            // largest target - that's the inner FAssetRegistryImpl::AppendState
            // (InitializeFromExisting + CachePathsFromState body). The public
            // outer (1-arg lock+delegate wrapper) is rarely E8-called from
            // within LoadPremadeAssetRegistry; that function calls the inner
            // 4-arg directly because it's a member of the same class.
            //
            // LoadPremadeAssetRegistry calls (per UE 5.6 source):
            //   - LoadFileToArray  (FFileHelper) - small/medium
            //   - State.Load(FArchive&)          - large (deserializer body or wrapper of it)
            //   - AppendState(EventContext, ...) - large (inner, 4-arg)
            //
            // The two largest E8 targets are Load and AppendState. We
            // discriminate by signature observation: AppendState is the LAST
            // significant E8 in the function before cleanup; Load is earlier.
            // Track all E8 targets, sort by file order, pick the second-large.
            void* gfpAppendState = nullptr;
            for (uint8_t* fn : candidateFns) {
                uint32_t fnSize = funcSizeViaPdata(fn);
                if (!fnSize || fnSize > 0x4000) continue;
                Hydro::logInfo("EngineAPI: GFP-anchor - fn exe+0x%zX size=%u",
                               (size_t)(fn - s_gm.base), fnSize);

                // Collect all E8 targets in this fn. Skip targets within
                // the same function (intra-function calls, rare but exist).
                struct E8Site { size_t off; uint8_t* tgt; uint32_t tgtSize; };
                std::vector<E8Site> sites;
                for (size_t i = 0; i + 5 <= fnSize; i++) {
                    if (fn[i] != 0xE8) continue;
                    int32_t rel = *(int32_t*)(fn + i + 1);
                    uint8_t* tgt = fn + i + 5 + rel;
                    if (tgt < s_gm.base || tgt >= s_gm.base + s_gm.size) continue;
                    if (tgt >= fn && tgt < fn + fnSize) continue; // intra-fn jump
                    uint32_t tgtSize = funcSizeViaPdata(tgt);
                    if (!tgtSize || tgtSize > 0x10000) continue; // skip oddities
                    sites.push_back({i, tgt, tgtSize});
                }
                Hydro::logInfo("EngineAPI: GFP-anchor -   %zu E8 callees in fn:",
                               sites.size());
                for (auto& s : sites) {
                    Hydro::logInfo("EngineAPI: GFP-anchor -     E8 @ +0x%zX → exe+0x%zX (size=%u)",
                                   s.off, (size_t)(s.tgt - s_gm.base), s.tgtSize);
                }
                if (sites.empty()) continue;

                // Heuristic: AppendState is the LARGEST E8 target in
                // LoadPremadeAssetRegistry (its body has the full TMap merge +
                // path-cache rebuild). Load(FArchive&) is also large but
                // typically smaller than AppendState's combined work. Pick
                // largest; LATEST occurrence in fn if tie. This is the inner
                // FAssetRegistryImpl::AppendState - the actual merge fn.
                E8Site best = sites[0];
                for (auto& s : sites) {
                    if (s.tgtSize > best.tgtSize ||
                        (s.tgtSize == best.tgtSize && s.off > best.off)) {
                        best = s;
                    }
                }
                Hydro::logInfo("EngineAPI: GFP-anchor -   picked largest E8 @ +0x%zX → exe+0x%zX (size=%u)",
                               best.off, (size_t)(best.tgt - s_gm.base), best.tgtSize);
                gfpAppendState = best.tgt;
                if (gfpAppendState) break;
            }

            if (gfpAppendState) {
                size_t gfpRva = (size_t)((uint8_t*)gfpAppendState - s_gm.base);
                size_t curRva = s_uarAppendStateFn
                    ? (size_t)((uint8_t*)s_uarAppendStateFn - s_gm.base) : 0;
                if (s_uarAppendStateFn && gfpAppendState != s_uarAppendStateFn) {
                    Hydro::logWarn("EngineAPI: GFP-anchor AppendState exe+0x%zX DIFFERS "
                                   "from env-var exe+0x%zX - overriding (likely CHAININFO miss)",
                                   gfpRva, curRva);
                    s_uarAppendStateFn = gfpAppendState;
                } else if (s_uarAppendStateFn) {
                    Hydro::logInfo("EngineAPI: GFP-anchor AppendState exe+0x%zX MATCHES "
                                   "env-var - resolver was correct, AV is elsewhere",
                                   gfpRva);
                } else {
                    s_uarAppendStateFn = gfpAppendState;
                    Hydro::logInfo("EngineAPI: GFP-anchor AppendState exe+0x%zX (no env-var)",
                                   gfpRva);
                }
            } else {
                Hydro::logWarn("EngineAPI: GFP-anchor - couldn't disambiguate AppendState target");
            }
        }
    }

    // EXPLORATORY: dump E8 callers of s_uarAppendStateFn. One of them is
    // FAssetRegistryImpl::LoadPremadeAssetRegistry or similar - and its
    // `this` argument is the LIVE FAssetRegistryImpl singleton. Next session
    // we'll trace how it gets `this` (global, passed-through, etc.) to find
    // the runtime AR pointer for the bridge.
    if (s_uarAppendStateFn) {
        auto callers = findE8CallersOf(s_gm.base, s_gm.size, (uint8_t*)s_uarAppendStateFn);
        Hydro::logInfo("EngineAPI: AppendState callers: %zu E8 caller fn(s)", callers.size());
        size_t i = 0;
        for (auto* c : callers) {
            uint32_t sz = funcSizeViaPdata(c);
            Hydro::logInfo("EngineAPI:   caller[%zu] = exe+0x%zX size=%u",
                           i++, (size_t)(c - s_gm.base), sz);
            if (i >= 12) break;
        }
    }
    // EXPLORATORY: find wide "AssetRegistry" - the FName module name string.
    // LEA refs to it land in functions involved with FModuleManager-keyed
    // module lookup. One of those callers is FAssetRegistryModule's load /
    // get path; from there we can locate the FAssetRegistryImpl pointer.
    {
        uint8_t* arStr = findWideString(s_gm.base, s_gm.size, L"AssetRegistry");
        if (arStr) {
            auto refs = findAllLeaRefs(s_gm.base, s_gm.size, arStr);
            Hydro::logInfo("EngineAPI: wide \"AssetRegistry\" string @ exe+0x%zX, %zu LEA-ref(s)",
                           (size_t)(arStr - s_gm.base), refs.size());
            std::unordered_set<uint8_t*> uniqueFns;
            for (auto* r : refs) {
                uint8_t* fn = funcStartViaPdata(r);
                if (fn) uniqueFns.insert(fn);
            }
            Hydro::logInfo("EngineAPI:   %zu unique containing function(s)", uniqueFns.size());
            size_t cnt = 0;
            for (auto* fn : uniqueFns) {
                uint32_t sz = funcSizeViaPdata(fn);
                Hydro::logInfo("EngineAPI:     fn[%zu] = exe+0x%zX size=%u",
                               cnt++, (size_t)(fn - s_gm.base), sz);
                if (cnt >= 20) break;
            }
        }
    }

    // Log final s_uarAppendStateFn size + first 16 bytes - outer (public 1-arg
    // wrapper) is thin (~40-200 B, mostly lock + tail-call into inner). Inner
    // (4-arg FAssetRegistryImpl::AppendState body) is hundreds of B.
    if (s_uarAppendStateFn) {
        uint8_t* fn = (uint8_t*)s_uarAppendStateFn;
        uint32_t fnSize = funcSizeViaPdata(fn);
        Hydro::logInfo("EngineAPI: AppendState final s_uarAppendStateFn=exe+0x%zX size=%u %s",
                       (size_t)(fn - s_gm.base), fnSize,
                       fnSize <= 200 ? "(THIN - likely outer 1-arg public)" :
                       fnSize <= 1500 ? "(MEDIUM - could be either)" :
                                        "(HEAVY - likely inner 4-arg merge body)");
        Hydro::logInfo("EngineAPI: AppendState first 16 bytes: "
                       "%02X %02X %02X %02X %02X %02X %02X %02X "
                       "%02X %02X %02X %02X %02X %02X %02X %02X",
                       fn[0],fn[1],fn[2],fn[3],fn[4],fn[5],fn[6],fn[7],
                       fn[8],fn[9],fn[10],fn[11],fn[12],fn[13],fn[14],fn[15]);
    }

    // VTABLE CROSS-CHECK. Read s_assetRegImpl->vtable[N] for N=0..150 and
    // log entries that match s_uarAppendStateFn. The match's slot index is
    // `IAssetRegistry::AppendState`'s vtable position. If exactly one match
    // → resolver IS what live AR calls. If 0 matches → resolver landed on a
    // function that ISN'T in this AR's vtable → wrong AR instance OR PGO/LTO
    // outlined the body and our resolver caught the body, not the slot entry.
    if (s_assetRegImpl && s_uarAppendStateFn) {
        void** vtable = nullptr;
        std::memcpy(&vtable, s_assetRegImpl, sizeof(vtable));
        if (vtable) {
            MEMORY_BASIC_INFORMATION vmbi = {};
            size_t maxSlots = 150;
            if (VirtualQuery(vtable, &vmbi, sizeof(vmbi)) &&
                (vmbi.State & MEM_COMMIT)) {
                size_t avail = ((uintptr_t)vmbi.BaseAddress + vmbi.RegionSize) -
                               (uintptr_t)vtable;
                size_t slotsAvail = avail / sizeof(void*);
                if (slotsAvail < maxSlots) maxSlots = slotsAvail;
            }
            int matches = 0;
            int firstMatchSlot = -1;
            for (size_t s = 0; s < maxSlots; s++) {
                void* slot = vtable[s];
                if (!slot) continue;
                if (slot == s_uarAppendStateFn) {
                    matches++;
                    if (firstMatchSlot < 0) firstMatchSlot = (int)s;
                    Hydro::logInfo("EngineAPI: vtable[%zu] = exe+0x%zX (== s_uarAppendStateFn)",
                                   s, (size_t)((uint8_t*)slot - s_gm.base));
                }
            }
            Hydro::logInfo("EngineAPI: vtable cross-check: scanned %zu slots, %d match(es) for "
                           "s_uarAppendStateFn=exe+0x%zX (first match slot=%d)",
                           maxSlots, matches,
                           (size_t)((uint8_t*)s_uarAppendStateFn - s_gm.base),
                           firstMatchSlot);
            if (matches == 0) {
                // Dump first 30 vtable entries with sizes so we can spot
                // what AppendState's slot probably is by size matching.
                Hydro::logWarn("EngineAPI: NO match in live AR's vtable - dumping first 30 "
                               "slots with .pdata sizes to identify candidates:");
                for (size_t s = 0; s < 30 && s < maxSlots; s++) {
                    void* slot = vtable[s];
                    if (!slot) {
                        Hydro::logInfo("EngineAPI:   vtable[%zu] = NULL", s);
                        continue;
                    }
                    if ((uint8_t*)slot < s_gm.base ||
                        (uint8_t*)slot >= s_gm.base + s_gm.size) {
                        Hydro::logInfo("EngineAPI:   vtable[%zu] = %p (out-of-module)",
                                       s, slot);
                        continue;
                    }
                    uint32_t sSize = funcSizeViaPdata((uint8_t*)slot);
                    Hydro::logInfo("EngineAPI:   vtable[%zu] = exe+0x%zX size=%u",
                                   s, (size_t)((uint8_t*)slot - s_gm.base), sSize);
                }
            }
        } else {
            Hydro::logWarn("EngineAPI: vtable cross-check - s_assetRegImpl[0] (vtable) is null");
        }
    }

    return s_loadFromDiskFn != nullptr && s_uarAppendStateFn != nullptr;
}

// LoadFromDisk signature:
//   static bool FAssetRegistryState::LoadFromDisk(
//       const TCHAR* InPath,                          // rcx
//       const FAssetRegistryLoadOptions& InOptions,   // rdx (pointer to struct)
//       FAssetRegistryState& OutState,                // r8  (pointer to struct)
//       FAssetRegistryVersion::Type* OutVersion);     // r9  (nullable)
using LoadFromDiskFn = bool(__fastcall*)(const wchar_t* path,
                                         const void* options,
                                         void* outState,
                                         void* outVersion);

// UAR::AppendState signature:
//   void UAssetRegistryImpl::AppendState(const FAssetRegistryState& InState);
// Win64: rcx = UAR* (this), rdx = &InState.
using AppendStateFn = void(__fastcall*)(void* uar, const void* inState);

static DWORD64 s_lastAppendFaultAddr = 0;
static DWORD   s_lastAppendFaultCode = 0;
// Capture full register state at fault - to read r9 (TMap base?) and rcx
// (key/index?) and decode "which TMap is being walked, why is rcx out of
// range". Win64 ABI: rcx,rdx,r8,r9 = first 4 args; rax,r8,r9,r10,r11 are
// volatile (used as scratch); rbx,rsi,rdi,r12-r15 are non-volatile.
static DWORD64 s_lastAppendFaultRegs[16] = {}; // rax,rcx,rdx,rbx,rsp,rbp,rsi,rdi,r8..r15

#ifdef _WIN32
static int sehAppendFilter(EXCEPTION_POINTERS* ep) {
    if (ep && ep->ExceptionRecord) {
        s_lastAppendFaultAddr = (DWORD64)ep->ExceptionRecord->ExceptionAddress;
        s_lastAppendFaultCode = ep->ExceptionRecord->ExceptionCode;
    }
    if (ep && ep->ContextRecord) {
        const CONTEXT* c = ep->ContextRecord;
        s_lastAppendFaultRegs[0]  = c->Rax;
        s_lastAppendFaultRegs[1]  = c->Rcx;
        s_lastAppendFaultRegs[2]  = c->Rdx;
        s_lastAppendFaultRegs[3]  = c->Rbx;
        s_lastAppendFaultRegs[4]  = c->Rsp;
        s_lastAppendFaultRegs[5]  = c->Rbp;
        s_lastAppendFaultRegs[6]  = c->Rsi;
        s_lastAppendFaultRegs[7]  = c->Rdi;
        s_lastAppendFaultRegs[8]  = c->R8;
        s_lastAppendFaultRegs[9]  = c->R9;
        s_lastAppendFaultRegs[10] = c->R10;
        s_lastAppendFaultRegs[11] = c->R11;
        s_lastAppendFaultRegs[12] = c->R12;
        s_lastAppendFaultRegs[13] = c->R13;
        s_lastAppendFaultRegs[14] = c->R14;
        s_lastAppendFaultRegs[15] = c->R15;
    }
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

static bool sehCallLoadFromDisk(void* fn, const wchar_t* path,
                                const void* options, void* outState,
                                void* outVersion) {
#ifdef _WIN32
    s_lastAppendFaultAddr = 0;
    s_lastAppendFaultCode = 0;
    __try {
        return ((LoadFromDiskFn)fn)(path, options, outState, outVersion);
    } __except(sehAppendFilter(GetExceptionInformation())) { return false; }
#else
    return ((LoadFromDiskFn)fn)(path, options, outState, outVersion);
#endif
}

static bool sehCallAppendState(void* fn, void* uar, const void* inState) {
#ifdef _WIN32
    s_lastAppendFaultAddr = 0;
    s_lastAppendFaultCode = 0;
    __try {
        ((AppendStateFn)fn)(uar, inState);
        return true;
    } __except(sehAppendFilter(GetExceptionInformation())) { return false; }
#else
    ((AppendStateFn)fn)(uar, inState);
    return true;
#endif
}

// FAssetRegistryLoadOptions layout (8 bytes):
//   uint8 bLoadDependencies (default true)
//   uint8 bLoadPackageData  (default true)
//   uint16 padding
//   int32  ParallelWorkers  (default 0)
//
// 2026-05-09: stripped both flags to 0 to test whether dep/package data is
// what's making AppendState's InitializeFromExisting AV at exe+0x221218E.
// FName indices in the dep graph may not translate to the live process's
// pool; without them we should pass cleanly. If T1 then passes, we can
// re-enable selectively.
struct HydroLoadOptions {
    uint8_t bLoadDependencies = 0;
    uint8_t bLoadPackageData  = 0;
    uint8_t pad[2]            = {};
    int32_t parallelWorkers   = 0;
};

// Try to merge AR.bin via the AppendState path. Returns true on success.
// On failure (env vars not set OR call failed), the caller should fall back
// to the legacy Serialize path. Leaks ~tens of KB per call (the temp state's
// FAssetData heap allocations) until we wire up the dtor.
static bool tryMergeViaAppendState(const wchar_t* arBinPath) {
    if (!resolveAppendStatePath()) return false;
    if (!s_assetRegImpl) return false;

    // Allocate temp FAssetRegistryState. The header math gives ~500-700 bytes,
    // but the structurally-corrupt TMaps observed in the dump at +0x40 (Num=256,
    // Max=0) suggest LoadFromDisk wrote past 0x800 into adjacent static memory.
    // Bump to 0x4000 (16KB) - comfortably oversized for any realistic future
    // sizeof growth, and isolated from neighboring statics.
    //
    // Zero-init: all TArrays/TMaps default to {Data=null, Num=0, Max=0}, ints
    // to 0, FNames to NAME_None idx 0. Matches the inline ctor:
    // `FAssetRegistryState() {}` at AssetRegistryState.h:803.
    alignas(16) static uint8_t tempState[0x4000] = {};
    std::memset(tempState, 0, sizeof(tempState));

    HydroLoadOptions opts;

    Hydro::logInfo("EngineAPI: AppendState path - LoadFromDisk(%ls) → temp state @ %p",
                   arBinPath, tempState);
    bool loadOk = sehCallLoadFromDisk(s_loadFromDiskFn, arBinPath, &opts,
                                      tempState, nullptr);
    if (!loadOk) {
        DWORD64 rva = 0;
        if (s_lastAppendFaultAddr >= (DWORD64)s_gm.base &&
            s_lastAppendFaultAddr <  (DWORD64)s_gm.base + s_gm.size) {
            rva = s_lastAppendFaultAddr - (DWORD64)s_gm.base;
        }
        Hydro::logWarn("EngineAPI: AppendState path - LoadFromDisk SEH-faulted "
                       "code=0x%08X fault@exe+0x%llX raw=0x%llX",
                       s_lastAppendFaultCode,
                       (unsigned long long)rva,
                       (unsigned long long)s_lastAppendFaultAddr);
        return false;
    }
    int32_t numAssets = *(int32_t*)(tempState + 0); // first field varies - log a few candidate offsets
    Hydro::logInfo("EngineAPI: AppendState path - LoadFromDisk OK; tempState[+0x00..0x10]: "
                   "%02X %02X %02X %02X %02X %02X %02X %02X "
                   "%02X %02X %02X %02X %02X %02X %02X %02X",
                   tempState[0], tempState[1], tempState[2], tempState[3],
                   tempState[4], tempState[5], tempState[6], tempState[7],
                   tempState[8], tempState[9], tempState[10], tempState[11],
                   tempState[12], tempState[13], tempState[14], tempState[15]);
    (void)numAssets;

    // FName-divergence diagnostic. Hypothesis: LoadFromDisk allocates a
    // per-state local FNameBatch instead of using the global FName pool, so
    // FName indices stored in tempState's heap arrays are LOCAL (small ints
    // 0..N) - when AppendState's TMap insert path indexes the LIVE AR's
    // hash table with a local index, it AVs. To test, look up known mod
    // strings in the global pool via FName::Add and scan tempState's first
    // heap allocation for those exact uint32 values. If found at plausible
    // FAssetData offsets → indices are global → divergence is NOT the bug.
    // If not found → strong evidence of pool divergence.
    {
        void* heap = nullptr;
        std::memcpy(&heap, tempState + 0, sizeof(heap));
        const wchar_t* probes[] = {
            L"/Game/Mods/HudTemplate/Sphere",
            L"/Game/Mods/HudTemplate/Red",
            L"/Game/Mods/HudTemplate/WBP_HydroOverlay",
            L"Sphere_C",
        };
        constexpr size_t kNumProbes = sizeof(probes) / sizeof(probes[0]);
        uint32_t globalIdx[kNumProbes] = {};
        for (size_t i = 0; i < kNumProbes; i++) {
            FName8 fn = {};
            if (safeConstructFName(&fn, probes[i])) {
                globalIdx[i] = fn.comparisonIndex;
                Hydro::logInfo("EngineAPI:   FName-probe '%ls' → global idx=%u",
                               probes[i], globalIdx[i]);
            } else {
                Hydro::logWarn("EngineAPI:   FName-probe '%ls' → ctor failed",
                               probes[i]);
            }
        }
        // Bound heap read with VirtualQuery so we don't fault scanning past
        // the allocation. 200 FAssetData entries × ~100 B ≈ 20 KB; cap at 64 KB.
        size_t scanCap = 64 * 1024;
        MEMORY_BASIC_INFORMATION mbi = {};
        if (heap && VirtualQuery(heap, &mbi, sizeof(mbi)) &&
            (mbi.State & MEM_COMMIT) &&
            (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ |
                            PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY |
                            PAGE_EXECUTE_WRITECOPY))) {
            size_t avail = (size_t)((uintptr_t)mbi.BaseAddress + mbi.RegionSize -
                                     (uintptr_t)heap);
            if (avail < scanCap) scanCap = avail;
            Hydro::logInfo("EngineAPI:   tempState heap @ %p (region %zu B); first 128 B:",
                           heap, scanCap);
            const uint8_t* p = (const uint8_t*)heap;
            for (size_t row = 0; row < 128; row += 16) {
                Hydro::logInfo("EngineAPI:     [+0x%02zX] "
                               "%02X %02X %02X %02X %02X %02X %02X %02X "
                               "%02X %02X %02X %02X %02X %02X %02X %02X",
                               row,
                               p[row+0],p[row+1],p[row+2],p[row+3],
                               p[row+4],p[row+5],p[row+6],p[row+7],
                               p[row+8],p[row+9],p[row+10],p[row+11],
                               p[row+12],p[row+13],p[row+14],p[row+15]);
            }
            for (size_t i = 0; i < kNumProbes; i++) {
                if (!globalIdx[i]) continue;
                uint32_t target = globalIdx[i];
                int hits = 0;
                size_t firstHit = 0;
                for (size_t off = 0; off + 4 <= scanCap; off += 4) {
                    uint32_t v;
                    std::memcpy(&v, p + off, 4);
                    if (v == target) {
                        if (!hits) firstHit = off;
                        hits++;
                        if (hits >= 8) break;
                    }
                }
                Hydro::logInfo("EngineAPI:   global idx %u ('%ls') found %d× in heap "
                               "(first @ +0x%zX)",
                               target, probes[i], hits, firstHit);
            }
        } else {
            Hydro::logWarn("EngineAPI:   tempState heap %p unreadable (VirtualQuery failed)",
                           heap);
        }
    }

    // FName-VIA-TOSTRING diagnostic - definitive divergence test.
    //
    // Per UE 5.6 source review (`AssetRegistryArchive.cpp:307-325`),
    // FAssetRegistryReader inline-remaps every FName via LoadNameBatch →
    // ToName(), producing global FNames. The values 58/59/13/etc. seen in
    // the heap dump should be **global FNameEntryId.Value**, not local
    // indices. Test by feeding them through Conv_NameToString (which
    // resolves through the live FNamePool) and seeing if the strings
    // round-trip to '/Game/Mods/HudTemplate/...'.
    //
    // Layout per heap dump: 16-byte TMap entries with shape:
    //   [+0..7]   FAssetData* (value)
    //   [+8..11]  FName::Number (0xFFFFFFFF = NAME_NO_NUMBER)
    //   [+12..15] FName::ComparisonIndex (small int - UE 5.6 layout has
    //             Number first, ComparisonIndex second per agent's source
    //             review of NameTypes.h:1373).
    //
    // Resolve the small int through getNameString() - if it returns the
    // mod path, FNames are global and divergence is REFUTED.
    {
        void* heap = nullptr;
        std::memcpy(&heap, tempState + 0, sizeof(heap));
        if (heap) {
            MEMORY_BASIC_INFORMATION mbi = {};
            if (VirtualQuery(heap, &mbi, sizeof(mbi)) &&
                (mbi.State & MEM_COMMIT)) {
                const uint8_t* p = (const uint8_t*)heap;
                // Probe first 5 entries (16-byte stride).
                for (size_t e = 0; e < 5; e++) {
                    size_t off = e * 16;
                    if (off + 16 > mbi.RegionSize) break;
                    uint32_t numberField = 0, idxField = 0;
                    std::memcpy(&numberField, p + off + 8,  4);
                    std::memcpy(&idxField,    p + off + 12, 4);
                    // Try the canonical layout first (idx at +12).
                    std::string s1 = getNameString(idxField);
                    // Also try +8 in case our layout interpretation is reversed.
                    std::string s2 = getNameString(numberField);
                    Hydro::logInfo("EngineAPI:   tempState heap[%zu]: "
                                   "FName{+8=0x%08X +12=0x%08X} "
                                   "→ ToString(+12)='%s' / ToString(+8)='%s'",
                                   e, numberField, idxField,
                                   s1.empty() ? "(empty)" : s1.c_str(),
                                   s2.empty() ? "(empty)" : s2.c_str());
                }
            }
        }
        // Also dereference first FAssetData* and read its first FName field
        // (PackageName at offset 0). Same two-layout test.
        void* fadHeap = nullptr;
        std::memcpy(&fadHeap, tempState + 0, sizeof(fadHeap));
        if (fadHeap) {
            void* firstFAD = nullptr;
            std::memcpy(&firstFAD, fadHeap, sizeof(firstFAD));
            if (firstFAD) {
                MEMORY_BASIC_INFORMATION fmbi = {};
                if (VirtualQuery(firstFAD, &fmbi, sizeof(fmbi)) &&
                    (fmbi.State & MEM_COMMIT) && fmbi.RegionSize >= 16) {
                    const uint8_t* fp = (const uint8_t*)firstFAD;
                    uint32_t f0 = 0, f4 = 0, f8 = 0, fC = 0;
                    std::memcpy(&f0, fp + 0,  4);
                    std::memcpy(&f4, fp + 4,  4);
                    std::memcpy(&f8, fp + 8,  4);
                    std::memcpy(&fC, fp + 12, 4);
                    std::string n0 = getNameString(f0);
                    std::string n4 = getNameString(f4);
                    std::string n8 = getNameString(f8);
                    std::string nC = getNameString(fC);
                    Hydro::logInfo("EngineAPI:   first FAssetData @ %p [+0..15]: "
                                   "0x%08X 0x%08X 0x%08X 0x%08X",
                                   firstFAD, f0, f4, f8, fC);
                    Hydro::logInfo("EngineAPI:   first FAssetData ToString: "
                                   "+0='%s' +4='%s' +8='%s' +12='%s'",
                                   n0.empty() ? "(empty)" : n0.c_str(),
                                   n4.empty() ? "(empty)" : n4.c_str(),
                                   n8.empty() ? "(empty)" : n8.c_str(),
                                   nC.empty() ? "(empty)" : nC.c_str());
                }
            }
        }
    }

    // CDO-vs-runtime AR check + GUObjectArray walk for live instance.
    // Hypothesis: s_assetRegImpl is the Default__AssetRegistryImpl CDO
    // (placeholder with uninitialized TMaps), not the runtime singleton.
    // Calling AppendState on a CDO walks empty hash tables → NumBuckets=0
    // → raw hash used as index → AV at exe+0x221218E with rcx=0x8041440C.
    void* arForCall = s_assetRegImpl;
    {
        uint32_t curFlags = 0;
        safeReadInt32((uint8_t*)s_assetRegImpl + UOBJ_FLAGS, (int32_t*)&curFlags);
        bool curIsCDO = (curFlags & 0x10) != 0;  // RF_ClassDefaultObject
        Hydro::logInfo("EngineAPI: s_assetRegImpl=%p flags=0x%08X %s",
                       s_assetRegImpl, curFlags,
                       curIsCDO ? "(CDO - RF_ClassDefaultObject set!)" : "(non-CDO runtime instance)");

        // Get s_assetRegImpl's UClass - we'll find sibling instances.
        void* targetClass = getObjClass(s_assetRegImpl);
        Hydro::logInfo("EngineAPI: s_assetRegImpl UClass=%p", targetClass);

        if (targetClass && s_guObjectArray) {
            int32_t count = getObjectCount();
            int matched = 0;
            int nonCDOCount = 0;
            void* firstNonCDO = nullptr;
            // Limit logging to first ~20 matches to avoid log spam if there
            // are many. Walk the entire UObject array.
            for (int32_t i = 0; i < count; i++) {
                void* obj = getObjectAt(i);
                if (!obj) continue;
                void* cls = getObjClass(obj);
                if (cls != targetClass) continue;
                matched++;
                uint32_t flags = 0;
                safeReadInt32((uint8_t*)obj + UOBJ_FLAGS, (int32_t*)&flags);
                bool isCDO = (flags & 0x10) != 0;
                if (!isCDO) {
                    nonCDOCount++;
                    if (!firstNonCDO) firstNonCDO = obj;
                }
                if (matched <= 10) {
                    Hydro::logInfo("EngineAPI:   GUObjectArray[%d] = %p flags=0x%08X %s%s",
                                   i, obj, flags,
                                   isCDO ? "CDO" : "runtime",
                                   obj == s_assetRegImpl ? " [== s_assetRegImpl]" : "");
                }
            }
            Hydro::logInfo("EngineAPI: GUObjectArray scan: %d total instance(s) of UClass %p, %d non-CDO",
                           matched, targetClass, nonCDOCount);

            if (curIsCDO && firstNonCDO) {
                Hydro::logWarn("EngineAPI: s_assetRegImpl IS the CDO - switching arForCall to runtime instance %p",
                               firstNonCDO);
                arForCall = firstNonCDO;
                void* cdoVT = nullptr; safeReadPtr(s_assetRegImpl, &cdoVT);
                void* rtVT  = nullptr; safeReadPtr(firstNonCDO, &rtVT);
                Hydro::logInfo("EngineAPI: CDO vtable=%p runtime vtable=%p (must match for safety)",
                               cdoVT, rtVT);
            } else if (!curIsCDO) {
                Hydro::logInfo("EngineAPI: s_assetRegImpl is already a runtime (non-CDO) instance - "
                               "CDO theory disproved; AV is something else");
            } else if (curIsCDO && !firstNonCDO) {
                // Subclass-aware fallback: exact UClass match found nothing, but
                // the runtime AR may be a SUBCLASS of UAssetRegistryImpl. Walk
                // GUObjectArray with parent-chain check: for each obj's UClass,
                // walk SuperStruct chain looking for `targetClass`.
                Hydro::logWarn("EngineAPI: no exact-class runtime - running subclass-aware walk "
                               "(check parent chain for UClass=%p)", targetClass);
                int superOff = (s_superOffset > 0) ? s_superOffset : 0x40;
                int subclassMatched = 0;
                int subclassNonCDO  = 0;
                void* firstSubclassNonCDO = nullptr;
                for (int32_t i = 0; i < count; i++) {
                    void* obj = getObjectAt(i);
                    if (!obj) continue;
                    void* cls = getObjClass(obj);
                    if (!cls) continue;
                    if (cls == targetClass) continue; // already counted in exact-match
                    // Walk parent chain
                    bool isSub = false;
                    void* cur = cls;
                    for (int d = 0; d < 64 && cur; d++) {
                        if (cur == targetClass) { isSub = true; break; }
                        void* super = nullptr;
                        if (!safeReadPtr((uint8_t*)cur + superOff, &super)) break;
                        if (super == cur) break;
                        cur = super;
                    }
                    if (!isSub) continue;
                    subclassMatched++;
                    uint32_t flags = 0;
                    safeReadInt32((uint8_t*)obj + UOBJ_FLAGS, (int32_t*)&flags);
                    bool isCDO = (flags & 0x10) != 0;
                    if (!isCDO) {
                        subclassNonCDO++;
                        if (!firstSubclassNonCDO) firstSubclassNonCDO = obj;
                    }
                    if (subclassMatched <= 10) {
                        Hydro::logInfo("EngineAPI:   subclass GUObjectArray[%d] = %p UClass=%p flags=0x%08X %s",
                                       i, obj, cls, flags, isCDO ? "CDO" : "runtime");
                    }
                }
                Hydro::logInfo("EngineAPI: subclass walk: %d subclass-instance(s), %d non-CDO",
                               subclassMatched, subclassNonCDO);
                if (firstSubclassNonCDO) {
                    Hydro::logWarn("EngineAPI: switching arForCall to subclass runtime instance %p",
                                   firstSubclassNonCDO);
                    arForCall = firstSubclassNonCDO;
                    void* cdoVT = nullptr; safeReadPtr(s_assetRegImpl, &cdoVT);
                    void* rtVT  = nullptr; safeReadPtr(firstSubclassNonCDO, &rtVT);
                    Hydro::logInfo("EngineAPI: CDO vtable=%p subclass vtable=%p", cdoVT, rtVT);
                } else {
                    Hydro::logWarn("EngineAPI: NO subclass runtime instance either - "
                                   "AR truly absent at tick 120 OR runtime isn't a UObject");
                }
            }
        }
    }

    // Original live-AR cross-check via UAssetManager::GetAssetRegistry path.
    {
        void* getRegFn = findObject(L"/Script/AssetRegistry.AssetRegistryHelpers:GetAssetRegistry");
        if (getRegFn && s_assetRegHelpersCDO && s_processEvent) {
            uint8_t regParams[32] = {};
            if (callProcessEvent(s_assetRegHelpersCDO, getRegFn, regParams)) {
                // FScriptInterface = { UObject* ObjectPointer; void* InterfacePointer }
                void* liveAR_obj   = *(void**)(regParams + 0x00);
                void* liveAR_iface = *(void**)(regParams + 0x08);
                Hydro::logInfo("EngineAPI: live-AR cross-check - "
                               "AssetRegistryHelpers::GetAssetRegistry() ObjectPointer=%p InterfacePointer=%p",
                               liveAR_obj, liveAR_iface);
                Hydro::logInfo("EngineAPI: live-AR cross-check - s_assetRegImpl=%p",
                               s_assetRegImpl);
                if (liveAR_obj && liveAR_obj != s_assetRegImpl) {
                    Hydro::logWarn("EngineAPI: live-AR DIFFERS from s_assetRegImpl - "
                                   "switching to live AR for this AppendState call");
                    arForCall = liveAR_obj;
                } else if (liveAR_obj == s_assetRegImpl) {
                    Hydro::logInfo("EngineAPI: live-AR MATCHES s_assetRegImpl - "
                                   "instance is correct, AV is something else");
                }
                // The InterfacePointer is the IAssetRegistry* - secondary vtable
                // lives there. Useful for vtable-slot lookup too.
                if (liveAR_iface && liveAR_iface != liveAR_obj) {
                    void** ifaceVtable = nullptr;
                    std::memcpy(&ifaceVtable, liveAR_iface, sizeof(ifaceVtable));
                    Hydro::logInfo("EngineAPI: live-AR InterfacePointer is offset within object "
                                   "(offset=%lld); IAssetRegistry secondary vtable @ %p",
                                   (long long)((uint8_t*)liveAR_iface - (uint8_t*)liveAR_obj),
                                   ifaceVtable);
                    // Quick scan of the SECONDARY vtable for AppendState match
                    if (ifaceVtable) {
                        MEMORY_BASIC_INFORMATION mbi = {};
                        if (VirtualQuery(ifaceVtable, &mbi, sizeof(mbi)) &&
                            (mbi.State & MEM_COMMIT)) {
                            size_t avail = ((uintptr_t)mbi.BaseAddress + mbi.RegionSize) -
                                           (uintptr_t)ifaceVtable;
                            size_t maxSlots = (std::min)((size_t)100, avail / sizeof(void*));
                            int matches = 0;
                            for (size_t s = 0; s < maxSlots; s++) {
                                void* slot = ifaceVtable[s];
                                if (!slot) continue;
                                if (slot == s_uarAppendStateFn) {
                                    matches++;
                                    Hydro::logInfo("EngineAPI: SECONDARY vtable[%zu] = exe+0x%zX (== s_uarAppendStateFn) ✓",
                                                   s, (size_t)((uint8_t*)slot - s_gm.base));
                                }
                            }
                            Hydro::logInfo("EngineAPI: secondary vtable scan - %d match(es) for s_uarAppendStateFn in first %zu slots",
                                           matches, maxSlots);
                            // Dump first 20 secondary-vtable slots so we can see candidates
                            for (size_t s = 0; s < 20 && s < maxSlots; s++) {
                                void* slot = ifaceVtable[s];
                                if (!slot) {
                                    Hydro::logInfo("EngineAPI:   secondary vtable[%zu] = NULL", s);
                                    continue;
                                }
                                if ((uint8_t*)slot < s_gm.base ||
                                    (uint8_t*)slot >= s_gm.base + s_gm.size) {
                                    Hydro::logInfo("EngineAPI:   secondary vtable[%zu] = %p (out-of-module)",
                                                   s, slot);
                                    continue;
                                }
                                uint32_t sSize = funcSizeViaPdata((uint8_t*)slot);
                                Hydro::logInfo("EngineAPI:   secondary vtable[%zu] = exe+0x%zX size=%u",
                                               s, (size_t)((uint8_t*)slot - s_gm.base), sSize);
                            }
                        }
                    }
                }
            } else {
                Hydro::logWarn("EngineAPI: live-AR cross-check - callProcessEvent failed");
            }
        } else {
            Hydro::logWarn("EngineAPI: live-AR cross-check skipped - "
                           "getRegFn=%p s_assetRegHelpersCDO=%p s_processEvent=%p",
                           getRegFn, s_assetRegHelpersCDO, s_processEvent);
        }
    }

    // IAssetRegistrySingleton::Singleton discovery via call-site pattern scan.
    //
    // Per UE 5.6 source (AssetRegistry.cpp:911-915 + AssetRegistryInterface.h:283-293
    // + AssetRegistryInterface.cpp:27): `IAssetRegistry::Get()` is an INLINE
    // function that reads a single 8-byte global slot
    // `IAssetRegistrySingleton::Singleton` in CoreUObject's .data section.
    // The slot is set ONCE inside `UAssetRegistryImpl::PostInitProperties` (the
    // CDO ctor), to `this` (= the CDO itself). So the CDO IS the live runtime
    // singleton. Our prior "CDO ≠ runtime" theory was inverted.
    //
    // Discovery primitive: every caller of IAssetRegistry::Get inlines as
    //   `48 8B 05 disp32        mov rax, [rip+disp32]   ; load Singleton`
    //   `48 85 C0               test rax, rax`
    //   `74 ??` or `0F 84 ...`  je short/near (null check)
    // Scan the binary for this pattern; the disp32 target referenced most often
    // IS IAssetRegistrySingleton::Singleton. Dereference → live AR pointer.
    // If it matches s_assetRegImpl, we have ground-truth confirmation that
    // the CDO is correct. (Also: the dereferenced value is what UE itself uses,
    // so passing it as arForCall is the most direct path.)
    {
        // Pattern: 48 8B 05 ?? ?? ?? ?? 48 85 C0  (10 bytes)
        //  = mov rax, [rip+disp32]; test rax, rax
        std::unordered_map<uintptr_t, int> globalRefs;
        size_t totalScanned = 0;
        for (uint8_t* p = s_gm.base; p + 10 < s_gm.base + s_gm.size; p++) {
            if (p[0] != 0x48 || p[1] != 0x8B || p[2] != 0x05) continue;
            if (p[7] != 0x48 || p[8] != 0x85 || p[9] != 0xC0) continue;
            int32_t disp32 = *(int32_t*)(p + 3);
            uintptr_t instrEnd = (uintptr_t)(p + 7);
            uintptr_t globalAddr = instrEnd + (intptr_t)disp32;
            if (globalAddr < (uintptr_t)s_gm.base ||
                globalAddr + 8 > (uintptr_t)(s_gm.base + s_gm.size)) continue;
            globalRefs[globalAddr]++;
            totalScanned++;
        }
        // Sort by hit count descending; take top 5 candidates.
        std::vector<std::pair<uintptr_t, int>> sorted(globalRefs.begin(), globalRefs.end());
        std::sort(sorted.begin(), sorted.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });
        Hydro::logInfo("EngineAPI: singleton scan: %zu unique disp32 globals from %zu hits",
                       sorted.size(), totalScanned);
        const size_t topN = (sorted.size() < 8) ? sorted.size() : 8;
        for (size_t i = 0; i < topN; i++) {
            uintptr_t addr = sorted[i].first;
            void* val = nullptr;
            safeReadPtr((void*)addr, &val);
            Hydro::logInfo("EngineAPI:   top[%zu] global exe+0x%zX hits=%d *=%p%s",
                           i, (size_t)(addr - (uintptr_t)s_gm.base), sorted[i].second, val,
                           val == s_assetRegImpl ? " [== s_assetRegImpl ✓]" : "");
        }

        // Find the highest-ranked candidate whose dereferenced value matches
        // s_assetRegImpl. That global IS IAssetRegistrySingleton::Singleton.
        // (It will not always be the #1 most-referenced - engine has many
        // popular globals - so accept any top-N hit that points at the CDO.)
        uintptr_t singletonAddr = 0;
        void* singletonVal = nullptr;
        int singletonHits = 0;
        for (size_t i = 0; i < topN; i++) {
            void* val = nullptr;
            if (!safeReadPtr((void*)sorted[i].first, &val) || !val) continue;
            if (val == s_assetRegImpl) {
                singletonAddr = sorted[i].first;
                singletonVal = val;
                singletonHits = sorted[i].second;
                break;
            }
        }
        if (singletonAddr) {
            Hydro::logInfo("EngineAPI: ✓ IAssetRegistrySingleton::Singleton @ exe+0x%zX "
                           "hits=%d *=%p (== s_assetRegImpl) - ground truth: CDO IS runtime",
                           (size_t)(singletonAddr - (uintptr_t)s_gm.base),
                           singletonHits, singletonVal);
            // Use the dereferenced singleton value (== s_assetRegImpl) as
            // arForCall. This is what UE itself uses for `IAssetRegistry::Get`.
            arForCall = singletonVal;
        } else {
            Hydro::logWarn("EngineAPI: no top-N global dereferences to s_assetRegImpl=%p - "
                           "either Singleton is set elsewhere OR the CDO theory is wrong; "
                           "falling back to s_assetRegImpl directly", s_assetRegImpl);
            arForCall = s_assetRegImpl;
        }
    }

    // Wide hex dump of s_assetRegImpl from +0x00 to +0x100. We need to see
    // 0..0x10 (vtable + flags) and look for the IAssetRegistry secondary
    // vtable pointer (a module address like 0x7FF6_xxxx_xxxx) - that pointer
    // marks IAssetRegistry-base; offset of vtable from s_assetRegImpl IS K.
    if (s_assetRegImpl) {
        Hydro::logInfo("EngineAPI: s_assetRegImpl wide hex dump (looking for "
                       "IAssetRegistry vtable pointer = module addr at offset K):");
        for (int row = 0; row < 0x110; row += 0x10) {
            uint8_t bytes[16] = {};
            bool ok = true;
            for (int j = 0; j < 16; j += 4) {
                int32_t v = 0;
                if (!safeReadInt32((uint8_t*)s_assetRegImpl + row + j, &v)) { ok = false; break; }
                std::memcpy(bytes + j, &v, 4);
            }
            if (!ok) {
                Hydro::logWarn("EngineAPI:   [+0x%03X]: read failed", row);
                continue;
            }
            // Check each 8-byte qword in this row to see if it's a module
            // pointer (in [s_gm.base, s_gm.base+s_gm.size)).
            uint64_t q0, q1;
            std::memcpy(&q0, bytes + 0, 8);
            std::memcpy(&q1, bytes + 8, 8);
            const char* tagQ0 = "";
            const char* tagQ1 = "";
            if (q0 >= (uint64_t)s_gm.base &&
                q0 <  (uint64_t)s_gm.base + s_gm.size) tagQ0 = " [Q0=MODULE PTR]";
            if (q1 >= (uint64_t)s_gm.base &&
                q1 <  (uint64_t)s_gm.base + s_gm.size) tagQ1 = " [Q1=MODULE PTR]";
            Hydro::logInfo("EngineAPI:   [+0x%03X]: %02X %02X %02X %02X %02X %02X %02X %02X "
                           "%02X %02X %02X %02X %02X %02X %02X %02X%s%s",
                           row,
                           bytes[0], bytes[1], bytes[2], bytes[3],
                           bytes[4], bytes[5], bytes[6], bytes[7],
                           bytes[8], bytes[9], bytes[10], bytes[11],
                           bytes[12], bytes[13], bytes[14], bytes[15],
                           tagQ0, tagQ1);
        }
    }

    // Zydis decode of s_uarAppendStateFn first ~40 instructions - pure log,
    // no execution. Lets us see what the function actually does to derive K
    // (or determine if we resolved the wrong function entirely).
    if (s_uarAppendStateFn) {
        ZydisDecoder decoder;
        if (ZYAN_SUCCESS(ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64,
                                          ZYDIS_STACK_WIDTH_64))) {
            ZydisFormatter formatter;
            ZydisFormatterInit(&formatter, ZYDIS_FORMATTER_STYLE_INTEL);
            Hydro::logInfo("EngineAPI: s_uarAppendStateFn (exe+0x%zX) "
                           "first 40 instructions:",
                           (size_t)((uint8_t*)s_uarAppendStateFn - s_gm.base));
            uint8_t* fn = (uint8_t*)s_uarAppendStateFn;
            int instCount = 0;
            constexpr int kMaxInsts = 40;
            constexpr int kMaxBytes = 500;
            int pos = 0;
            while (instCount < kMaxInsts && pos < kMaxBytes) {
                ZydisDecodedInstruction inst;
                ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
                if (ZYAN_FAILED(ZydisDecoderDecodeFull(&decoder, fn + pos,
                                                       kMaxBytes - pos, &inst, ops))) break;
                char buf[256];
                ZydisFormatterFormatInstruction(&formatter, &inst, ops,
                    inst.operand_count_visible, buf, sizeof(buf),
                    (ZyanU64)(fn + pos), nullptr);
                Hydro::logInfo("EngineAPI:   [+0x%03X] %s", pos, buf);
                instCount++;
                if (inst.mnemonic == ZYDIS_MNEMONIC_RET) break;
                pos += inst.length;
            }
        }
    }

    // K-discovery: find the IAssetRegistry secondary vtable pointer within
    // s_assetRegImpl. UAssetRegistryImpl uses MI (UObject + IAssetRegistry);
    // the secondary base has a vtable pointer at offset K from the object
    // start. K IS the IAssetRegistry MI offset.
    //
    // Resilient discovery: scan s_assetRegImpl[+0x08..+0x100] in 8-byte stride
    // for the FIRST qword that's a module pointer (in [s_gm.base, s_gm.base+s_gm.size)).
    // The UObject primary vtable is at +0x00 - skip it. The next module
    // pointer in the object IS the IAssetRegistry secondary vtable. Its
    // offset is K. This pattern survives all UE build variants because
    // multiple-inheritance vtable placement is dictated by C++ ABI, not
    // by UE config flags.
    intptr_t discoveredK = -1;
    {
        for (intptr_t off = 0x08; off < 0x100; off += 8) {
            void* val = nullptr;
            if (!safeReadPtr((uint8_t*)s_assetRegImpl + off, &val)) continue;
            if (!val) continue;
            if (val >= (void*)s_gm.base &&
                (uint8_t*)val < s_gm.base + s_gm.size) {
                discoveredK = off;
                Hydro::logInfo("EngineAPI: ✓ K-discovery - IAssetRegistry secondary vtable "
                               "at s_assetRegImpl+0x%zX (vtable=%p, in module range); K=0x%zX",
                               (size_t)off, val, (size_t)off);
                break;
            }
        }
        if (discoveredK < 0) {
            Hydro::logWarn("EngineAPI: K-discovery - no module pointer in "
                           "s_assetRegImpl[0x08..0x100]; cannot compute K");
        }
    }
    // Adjusted `this` for the AppendState call.
    void* arForCall_kAdjusted = (discoveredK >= 0)
        ? (void*)((uint8_t*)s_assetRegImpl + discoveredK)
        : nullptr;
    bool appendOk = false;  // forward-declared; set true on K-call success
    // Env-var-gated one-shot AppendState call with K-adjusted `this`.
    // ONE attempt only - if AppendState faults inside the lock-acquired region
    // it leaks the lock, and any retry deadlocks. User opts in deliberately
    // via HYDRO_TRY_K_APPEND=1 in the launch batch file.
    bool tryKAppend = false;
    {
        char buf[8] = {};
        size_t len = 0;
        if (getenv_s(&len, buf, sizeof(buf), "HYDRO_TRY_K_APPEND") == 0 &&
            len >= 2 && buf[0] == '1') {
            tryKAppend = true;
        }
    }
    if (tryKAppend && arForCall_kAdjusted) {
        Hydro::logInfo("EngineAPI: HYDRO_TRY_K_APPEND=1 - attempting one-shot AppendState "
                       "with this = s_assetRegImpl + 0x%zX = %p",
                       (size_t)discoveredK, arForCall_kAdjusted);
        // Pre-merge TMap snapshot at the populated TMaps we identified
        // (s_assetRegImpl+0x90, +0xB0, +0xC0). Read NumElements/NumBuckets
        // from each so we can compare against post-merge state.
        struct MapSnap { uintptr_t off; uint32_t numElems; uint32_t numBuckets; };
        const uintptr_t mapOffsets[] = { 0x20, 0x70, 0x90, 0xB0, 0xC0, 0xE0 };
        constexpr size_t kNumOffsets = sizeof(mapOffsets) / sizeof(mapOffsets[0]);
        MapSnap pre[kNumOffsets] = {};
        for (size_t i = 0; i < kNumOffsets; i++) {
            pre[i].off = mapOffsets[i];
            int32_t n = 0, b = 0;
            safeReadInt32((uint8_t*)s_assetRegImpl + mapOffsets[i] + 8,  &n);
            safeReadInt32((uint8_t*)s_assetRegImpl + mapOffsets[i] + 12, &b);
            pre[i].numElems = (uint32_t)n;
            pre[i].numBuckets = (uint32_t)b;
        }
        bool ok = sehCallAppendState(s_uarAppendStateFn, arForCall_kAdjusted, tempState);
        if (ok) {
            Hydro::logInfo("EngineAPI: ✓✓✓ AppendState SUCCEEDED with K-adjusted this - "
                           "AR.bin merge call returned");
            // Post-merge probe: count delta tells us if merge actually wrote.
            for (size_t i = 0; i < kNumOffsets; i++) {
                int32_t n = 0, b = 0;
                safeReadInt32((uint8_t*)s_assetRegImpl + mapOffsets[i] + 8,  &n);
                safeReadInt32((uint8_t*)s_assetRegImpl + mapOffsets[i] + 12, &b);
                int32_t deltaN = (int32_t)((uint32_t)n - pre[i].numElems);
                int32_t deltaB = (int32_t)((uint32_t)b - pre[i].numBuckets);
                Hydro::logInfo("EngineAPI:   TMap[+0x%02zX]: pre {N=%u B=%u} post {N=%u B=%u} ΔN=%+d ΔB=%+d %s",
                               (size_t)mapOffsets[i],
                               pre[i].numElems, pre[i].numBuckets,
                               (uint32_t)n, (uint32_t)b, deltaN, deltaB,
                               deltaN > 0 ? "[GREW - merge wrote data]" :
                               deltaN == 0 ? "[unchanged]" : "[shrank?!]");
            }
            appendOk = true;  // cancel downstream stale-fault dump
        } else {
            DWORD64 rva = 0;
            if (s_lastAppendFaultAddr >= (DWORD64)s_gm.base &&
                s_lastAppendFaultAddr <  (DWORD64)s_gm.base + s_gm.size) {
                rva = s_lastAppendFaultAddr - (DWORD64)s_gm.base;
            }
            Hydro::logWarn("EngineAPI: ✗ AppendState faulted at exe+0x%llX raw=0x%llX "
                           "code=0x%08X (one-shot - InterfaceLock may now be leaked; "
                           "next AR access could deadlock)",
                           (unsigned long long)rva,
                           (unsigned long long)s_lastAppendFaultAddr,
                           s_lastAppendFaultCode);
            Hydro::logWarn("EngineAPI:   regs at fault:");
            Hydro::logWarn("EngineAPI:     RAX=%016llX RCX=%016llX RDX=%016llX RBX=%016llX",
                           s_lastAppendFaultRegs[0], s_lastAppendFaultRegs[1],
                           s_lastAppendFaultRegs[2], s_lastAppendFaultRegs[3]);
            Hydro::logWarn("EngineAPI:     R8 =%016llX R9 =%016llX R10=%016llX R11=%016llX",
                           s_lastAppendFaultRegs[8], s_lastAppendFaultRegs[9],
                           s_lastAppendFaultRegs[10], s_lastAppendFaultRegs[11]);
        }
    } else if (!tryKAppend) {
        Hydro::logInfo("EngineAPI: HYDRO_TRY_K_APPEND not set - skipping AppendState call. "
                       "Set HYDRO_TRY_K_APPEND=1 in launch.bat to attempt the merge.");
    } else {
        Hydro::logWarn("EngineAPI: HYDRO_TRY_K_APPEND=1 but K-discovery failed; "
                       "cannot attempt AppendState");
    }

    // FPackageStore discovery - wide net. Shipping builds strip UE_LOG format
    // strings; canonical 5.6 source anchors aren't present. Try a battery of
    // wide AND narrow candidates: log category names, class names, RTTI type
    // descriptors, file paths from check() macros, profiler scope names.
    //
    // For each match: log offset, search for LEA refs, log containing functions.
    static bool s_psDiscoveryDone = false;
    if (!s_psDiscoveryDone) {
        s_psDiscoveryDone = true;
        Hydro::logInfo("EngineAPI: FPackageStore discovery - scanning for any mount-related anchor:");
        struct WideAnchor { const wchar_t* str; const char* note; };
        struct NarrowAnchor { const char* str; const char* note; };
        const WideAnchor wides[] = {
            { L"FilePackageStore", "wide class/category" },
            { L"PackageStore", "wide generic" },
            { L"IoStore container", "wide log fragment" },
            { L"FMountedContainer", "wide struct" },
            { L"FFilePackageStoreBackend", "wide class" },
            { L"PackageStoreBackend", "wide class fragment" },
            { L"LogFilePackageStore", "wide log category" },
            { L"LogPackageStore", "wide log category" },
            { L"LogIoStore", "wide log category" },
            { L"NumPackages", "wide log fmt frag" },
            { L"Order=%u", "wide log fmt frag" },
            { L"FIoContainerHeader", "wide struct" },
        };
        const NarrowAnchor narrows[] = {
            { "FilePackageStore", "narrow class/category" },
            { "FFilePackageStoreBackend", "narrow class" },
            { "PackageStoreBackend", "narrow class fragment" },
            { "LogFilePackageStore", "narrow log category" },
            { "FFileIoStore", "narrow class A" },
            { "FPakPlatformFile", "narrow class caller" },
            { "FilePackageStore.cpp", "narrow file path" },
            { "IPlatformFilePak.cpp", "narrow file path" },
            { "IoDispatcherFileBackend.cpp", "narrow file path" },
            { ".?AVFFilePackageStoreBackend@@", "RTTI descriptor" },
            { ".?AVFPakPlatformFile@@", "RTTI descriptor" },
            { ".?AVFFileIoStore@@", "RTTI descriptor" },
        };
        auto reportFn = [&](uint8_t* strAddr, const char* note, const char* enc) {
            auto refs = findAllLeaRefs(s_gm.base, s_gm.size, strAddr);
            Hydro::logInfo("EngineAPI:   ✓ [%s %s] @ exe+0x%zX  (%zu LEA-ref(s))",
                           enc, note, (size_t)(strAddr - s_gm.base), refs.size());
            std::unordered_set<uint8_t*> uniqueFns;
            for (auto* r : refs) {
                uint8_t* fn = funcStartViaPdata(r);
                if (fn) uniqueFns.insert(fn);
            }
            size_t cnt = 0;
            for (auto* fn : uniqueFns) {
                uint32_t sz = funcSizeViaPdata(fn);
                Hydro::logInfo("EngineAPI:       containing fn[%zu] = exe+0x%zX size=%u",
                               cnt++, (size_t)(fn - s_gm.base), sz);
                if (cnt >= 8) break;
            }
        };
        for (const auto& a : wides) {
            uint8_t* p = findWideString(s_gm.base, s_gm.size, a.str);
            if (p) reportFn(p, a.note, "WIDE");
        }
        for (const auto& a : narrows) {
            uint8_t* p = findAsciiString(s_gm.base, s_gm.size, a.str);
            if (p) reportFn(p, a.note, "ASCII");
        }
        Hydro::logInfo("EngineAPI: FPackageStore discovery - done");
    }

    // Stage 2: Meyers-singleton byte-pattern scan for FPackageStore::Get().
    // Per UE 5.6 source (PackageStore.cpp:174-178): `static FPackageStore Instance;
    // return Instance;`. After init (always done by tick 120), the hot path is
    // a `lea rax, [rip+disp32]; ret` 8-byte leaf returning &Instance.
    //
    // FPackageStore layout (PackageStore.h:222-228) is ~32 bytes:
    //   +0:  TSharedRef<FPackageStoreBackendContext> BackendContext  (16 B)
    //         { Object*, Controller* } both heap pointers
    //   +16: TArray<TTuple<int32, TSharedRef<IPackageStoreBackend>>> Backends (16 B)
    //         { Data*, ArrayNum, ArrayMax }
    //
    // Filter for the singleton by data-shape match. Then walk Backends to
    // get FFilePackageStoreBackend*. Then read its MountedContainers and
    // log each entry's ContainerHeader.
    static bool s_psSingletonScanned = false;
    if (!s_psSingletonScanned) {
        s_psSingletonScanned = true;
        Hydro::logInfo("EngineAPI: FPackageStore singleton scan - searching for "
                       "`lea rax, [rip+disp32]; ret` 8B leafs with FPackageStore data shape:");
        struct Candidate { uint8_t* fn; uintptr_t target; };
        std::vector<Candidate> cands;
        for (uint8_t* p = s_gm.base; p + 8 < s_gm.base + s_gm.size; p++) {
            if (p[0] != 0x48 || p[1] != 0x8D || p[2] != 0x05) continue;
            if (p[7] != 0xC3) continue;
            int32_t disp32 = *(int32_t*)(p + 3);
            uintptr_t target = (uintptr_t)(p + 7) + disp32;
            if (target < (uintptr_t)s_gm.base ||
                target + 32 > (uintptr_t)(s_gm.base + s_gm.size)) continue;
            // Data shape filter: read 32 bytes at target, check for
            //   q[0]=heap (TSharedRef Object), q[2]=heap or null (TArray Data),
            //   q[3] low/high uint32 = small (ArrayNum, ArrayMax).
            uint64_t q[4] = {};
            bool readOk = true;
            for (int i = 0; i < 4; i++) {
                int32_t lo = 0, hi = 0;
                if (!safeReadInt32((void*)(target + i * 8),     &lo)) { readOk = false; break; }
                if (!safeReadInt32((void*)(target + i * 8 + 4), &hi)) { readOk = false; break; }
                q[i] = ((uint64_t)(uint32_t)hi << 32) | (uint32_t)lo;
            }
            if (!readOk) continue;
            // Tight heap-pointer filter: high 16 bits must be 0 (Win64 user-space),
            // value > 4 GB (above ASLR low region), 8-byte aligned.
            auto isHeapPtr = [](uint64_t v) {
                if ((v >> 48) != 0) return false;       // upper 16 bits must be 0
                if (v < 0x0000000100000000ULL) return false;  // > 4 GB
                if ((v & 0x7) != 0) return false;       // 8-byte aligned
                return true;
            };
            // q[0] = TSharedRef Object* - must be heap
            // q[1] = TSharedRef Controller* - must be heap
            // q[2] = TArray Data* - heap or null (rarely null when Backends non-empty)
            // q[3] = (ArrayMax << 32) | ArrayNum - both should be small (1-8 typical)
            uint32_t arrNum = (uint32_t)q[3];
            uint32_t arrMax = (uint32_t)(q[3] >> 32);
            bool counts_ok = (arrNum >= 1 && arrNum <= 8 &&
                              arrMax >= arrNum && arrMax <= 16);
            bool q0_heap = isHeapPtr(q[0]);
            bool q1_heap = isHeapPtr(q[1]);
            bool q2_heap = isHeapPtr(q[2]);
            if (q0_heap && q1_heap && q2_heap && counts_ok) {
                cands.push_back({p, target});
            }
        }
        Hydro::logInfo("EngineAPI:   found %zu candidate(s) matching FPackageStore data shape",
                       cands.size());
        size_t logged = 0;
        for (const auto& c : cands) {
            if (logged++ >= 12) break;
            uint64_t q[4] = {};
            for (int i = 0; i < 4; i++) {
                int32_t lo = 0, hi = 0;
                safeReadInt32((void*)(c.target + i * 8),     &lo);
                safeReadInt32((void*)(c.target + i * 8 + 4), &hi);
                q[i] = ((uint64_t)(uint32_t)hi << 32) | (uint32_t)lo;
            }
            uint32_t arrNum = (uint32_t)q[3];
            uint32_t arrMax = (uint32_t)(q[3] >> 32);
            Hydro::logInfo("EngineAPI:   cand fn=exe+0x%zX target=%p  Q[0]=%016llX Q[1]=%016llX Q[2]=%016llX Num=%u Max=%u",
                           (size_t)(c.fn - s_gm.base), (void*)c.target,
                           (unsigned long long)q[0], (unsigned long long)q[1],
                           (unsigned long long)q[2], arrNum, arrMax);
            // Stage 3 attempt: walk Backends array.
            // Element is TTuple<int32, TSharedRef> = 4 + 16 = 20 B padded to 24 B.
            // TSharedRef = { ObjectPtr (8), ControllerPtr (8) }.
            uintptr_t arrData = q[2];
            uint32_t entryStride = 24;  // most likely with padding
            for (uint32_t i = 0; i < arrNum && i < 8; i++) {
                uintptr_t entryAddr = arrData + i * entryStride;
                int32_t prio = 0;
                int32_t loObj = 0, hiObj = 0;
                int32_t loCtl = 0, hiCtl = 0;
                if (!safeReadInt32((void*)entryAddr, &prio)) break;
                if (!safeReadInt32((void*)(entryAddr + 8),  &loObj)) break;
                if (!safeReadInt32((void*)(entryAddr + 12), &hiObj)) break;
                if (!safeReadInt32((void*)(entryAddr + 16), &loCtl)) break;
                if (!safeReadInt32((void*)(entryAddr + 20), &hiCtl)) break;
                uint64_t obj = ((uint64_t)(uint32_t)hiObj << 32) | (uint32_t)loObj;
                uint64_t ctl = ((uint64_t)(uint32_t)hiCtl << 32) | (uint32_t)loCtl;
                // Read backend's vtable pointer (first 8 bytes of obj)
                uint64_t vt = 0;
                if (obj) {
                    int32_t vlo = 0, vhi = 0;
                    safeReadInt32((void*)obj, &vlo);
                    safeReadInt32((void*)(obj + 4), &vhi);
                    vt = ((uint64_t)(uint32_t)vhi << 32) | (uint32_t)vlo;
                }
                bool vtInModule = (vt >= (uint64_t)s_gm.base &&
                                   vt <  (uint64_t)s_gm.base + s_gm.size);
                Hydro::logInfo("EngineAPI:     backend[%u] prio=%d obj=%016llX vtable=%016llX%s ctl=%016llX",
                               i, prio,
                               (unsigned long long)obj,
                               (unsigned long long)vt,
                               vtInModule ? " [in-module]" : " [out-of-module]",
                               (unsigned long long)ctl);
                if (vtInModule && obj) {
                    // Stage 4: try to find MountedContainers TArray on this backend.
                    // backend layout: vtable(8) + lock(8) + lock(8) + TArray(16) + ...
                    // sweep offsets +16, +24, +32 for TArray-shape header
                    for (uint32_t off = 16; off <= 64; off += 8) {
                        uint64_t qm[2] = {};
                        int32_t lo = 0, hi = 0;
                        safeReadInt32((void*)(obj + off),     &lo);
                        safeReadInt32((void*)(obj + off + 4), &hi);
                        qm[0] = ((uint64_t)(uint32_t)hi << 32) | (uint32_t)lo;
                        safeReadInt32((void*)(obj + off + 8),  &lo);
                        safeReadInt32((void*)(obj + off + 12), &hi);
                        qm[1] = ((uint64_t)(uint32_t)hi << 32) | (uint32_t)lo;
                        uint32_t mcNum = (uint32_t)qm[1];
                        uint32_t mcMax = (uint32_t)(qm[1] >> 32);
                        bool mc_ptr_ok = (qm[0] == 0) ||
                            (qm[0] >= 0x0000000100000000ULL && qm[0] < 0x0000800000000000ULL);
                        bool mc_counts_ok = (mcNum < 1000 && mcMax < 1000 && mcMax >= mcNum);
                        if (mc_ptr_ok && mc_counts_ok && (mcNum > 0 || qm[0] != 0)) {
                            Hydro::logInfo("EngineAPI:       MountedContainers? @ obj+0x%X "
                                           "Data=%016llX Num=%u Max=%u",
                                           off, (unsigned long long)qm[0], mcNum, mcMax);
                            // Read up to 4 FMountedContainer entries (24 B each).
                            for (uint32_t j = 0; j < mcNum && j < 4; j++) {
                                uintptr_t mcAddr = qm[0] + j * 24;
                                int32_t mlo = 0, mhi = 0;
                                safeReadInt32((void*)mcAddr,       &mlo);
                                safeReadInt32((void*)(mcAddr + 4), &mhi);
                                uint64_t hdr = ((uint64_t)(uint32_t)mhi << 32) | (uint32_t)mlo;
                                int32_t order = 0, seq = 0;
                                safeReadInt32((void*)(mcAddr + 8),  &order);
                                safeReadInt32((void*)(mcAddr + 12), &seq);
                                Hydro::logInfo("EngineAPI:         mc[%u]: hdr=%016llX order=%d seq=%d",
                                               j, (unsigned long long)hdr, order, seq);
                            }
                        }
                    }
                }
            }
        }
        Hydro::logInfo("EngineAPI: FPackageStore singleton scan - done");
    }

    // -- FPakPlatformFile::Mount static disassembly -----------------------
    //
    // PakLoader already discovers FPakPlatformFile's vtable via L"PakFile"
    // anchor (exe+0x6DB8918 on this build). Its instance scan fails - no
    // qword in writable memory equals the vtable address. That blocks
    // dispatching Mount via vtable, but NOT static analysis: we can find
    // the Mount function statically and disassemble its body to identify
    // the call into FFilePackageStoreBackend::Mount + the field offset
    // where the backend pointer is stored on FPakPlatformFile. With those
    // two facts, we have everything to register a pak with the package
    // store *if* we can locate either FPakPlatformFile or the backend
    // instance via another path.
    //
    // Anchor: L"utoc not found" - FIoStatus error string at
    // IPlatformFilePak.cpp:6085, single occurrence inside Mount.
    static bool s_fpakMountDisasmed = false;
    if (!s_fpakMountDisasmed) {
        s_fpakMountDisasmed = true;
        Hydro::logInfo("EngineAPI: FPakPlatformFile::Mount static disasm -");
        const wchar_t* anchors[] = { L"utoc not found", L"IoStore container" };
        uint8_t* mountFn = nullptr;
        for (const wchar_t* anchor : anchors) {
            uint8_t* strAddr = findWideString(s_gm.base, s_gm.size, anchor);
            if (!strAddr) {
                Hydro::logInfo("EngineAPI:   anchor L'%ls' not found", anchor);
                continue;
            }
            auto leas = findAllLeaRefs(s_gm.base, s_gm.size, strAddr);
            Hydro::logInfo("EngineAPI:   anchor L'%ls' @ exe+0x%zX  (%zu LEA-ref(s))",
                           anchor, (size_t)(strAddr - s_gm.base), leas.size());
            for (auto* lea : leas) {
                uint8_t* fn = funcStartViaPdata(lea);
                if (!fn) continue;
                uint32_t sz = funcSizeViaPdata(fn);
                Hydro::logInfo("EngineAPI:     containing fn=exe+0x%zX size=%u",
                               (size_t)(fn - s_gm.base), sz);
                if (!mountFn && sz >= 200 && sz <= 0x4000) mountFn = fn;
            }
            if (mountFn) break;
        }
        if (!mountFn) {
            Hydro::logWarn("EngineAPI:   could not resolve Mount fn - abort disasm");
        } else {
            uint32_t mountSz = funcSizeViaPdata(mountFn);
            Hydro::logInfo("EngineAPI:   Mount = exe+0x%zX size=%u - scanning E8 callees:",
                           (size_t)(mountFn - s_gm.base), mountSz);
            // Walk byte-by-byte for E8 / E9 (near call/jmp) within the fn body.
            // Each E8 callee is logged with its address, .pdata size, and the
            // immediately preceding 8 bytes (typical: `mov rcx, [rdi+disp]` or
            // `mov rcx, [rsi+disp]` - that disp is the FPakPlatformFile field
            // offset for the callee's `this`).
            size_t e8Cnt = 0;
            std::unordered_set<uint8_t*> seenCallees;
            for (uint32_t off = 0; off + 5 <= mountSz; off++) {
                uint8_t b = mountFn[off];
                if (b != 0xE8) continue;
                int32_t disp = *(int32_t*)(mountFn + off + 1);
                uint8_t* callee = mountFn + off + 5 + disp;
                if (callee < s_gm.base || callee >= s_gm.base + s_gm.size) continue;
                if (seenCallees.count(callee)) continue;
                seenCallees.insert(callee);
                uint32_t calleeSz = funcSizeViaPdata(callee);
                if (calleeSz == 0) continue;
                // Log the 8 bytes immediately preceding the E8 (often the
                // mov-rcx setup that loads `this`). disp32-form mov rcx is:
                //   48 8B 8F dd dd dd dd  (mov rcx,[rdi+disp32])  - 7 B
                //   48 8B 8E dd dd dd dd  (mov rcx,[rsi+disp32])  - 7 B
                //   48 8B 8B dd dd dd dd  (mov rcx,[rbx+disp32])  - 7 B
                //   48 8B 4F dd            (mov rcx,[rdi+disp8])  - 4 B
                //   48 8D 0D dd dd dd dd  (lea rcx,[rip+disp32]) - 7 B (singleton)
                char prebytes[40] = {};
                int prebytesIdx = 0;
                for (int k = 8; k >= 1; k--) {
                    if (off >= (uint32_t)k) {
                        prebytesIdx += snprintf(prebytes + prebytesIdx,
                                                sizeof(prebytes) - prebytesIdx,
                                                "%02X ", mountFn[off - k]);
                    }
                }
                Hydro::logInfo("EngineAPI:     E8[%zu] @ Mount+0x%X -> exe+0x%zX size=%u  pre: %s",
                               e8Cnt, off, (size_t)(callee - s_gm.base),
                               calleeSz, prebytes);
                e8Cnt++;
                if (e8Cnt >= 40) {
                    Hydro::logInfo("EngineAPI:     ... (capped at 40 E8 callees)");
                    break;
                }
            }
            Hydro::logInfo("EngineAPI:   Mount disasm done (%zu unique E8 callees)", e8Cnt);

            // Phase 2: Zydis-disasm each PakFile-RVA-range callee's first
            // ~25 instructions. FFilePackageStoreBackend::Mount fingerprint:
            //   * Acquires FRWLock::AcquireWriterLock early (E8 to a small fn,
            //     OR inline `lock cmpxchg`)
            //   * TArray::Add of a 24-byte FMountedContainer (calls a TArray
            //     reallocation/Emplace helper, often with hard-coded 24)
            //   * Algo::StableSort or sentinel byte-store for bNeedsContainerUpdate=1
            //   * NextSequence atomic increment (lock xadd / lock inc)
            //   * Function size ~200-400 bytes
            // Goal: identify the right callee, then read the prebytes before
            // its E8 in Mount to extract the field offset where the backend
            // pointer/object is stored on FPakPlatformFile.
            ZydisDecoder dec;
            if (ZYAN_SUCCESS(ZydisDecoderInit(&dec, ZYDIS_MACHINE_MODE_LONG_64,
                                              ZYDIS_STACK_WIDTH_64))) {
                ZydisFormatter fmt;
                ZydisFormatterInit(&fmt, ZYDIS_FORMATTER_STYLE_INTEL);
                Hydro::logInfo("EngineAPI:   PHASE 2 - disassembling PakFile-RVA "
                               "callees for Mount-shape fingerprint:");
                size_t shown = 0;
                for (auto* callee : seenCallees) {
                    uint64_t rva = (uint64_t)(callee - s_gm.base);
                    // PakFile module RVA range on this build; FPakPlatformFile
                    // sits at 0x23DE630 → siblings ~0x23B0000-0x2400000.
                    if (rva < 0x23A0000 || rva > 0x2410000) continue;
                    uint32_t sz = funcSizeViaPdata(callee);
                    if (sz == 0 || sz > 600) continue;  // tiny thunks + huge fns out
                    Hydro::logInfo("EngineAPI:     -- callee exe+0x%zX size=%u --",
                                   (size_t)rva, sz);
                    int pos = 0;
                    int instCnt = 0;
                    int maxInsts = 30;
                    bool hasLock = false;
                    bool hasAtomic = false;
                    bool hasSetByte = false;
                    while (pos < (int)sz && instCnt < maxInsts) {
                        ZydisDecodedInstruction inst;
                        ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
                        if (ZYAN_FAILED(ZydisDecoderDecodeFull(&dec, callee + pos,
                                                                sz - pos, &inst, ops))) break;
                        char buf[128] = {};
                        ZydisFormatterFormatInstruction(&fmt, &inst, ops,
                                                        inst.operand_count_visible,
                                                        buf, sizeof(buf),
                                                        (ZyanU64)(uintptr_t)(callee + pos),
                                                        nullptr);
                        Hydro::logInfo("EngineAPI:       +0x%03X  %s", pos, buf);
                        if (inst.attributes & ZYDIS_ATTRIB_HAS_LOCK) hasLock = true;
                        if (inst.mnemonic == ZYDIS_MNEMONIC_CMPXCHG ||
                            inst.mnemonic == ZYDIS_MNEMONIC_XADD) hasAtomic = true;
                        if (inst.mnemonic == ZYDIS_MNEMONIC_MOV &&
                            inst.operand_count >= 2 &&
                            ops[0].type == ZYDIS_OPERAND_TYPE_MEMORY &&
                            ops[1].type == ZYDIS_OPERAND_TYPE_IMMEDIATE &&
                            ops[1].imm.value.u == 1 &&
                            inst.operand_width == 8) hasSetByte = true;
                        pos += inst.length;
                        instCnt++;
                        if (inst.mnemonic == ZYDIS_MNEMONIC_RET) break;
                    }
                    Hydro::logInfo("EngineAPI:       +- fingerprint: lock=%d atomic=%d setByte=%d",
                                   hasLock, hasAtomic, hasSetByte);
                    if (++shown >= 8) {
                        Hydro::logInfo("EngineAPI:     (capped at 8 callees disasmed)");
                        break;
                    }
                }
            }
        }
        Hydro::logInfo("EngineAPI: FPakPlatformFile::Mount disasm - done");
    }

    // Old probe loop kept below (commented) for reference but inert.
    {
        struct ThisCandidate { uintptr_t off; const char* note; };
        const ThisCandidate cands[] = {
            { 0,    "s_assetRegImpl (UObject*) - current, AVs at +0x80" },
            { 0x08, "s_assetRegImpl + 0x08" },
            { 0x10, "s_assetRegImpl + 0x10" },
            { 0x18, "s_assetRegImpl + 0x18" },
            { 0x20, "s_assetRegImpl + 0x20" },
            { 0x28, "s_assetRegImpl + 0x28 (typical UObject base size - IAssetRegistry MI offset?)" },
            { 0x30, "s_assetRegImpl + 0x30 (= GuardedData inline)" },
            { 0x38, "s_assetRegImpl + 0x38" },
            { 0x40, "s_assetRegImpl + 0x40" },
            { 0x48, "s_assetRegImpl + 0x48 (= R11 at fault site)" },
        };
        Hydro::logInfo("EngineAPI: this-offset memory probe - for each candidate, "
                       "read 32 B at candThis+0x80 (where the AV reads); look for "
                       "populated TMap-like header (heap ptr + small counts):");
        for (const auto& c : cands) {
            uint8_t* candThis = (uint8_t*)s_assetRegImpl + c.off;
            uint8_t* probeAddr = candThis + 0x80;
            uint8_t bytes[32] = {};
            bool readOk = true;
            for (int i = 0; i < 32; i += 4) {
                int32_t v = 0;
                if (!safeReadInt32(probeAddr + i, &v)) { readOk = false; break; }
                std::memcpy(bytes + i, &v, 4);
            }
            if (!readOk) {
                Hydro::logWarn("EngineAPI:   off=0x%02zX (%s) - read failed at %p",
                               (size_t)c.off, c.note, probeAddr);
                continue;
            }
            // Print as 4 quadwords (more readable for pointer/count fields)
            uint64_t q0, q1, q2, q3;
            std::memcpy(&q0, bytes + 0,  8);
            std::memcpy(&q1, bytes + 8,  8);
            std::memcpy(&q2, bytes + 16, 8);
            std::memcpy(&q3, bytes + 24, 8);
            // Quick "looks populated" heuristic: q0 (or q2) is a heap pointer
            // (high bits set) AND q1 (next 8 bytes containing counts) is small
            // (< 0x10000000)
            bool q0_heap = (q0 > 0x7FF000000000ULL);
            bool q2_heap = (q2 > 0x7FF000000000ULL);
            bool q0_small = (q0 < 0x10000000ULL);
            const char* tag = "";
            if ((q0_heap && q1 < 0x100000000ULL) ||
                (q2_heap && q3 < 0x100000000ULL))
                tag = " [LOOKS LIKE POPULATED TMAP ✓]";
            else if (q0 == 0 && q1 == 0 && q2 == 0 && q3 == 0)
                tag = " [all zeros - empty/uninit]";
            else if (q0_small && q1 == 0 && q2 == 0 && q3 == 0)
                tag = " [small int + zeros - possibly inline TArray]";
            Hydro::logInfo("EngineAPI:   off=0x%02zX (%s)",
                           (size_t)c.off, c.note);
            Hydro::logInfo("EngineAPI:     candThis=%p, probe @ %p:",
                           candThis, probeAddr);
            Hydro::logInfo("EngineAPI:     q[0..3] = 0x%016llX 0x%016llX 0x%016llX 0x%016llX%s",
                           (unsigned long long)q0, (unsigned long long)q1,
                           (unsigned long long)q2, (unsigned long long)q3, tag);
        }
        Hydro::logInfo("EngineAPI: this-offset memory probe - done. "
                       "Next session will only call AppendState with the 'LOOKS LIKE POPULATED TMAP' winner.");
    }

    // -- FPackageStore live-state inspection (DIRECTIVE 2026-05-10) -------
    //
    // Before any deeper FPackageStore work, verify the bug is actually IN
    // FPackageStore. Find the live singleton, walk its Backends, walk each
    // backend's MountedContainers, and check whether our pak's package(s)
    // are already registered. If yes → engine auto-mounted Layer B and
    // the bug is downstream (StaticLoadObject path). If no → original
    // Layer-B-skip theory stands.
    //
    // Discovery: broadened lea-scan. Prior 8-byte Meyers-leaf scan failed
    // because UE 5.6 PGO inlined `FPackageStore::Get()` into every caller.
    // But every inlined call site still emits `lea rax, [rip+&Instance]`.
    // Tally those across the binary, filter by FPackageStore data shape,
    // pick the most-referenced match.
    static bool s_psLiveInspected = false;
    if (!s_psLiveInspected) {
        s_psLiveInspected = true;
        Hydro::logInfo("EngineAPI: FPackageStore live inspection -");

        // Phase A: tally every `lea reg, [rip+disp32]` target. REX.W + 8D + ModRM
        // where ModRM.rm == 5 means RIP-relative. Reg can be RAX/RCX/RDX/RBX
        // (`05/0D/15/1D` for non-extended, `05/0D/15/1D` with REX.R bit 4C
        // prefix for R8/R9/R10/R11 → `05/0D/15/1D` with `4C 8D ...`).
        std::unordered_map<uintptr_t, int> targetTally;
        auto tallyLea = [&](uint8_t* p) {
            int32_t disp = *(int32_t*)(p + 3);
            uintptr_t target = (uintptr_t)(p + 7) + disp;
            if (target < (uintptr_t)s_gm.base) return;
            if (target + 32 > (uintptr_t)(s_gm.base + s_gm.size)) return;
            targetTally[target]++;
        };
        for (uint8_t* p = s_gm.base; p + 7 < s_gm.base + s_gm.size; p++) {
            // `48 8D` = REX.W lea. ModRM byte at p[2]: top 3 bits = mod=00,
            // mid 3 bits = reg, low 3 bits = rm=101 (RIP-rel) → 0x05/0D/15/1D.
            if ((p[0] == 0x48 || p[0] == 0x4C) && p[1] == 0x8D) {
                uint8_t modrm = p[2];
                if ((modrm & 0xC7) == 0x05) {  // mod=00, rm=101
                    tallyLea(p);
                }
            }
        }
        Hydro::logInfo("EngineAPI:   tallied %zu unique lea targets", targetTally.size());

        // Phase A2: also tally `mov reg, [rip+disp32]` (48/4C 8B + 05/0D/15/1D).
        // Some FPackageStore::Get inlinings might compile to mov-via-pointer-table
        // rather than lea-singleton-address.
        std::unordered_map<uintptr_t, int> derefTally;
        for (uint8_t* p = s_gm.base; p + 7 < s_gm.base + s_gm.size; p++) {
            if ((p[0] == 0x48 || p[0] == 0x4C) && p[1] == 0x8B) {
                uint8_t modrm = p[2];
                if ((modrm & 0xC7) == 0x05) {
                    int32_t disp = *(int32_t*)(p + 3);
                    uintptr_t target = (uintptr_t)(p + 7) + disp;
                    if (target < (uintptr_t)s_gm.base) continue;
                    if (target + 8 > (uintptr_t)(s_gm.base + s_gm.size)) continue;
                    derefTally[target]++;
                }
            }
        }
        Hydro::logInfo("EngineAPI:   tallied %zu unique mov-deref targets", derefTally.size());

        // Top 20 most-referenced lea targets, dumped raw - sanity check.
        // FPackageStore singleton candidate should be among heavy hitters AND
        // have heap-pointer+TArray data shape.
        {
            std::vector<std::pair<uintptr_t, int>> top(targetTally.begin(),
                                                        targetTally.end());
            std::sort(top.begin(), top.end(),
                      [](const auto& a, const auto& b){ return a.second > b.second; });
            Hydro::logInfo("EngineAPI:   top 20 lea targets (raw - visual scan for FPackageStore shape):");
            for (size_t i = 0; i < top.size() && i < 20; i++) {
                uintptr_t addr = top[i].first;
                int hits = top[i].second;
                uint64_t q[4] = {};
                bool ok = true;
                for (int j = 0; j < 4; j++) {
                    int32_t lo = 0, hi = 0;
                    if (!safeReadInt32((void*)(addr + j*8),     &lo)) { ok=false; break; }
                    if (!safeReadInt32((void*)(addr + j*8 + 4), &hi)) { ok=false; break; }
                    q[j] = ((uint64_t)(uint32_t)hi << 32) | (uint32_t)lo;
                }
                if (!ok) continue;
                Hydro::logInfo("EngineAPI:     [%2zu] @ exe+0x%08zX hits=%d  Q=%016llX %016llX %016llX %016llX",
                               i, (size_t)(addr - (uintptr_t)s_gm.base), hits,
                               (unsigned long long)q[0], (unsigned long long)q[1],
                               (unsigned long long)q[2], (unsigned long long)q[3]);
            }
        }

        // Phase B: filter by FPackageStore data shape:
        //   [+0..7]   heap ptr   (TSharedRef BackendContext.Object)
        //   [+8..15]  heap ptr   (TSharedRef BackendContext.Controller)
        //   [+16..23] heap ptr or 0 (TArray Backends.Data)
        //   [+24..27] uint32 1..16 (Num)
        //   [+28..31] uint32 ≥ Num, ≤ 32 (Max)
        // Hits ≥ 3 (FPackageStore::Get is inlined at multiple sites).
        auto isHeapPtr = [](uint64_t v) {
            if ((v >> 48) != 0) return false;
            if (v < 0x0000000100000000ULL) return false;
            if ((v & 7) != 0) return false;
            return true;
        };
        struct Cand { uintptr_t addr; int hits; uint64_t q[4]; };
        std::vector<Cand> cands;
        for (auto& kv : targetTally) {
            // No minimum hits - FPackageStore::Get inlined at only a few
            // sites (LoadPackage etc). Could have very few references.
            uint64_t q[4] = {};
            bool ok = true;
            for (int i = 0; i < 4; i++) {
                int32_t lo = 0, hi = 0;
                if (!safeReadInt32((void*)(kv.first + i*8),     &lo)) { ok=false; break; }
                if (!safeReadInt32((void*)(kv.first + i*8 + 4), &hi)) { ok=false; break; }
                q[i] = ((uint64_t)(uint32_t)hi << 32) | (uint32_t)lo;
            }
            if (!ok) continue;
            // Tight FPackageStore filter:
            //   +0 BackendContext.Object       - heap ptr, REQUIRED
            //   +8 BackendContext.RefController - heap ptr, REQUIRED, ≠ +0
            //  +16 Backends.Data               - heap ptr, REQUIRED non-null
            //                                    (engine always has ≥1 backend)
            //  +24 Backends.Num                - uint32 1..16
            //  +28 Backends.Max                - uint32 ≥ Num, ≤ 32
            // "TRUE HEAP" excludes module range AND tiny pointer-encoded
            // ints (the 0x800000000-range "TArray of int ranges" we saw on
            // first run). Real Win64 heap allocations are >= 0x100_0000_0000.
            auto isTrueHeap = [&](uint64_t v) -> bool {
                if (!isHeapPtr(v)) return false;
                if (v >= (uint64_t)s_gm.base &&
                    v <  (uint64_t)s_gm.base + s_gm.size) return false;
                if (v < 0x0000010000000000ULL) return false;
                return true;
            };
            if (!isTrueHeap(q[0])) continue;
            if (!isTrueHeap(q[1])) continue;
            if (q[1] == q[0]) continue;
            if (!isTrueHeap(q[2])) continue;
            uint32_t arrNum = (uint32_t)q[3];
            uint32_t arrMax = (uint32_t)(q[3] >> 32);
            if (arrNum < 1 || arrNum > 16) continue;
            if (arrMax < arrNum || arrMax > 32) continue;

            // FPackageStore-specific shape: BackendContext is a
            // MakeShared<>'d TSharedRef → Object and RefController allocated
            // in one block, typically 16-48 B apart. Bogus candidates from
            // unrelated tables have Q[0]/Q[1] hundreds of bytes apart.
            uint64_t diff = (q[0] > q[1]) ? (q[0] - q[1]) : (q[1] - q[0]);
            if (diff > 64) continue;

            // FPackageStore-specific liveness: Backends[0]'s obj must have a
            // valid in-module vtable. This separates real singletons from
            // shape-coincidences in static data.
            //   entry layout: int32 priority + 4 pad + TSharedRef{obj*,rc*}
            int32_t bLo = 0, bHi = 0;
            if (!safeReadInt32((void*)(q[2] + 8),  &bLo)) continue;
            if (!safeReadInt32((void*)(q[2] + 12), &bHi)) continue;
            uint64_t backendObj = ((uint64_t)(uint32_t)bHi << 32) | (uint32_t)bLo;
            if (!isTrueHeap(backendObj)) continue;
            int32_t vLo = 0, vHi = 0;
            if (!safeReadInt32((void*)backendObj, &vLo)) continue;
            if (!safeReadInt32((void*)(backendObj + 4), &vHi)) continue;
            uint64_t backendVt = ((uint64_t)(uint32_t)vHi << 32) | (uint32_t)vLo;
            if (backendVt < (uint64_t)s_gm.base) continue;
            if (backendVt >= (uint64_t)s_gm.base + s_gm.size) continue;

            Cand c; c.addr = kv.first; c.hits = kv.second;
            c.q[0]=q[0]; c.q[1]=q[1]; c.q[2]=q[2]; c.q[3]=q[3];
            cands.push_back(c);
        }
        std::sort(cands.begin(), cands.end(),
                  [](const Cand& a, const Cand& b){ return a.hits > b.hits; });
        Hydro::logInfo("EngineAPI:   data-shape candidates: %zu match(es), top 10 by hits:",
                       cands.size());
        for (size_t i = 0; i < cands.size() && i < 10; i++) {
            const auto& c = cands[i];
            Hydro::logInfo("EngineAPI:     [%zu] @ %p  hits=%d  Q[0..3]=%016llX %016llX %016llX  Num=%u Max=%u",
                           i, (void*)c.addr, c.hits,
                           (unsigned long long)c.q[0], (unsigned long long)c.q[1],
                           (unsigned long long)c.q[2],
                           (uint32_t)c.q[3], (uint32_t)(c.q[3] >> 32));
        }

        // Phase C: walk top candidate's Backends, then each backend's
        // MountedContainers. For each FMountedContainer, dump first 128 B
        // of its FIoContainerHeader and search the first 4 KB for our
        // mod's signature ("HudTemplate" wide/ascii) and "pakchunk".
        if (cands.empty()) {
            Hydro::logWarn("EngineAPI:   no FPackageStore candidate matched data shape");
        }
        // Walk top 3 candidates' Backends - don't trust hit-count-only picker.
        for (size_t ci = 0; ci < cands.size() && ci < 3; ci++) {
            const auto& top = cands[ci];
            Hydro::logInfo("EngineAPI:   -- candidate #%zu @ %p (hits=%d) --",
                           ci, (void*)top.addr, top.hits);
            uintptr_t arrData = (uintptr_t)top.q[2];
            uint32_t arrNum = (uint32_t)top.q[3];
            if (!arrData || !arrNum) {
                Hydro::logWarn("EngineAPI:   Backends array empty");
            } else {
                // TTuple<int32, TSharedRef> = 4 + 16 = 20 B padded to 24.
                const uint32_t entryStride = 24;
                for (uint32_t i = 0; i < arrNum && i < 8; i++) {
                    uintptr_t entry = arrData + i * entryStride;
                    int32_t prio = 0;
                    if (!safeReadInt32((void*)entry, &prio)) break;
                    int32_t lo = 0, hi = 0;
                    if (!safeReadInt32((void*)(entry + 8),  &lo)) break;
                    if (!safeReadInt32((void*)(entry + 12), &hi)) break;
                    uint64_t obj = ((uint64_t)(uint32_t)hi << 32) | (uint32_t)lo;
                    uint64_t vt = 0;
                    if (obj) {
                        int32_t vlo = 0, vhi = 0;
                        safeReadInt32((void*)obj, &vlo);
                        safeReadInt32((void*)(obj + 4), &vhi);
                        vt = ((uint64_t)(uint32_t)vhi << 32) | (uint32_t)vlo;
                    }
                    bool vtInModule = (vt >= (uint64_t)s_gm.base &&
                                       vt <  (uint64_t)s_gm.base + s_gm.size);
                    // Capture the first valid in-module backend for runtime
                    // mount (reverse-discovery of FPakPlatformFile uses this).
                    if (!s_filePackageStoreBackend && obj && vtInModule) {
                        s_filePackageStoreBackend = (void*)obj;
                    }
                    Hydro::logInfo("EngineAPI:   backend[%u] prio=%d obj=%016llX vt=%016llX%s",
                                   i, prio, (unsigned long long)obj,
                                   (unsigned long long)vt,
                                   vtInModule ? " [in-module]" : " [out]");
                    if (!obj || !vtInModule) continue;
                    // Find MountedContainers. Backend layout: vtable(8) +
                    // FRWLock(8) + FRWLock(8) + TArray(16) + ... → +16..+64 sweep.
                    bool found = false;
                    for (uint32_t off = 16; off <= 64 && !found; off += 8) {
                        int32_t plo=0,phi=0,clo=0,chi=0;
                        if (!safeReadInt32((void*)(obj + off),     &plo)) continue;
                        if (!safeReadInt32((void*)(obj + off + 4), &phi)) continue;
                        if (!safeReadInt32((void*)(obj + off + 8), &clo)) continue;
                        if (!safeReadInt32((void*)(obj + off + 12),&chi)) continue;
                        uint64_t mcData = ((uint64_t)(uint32_t)phi << 32) | (uint32_t)plo;
                        uint64_t mcCounts = ((uint64_t)(uint32_t)chi << 32) | (uint32_t)clo;
                        uint32_t mcNum = (uint32_t)mcCounts;
                        uint32_t mcMax = (uint32_t)(mcCounts >> 32);
                        bool ptrOk = (mcData == 0) ||
                            (mcData >= 0x0000000100000000ULL &&
                             mcData < 0x0000800000000000ULL &&
                             (mcData & 7) == 0);
                        bool numOk = (mcNum < 1000 && mcMax >= mcNum && mcMax < 1000);
                        if (!(ptrOk && numOk)) continue;
                        if (mcNum == 0) continue;
                        Hydro::logInfo("EngineAPI:     MountedContainers @ obj+0x%X "
                                       "Data=%016llX Num=%u Max=%u",
                                       off, (unsigned long long)mcData, mcNum, mcMax);
                        found = true;
                        // FMountedContainer fields total 28 B; padded to 32.
                        // Earlier 24-stride read mc[1+] as garbage. 32 lines up.
                        const uint32_t mcStride = 32;
                        for (uint32_t j = 0; j < mcNum && j < 16; j++) {
                            uintptr_t mc = mcData + j * mcStride;
                            int32_t lo2=0,hi2=0;
                            safeReadInt32((void*)mc,     &lo2);
                            safeReadInt32((void*)(mc+4), &hi2);
                            uint64_t hdrPtr = ((uint64_t)(uint32_t)hi2 << 32) | (uint32_t)lo2;
                            int32_t order=0, seq=0;
                            safeReadInt32((void*)(mc+8),  &order);
                            safeReadInt32((void*)(mc+12), &seq);
                            Hydro::logInfo("EngineAPI:       mc[%u]: hdr=%016llX order=%d seq=%d",
                                           j, (unsigned long long)hdrPtr, order, seq);
                            if (!hdrPtr) continue;
                            MEMORY_BASIC_INFORMATION mbi = {};
                            if (!VirtualQuery((void*)hdrPtr, &mbi, sizeof(mbi))) continue;
                            if (!(mbi.State & MEM_COMMIT) || mbi.RegionSize < 128) continue;
                            const uint8_t* hp = (const uint8_t*)hdrPtr;
                            // First 256 B of FIoContainerHeader. UE 5.6 layout:
                            //   +0   ContainerId (uint64)
                            //   +8   ... metadata
                            //   ...  several inline TArray<FPackageId> /
                            //        TArray<FFilePackageStoreEntry> headers.
                            // TArray header = {Data*(8), Num(4), Max(4)}; visible
                            // as heap_ptr + small_uint_pair on 16-byte boundaries.
                            for (size_t row = 0; row < 256; row += 16) {
                                Hydro::logInfo("EngineAPI:         [+0x%02zX] "
                                               "%02X %02X %02X %02X %02X %02X %02X %02X "
                                               "%02X %02X %02X %02X %02X %02X %02X %02X",
                                               row,
                                               hp[row+0],hp[row+1],hp[row+2],hp[row+3],
                                               hp[row+4],hp[row+5],hp[row+6],hp[row+7],
                                               hp[row+8],hp[row+9],hp[row+10],hp[row+11],
                                               hp[row+12],hp[row+13],hp[row+14],hp[row+15]);
                            }
                            // Signatures we hunt for: our mod path (ASCII/WIDE)
                            // and base-game name (to identify what each mc is).
                            struct SigCount { int A; int W; const char* asc; const char* wide; size_t aLen; size_t wLen; };
                            SigCount sigs[] = {
                                {0,0, "HudTemplate", "H\0u\0d\0T\0e\0m\0p\0l\0a\0t\0e\0", 11, 22},
                                {0,0, "Sphere", "S\0p\0h\0e\0r\0e\0", 6, 12},
                                {0,0, "DummyModdableGame", "D\0u\0m\0m\0y\0M\0o\0d\0d\0a\0b\0l\0e\0G\0a\0m\0e\0", 17, 34},
                                {0,0, "/Game/Mods", "/\0G\0a\0m\0e\0/\0M\0o\0d\0s\0", 10, 20},
                                {0,0, "pakchunk", "p\0a\0k\0c\0h\0u\0n\0k\0", 8, 16},
                            };
                            constexpr size_t kSigCount = sizeof(sigs) / sizeof(sigs[0]);
                            auto scanRange = [&](const uint8_t* base, size_t len, const char* tag) {
                                for (size_t k = 0; k < len; k++) {
                                    for (size_t s = 0; s < kSigCount; s++) {
                                        if (k + sigs[s].aLen <= len &&
                                            memcmp(base + k, sigs[s].asc, sigs[s].aLen) == 0) {
                                            if (sigs[s].A == 0) {
                                                Hydro::logInfo("EngineAPI:         FOUND ASCII '%s' in %s @ +0x%zX",
                                                               sigs[s].asc, tag, k);
                                            }
                                            sigs[s].A++;
                                        }
                                        if (k + sigs[s].wLen <= len &&
                                            memcmp(base + k, sigs[s].wide, sigs[s].wLen) == 0) {
                                            if (sigs[s].W == 0) {
                                                Hydro::logInfo("EngineAPI:         FOUND WIDE  '%s' in %s @ +0x%zX",
                                                               sigs[s].asc, tag, k);
                                            }
                                            sigs[s].W++;
                                        }
                                    }
                                }
                            };
                            // Scan inline header (first 64 KB or region end).
                            size_t scan = (mbi.RegionSize < 65536) ? mbi.RegionSize : 65536;
                            uintptr_t regionEnd = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
                            if ((uintptr_t)hp + scan > regionEnd) scan = regionEnd - (uintptr_t)hp;
                            scanRange(hp, scan, "header-inline");
                            // Walk first 256 B for TArray-shape headers, then
                            // chase each .Data into its heap allocation and scan
                            // up to 64 KB there. Strings (FString.Data) live in
                            // these heap regions, not in the inline header.
                            int chasedArrays = 0;
                            for (size_t off = 0; off + 16 <= 256 && off + 16 <= scan; off += 8) {
                                uint64_t dataPtr = 0; uint32_t aNum = 0, aMax = 0;
                                std::memcpy(&dataPtr, hp + off, 8);
                                std::memcpy(&aNum,    hp + off + 8, 4);
                                std::memcpy(&aMax,    hp + off + 12, 4);
                                bool ptrLooksOk = (dataPtr >= 0x0000010000000000ULL &&
                                                   dataPtr <  0x0000800000000000ULL &&
                                                   (dataPtr & 7) == 0);
                                bool numLooksOk = (aNum > 0 && aNum < 100000 &&
                                                   aMax >= aNum && aMax < 200000);
                                if (!ptrLooksOk || !numLooksOk) continue;
                                MEMORY_BASIC_INFORMATION m2 = {};
                                if (!VirtualQuery((void*)dataPtr, &m2, sizeof(m2))) continue;
                                if (!(m2.State & MEM_COMMIT)) continue;
                                size_t avail = ((uintptr_t)m2.BaseAddress + m2.RegionSize)
                                               - (uintptr_t)dataPtr;
                                size_t scan2 = (avail < 65536) ? avail : 65536;
                                Hydro::logInfo("EngineAPI:         hdr+0x%02zX: TArray Data=%p Num=%u Max=%u (scan %zu B)",
                                               off, (void*)dataPtr, aNum, aMax, scan2);
                                char tag[32]; snprintf(tag, sizeof(tag), "TArray@hdr+0x%zX", off);
                                scanRange((const uint8_t*)dataPtr, scan2, tag);
                                if (++chasedArrays >= 8) break;
                            }
                            // Final tally per signature.
                            for (size_t s = 0; s < kSigCount; s++) {
                                if (sigs[s].A || sigs[s].W) {
                                    Hydro::logInfo("EngineAPI:         signature '%s': ASCII=%d WIDE=%d",
                                                   sigs[s].asc, sigs[s].A, sigs[s].W);
                                }
                            }
                            bool anySig = false;
                            for (auto& s : sigs) if (s.A || s.W) { anySig = true; break; }
                            if (!anySig) {
                                Hydro::logInfo("EngineAPI:         (no known signatures matched)");
                            }
                        }
                    }
                    if (!found) {
                        Hydro::logInfo("EngineAPI:     no MountedContainers TArray "
                                       "found at obj+16..+64 - backend may use "
                                       "different layout or have 0 containers");
                    }
                }
            }
        }
        Hydro::logInfo("EngineAPI: FPackageStore live inspection - done");
    }

    // -- FPakPlatformFile reverse-discovery via backend pointer ---------
    //
    // Engine auto-mount worked: our pak is in FFilePackageStoreBackend.
    // MountedContainers (verified by ContainerId match). PakLoader's
    // L"PakFile" anchor picked a wrong vtable on this build (instance scan
    // returned 0 candidates), so to call FPakPlatformFile::Mount at runtime
    // we need its instance via a different route.
    //
    // FPakPlatformFile owns the FFilePackageStoreBackend (TUniquePtr<>
    // member). Any heap allocation containing the backend pointer is
    // presumptively inside FPakPlatformFile. The first qword of that
    // allocation is its real vtable.
    //
    // Algorithm:
    //   1. Scan all writable, committed process memory for qword == backend.
    //   2. For each hit at heap address P, look at the start of the
    //      containing object: walk back through 8-byte qwords looking for
    //      the vtable (first qword in module .rdata range, with valid
    //      function pointers in the next 5+ slots).
    //   3. If found, P - walkback = FPakPlatformFile* and *FPakPlatformFile*
    //      = its vtable.
    //   4. Validate by checking vtable's first slot (deleting destructor)
    //      points to a real function and the next ~10 slots also do.
    if (s_filePackageStoreBackend && !s_fpakPlatformFile) {
        Hydro::logInfo("EngineAPI: FPakPlatformFile reverse-discovery - backend=%p",
                       s_filePackageStoreBackend);

        // FPakPlatformFile owns the backend via a TUniquePtr<> member.
        // Both are heap-allocated by UE's allocator, typically clustered
        // in the same general heap address range but sometimes in
        // separate VirtualAlloc regions. Strategy:
        //   1. Locate the backend's region (cheap VirtualQuery).
        //   2. Scan all writable PRIVATE+COMMIT regions whose base
        //      addresses sit within ±1 GB of the backend region.
        //      That keeps scan tight (~tens of MB total) and excludes
        //      thread stacks (low addresses, far from heap arenas) and
        //      file mappings (mapped, not private).
        //   3. Skip any region > 256 MB (probably a memory-mapped file
        //      or arena, unlikely to contain a 1-3 KB FPakPlatformFile).
        uint64_t needle = (uint64_t)s_filePackageStoreBackend;
        MEMORY_BASIC_INFORMATION backendMbi = {};
        if (!VirtualQuery((void*)needle, &backendMbi, sizeof(backendMbi)) ||
            !(backendMbi.State & MEM_COMMIT)) {
            Hydro::logWarn("EngineAPI:   backend ptr %p not in committed memory",
                           (void*)needle);
            return false;
        }
        uintptr_t backendRegion = (uintptr_t)backendMbi.BaseAddress;
        constexpr uintptr_t kNeighborhood = 0x40000000ull; // ±1 GB
        constexpr size_t kMaxRegionBytes = 0x10000000ull;  // 256 MB cap per region
        uintptr_t scanLo = (backendRegion > kNeighborhood) ? (backendRegion - kNeighborhood) : 0;
        uintptr_t scanHi = backendRegion + kNeighborhood;
        Hydro::logInfo("EngineAPI:   scoped scan in [%p..%p] (±1 GB around backend region %p)",
                       (void*)scanLo, (void*)scanHi, (void*)backendRegion);
        uint64_t startTick = GetTickCount64();
        struct Hit { uintptr_t addr; uintptr_t regionBase; size_t regionSize; };
        std::vector<Hit> hits;
        int regionsScanned = 0;
        size_t bytesScanned = 0;
        {
            uint8_t* addr2 = (uint8_t*)scanLo;
            uint8_t* maxAddr = (uint8_t*)scanHi;
            while (addr2 < maxAddr) {
                MEMORY_BASIC_INFORMATION mbi;
                if (VirtualQuery(addr2, &mbi, sizeof(mbi)) == 0) break;
                if (mbi.State == MEM_COMMIT &&
                    mbi.Type == MEM_PRIVATE &&  // exclude MEM_MAPPED + MEM_IMAGE
                    mbi.RegionSize <= kMaxRegionBytes) {
                    DWORD prot = mbi.Protect & 0xFF;
                    bool isWritable =
                        (prot == PAGE_READWRITE) || (prot == PAGE_WRITECOPY) ||
                        (prot == PAGE_EXECUTE_READWRITE) || (prot == PAGE_EXECUTE_WRITECOPY);
                    if (isWritable && !(mbi.Protect & PAGE_GUARD)) {
                        regionsScanned++;
                        bytesScanned += mbi.RegionSize;
                        uint8_t* p = (uint8_t*)mbi.BaseAddress;
                        uint8_t* end = p + mbi.RegionSize;
                        for (; p + 8 <= end; p += 8) {
                            if (*(uint64_t*)p == needle) {
                                hits.push_back({(uintptr_t)p,
                                                (uintptr_t)mbi.BaseAddress,
                                                mbi.RegionSize});
                                if (hits.size() >= 32) break;
                            }
                        }
                    }
                }
                uint8_t* prev = addr2;
                size_t advance = mbi.RegionSize ? mbi.RegionSize : 0x1000;
                addr2 = (uint8_t*)mbi.BaseAddress + advance;
                if (addr2 <= prev) break;
                if (hits.size() >= 32) break;
            }
        }
        uint64_t scanMs = GetTickCount64() - startTick;
        Hydro::logInfo("EngineAPI:   found %zu hit(s) across %d region(s), %.1f MB scanned, %llu ms",
                       hits.size(), regionsScanned, bytesScanned / 1048576.0,
                       (unsigned long long)scanMs);

        // For each hit, walk back to find a candidate object start. An
        // FPakPlatformFile-shape allocation has its vtable at offset 0
        // (a module .rdata pointer) and the backend at some larger offset.
        // We try walkback offsets in 8-byte stride and stop at the FIRST
        // qword that's a function pointer in module .rdata range AND whose
        // next 8 slots are also fn pointers (a real vtable).
        auto isInModule = [&](uint64_t v) -> bool {
            return v >= (uint64_t)s_gm.base &&
                   v <  (uint64_t)s_gm.base + s_gm.size;
        };
        // Looser: require at least 4 of the first 8 slots to be valid in-module
        // function pointers (via .pdata). Goal is to filter out random data
        // that happens to have a single in-module qword, while accepting
        // real vtables even if some slots are pure-virtual stubs or unused.
        auto looksLikeVtable = [&](uintptr_t vtAddr) -> bool {
            if (!isInModule((uint64_t)vtAddr)) return false;
            int valid = 0;
            for (int i = 0; i < 8; i++) {
                int32_t lo = 0, hi = 0;
                if (!safeReadInt32((void*)(vtAddr + i*8), &lo)) continue;
                if (!safeReadInt32((void*)(vtAddr + i*8 + 4), &hi)) continue;
                uint64_t slot = ((uint64_t)(uint32_t)hi << 32) | (uint32_t)lo;
                if (!isInModule(slot)) continue;
                if (!funcStartViaPdata((uint8_t*)slot)) continue;
                valid++;
            }
            return valid >= 4;
        };
        size_t candCount = 0;
        for (const auto& h : hits) {
            // Walk back looking for the object start. FPakPlatformFile is a
            // very large class on UE 5.6 (TArray<FPakFile>, multiple member
            // lists, locks, etc). Backend pointer could be ~kilobyte deep.
            // Search up to 0x2000 (8 KB) walkback, log first valid match.
            Hydro::logInfo("EngineAPI:   hit @ %p region=[%p..+0x%zX]",
                           (void*)h.addr, (void*)h.regionBase, h.regionSize);
            bool foundForThisHit = false;
            for (uintptr_t back = 8; back <= 0x2000 && !foundForThisHit; back += 8) {
                uintptr_t candStart = h.addr - back;
                if (candStart < h.regionBase) break;
                int32_t lo = 0, hi = 0;
                if (!safeReadInt32((void*)candStart,     &lo)) continue;
                if (!safeReadInt32((void*)(candStart + 4), &hi)) continue;
                uint64_t maybeVt = ((uint64_t)(uint32_t)hi << 32) | (uint32_t)lo;
                if (!looksLikeVtable((uintptr_t)maybeVt)) continue;
                // Found a candidate FPakPlatformFile-shape object.
                Hydro::logInfo("EngineAPI:   candidate FPakPlatformFile @ %p  vtable=%016llX  (backend at obj+0x%zX)",
                               (void*)candStart, (unsigned long long)maybeVt,
                               (size_t)back);
                candCount++;
                foundForThisHit = true;
                if (!s_fpakPlatformFile) {
                    s_fpakPlatformFile = (void*)candStart;
                    s_fpakPlatformFileVtable = (void*)(uintptr_t)maybeVt;
                    Hydro::logInfo("EngineAPI:   ✓ adopted FPakPlatformFile=%p vtable=%p (backend field offset 0x%zX)",
                                   s_fpakPlatformFile, s_fpakPlatformFileVtable, (size_t)back);
                }
            }
            if (candCount >= 4) break;
        }
        if (!s_fpakPlatformFile) {
            Hydro::logWarn("EngineAPI:   reverse-discovery FAILED - no qualifying object shape found");
        }
        Hydro::logInfo("EngineAPI: FPakPlatformFile reverse-discovery - done");
    }

    // -- Runtime pak-mount test trigger ----------------------------------
    //
    // Env var HYDRO_RUNTIME_MOUNT_TEST=<absolute pak path> triggers a
    // one-shot Mount call against our reverse-discovered FPakPlatformFile.
    // If HYDRO_RUNTIME_MOUNT_AR_BIN=<absolute AR.bin path> is also set,
    // run AppendState for that AR.bin afterward. Logs every step.
    //
    // Test workflow:
    //   1. Empty Content/Paks/<sub>/ of mod paks at game launch.
    //   2. Set HYDRO_RUNTIME_MOUNT_TEST=<pak path> in launch.bat.
    //   3. Game starts WITHOUT mod content registered.
    //   4. AppendState bridge fires at tick 120 → reverse-discovery runs
    //      → this block calls mountPakAtRuntime once → pak goes into
    //      FPackageStore.MountedContainers post-mount → Assets.load works.
    static bool s_runtimeMountTestDone = false;
    if (!s_runtimeMountTestDone && s_fpakPlatformFile) {
        s_runtimeMountTestDone = true;
        char pakBuf[1024] = {};
        char arBinBuf[1024] = {};
        DWORD pakLen = GetEnvironmentVariableA(
            "HYDRO_RUNTIME_MOUNT_TEST", pakBuf, sizeof(pakBuf));
        DWORD arLen = GetEnvironmentVariableA(
            "HYDRO_RUNTIME_MOUNT_AR_BIN", arBinBuf, sizeof(arBinBuf));
        if (pakLen > 0 && pakLen < sizeof(pakBuf)) {
            Hydro::logInfo("EngineAPI: HYDRO_RUNTIME_MOUNT_TEST set - attempting "
                           "runtime mount of '%s'", pakBuf);
            // Convert to wide
            std::wstring wpak;
            for (DWORD i = 0; i < pakLen; i++) wpak.push_back((wchar_t)pakBuf[i]);
            std::wstring war;
            if (arLen > 0 && arLen < sizeof(arBinBuf)) {
                for (DWORD i = 0; i < arLen; i++) war.push_back((wchar_t)arBinBuf[i]);
            }
            bool ok = mountPakAtRuntime(wpak.c_str(),
                                        war.empty() ? nullptr : war.c_str(),
                                        1000);
            Hydro::logInfo("EngineAPI: runtime mount test - result=%s", ok ? "OK" : "FAIL");
        }
    }

    // (Process-wide pak-data hunt removed - was a 15s game-thread freeze
    // diagnostic. We've since proven via FPackageStore inspection +
    // ContainerId match that our pak DOES register in MountedContainers
    // post-mount; no need to scan all writable memory for our content
    // signatures any more.)
#if 0
    static bool s_paktagHunted = false;
    if (!s_paktagHunted) {
        s_paktagHunted = true;
        Hydro::logInfo("EngineAPI: process-wide hunt for our pak's data -");
        const uint8_t hudTplWide[]   = { 'H',0,'u',0,'d',0,'T',0,'e',0,
                                         'm',0,'p',0,'l',0,'a',0,'t',0,'e',0 };
        const uint8_t pakChunkWide[] = { 'p',0,'a',0,'k',0,'c',0,'h',0,
                                         'u',0,'n',0,'k',0,'7',0,'4',0,
                                         '1',0,'8',0,'3',0 };
        const uint8_t hudTplAscii[]  = { 'H','u','d','T','e','m','p','l','a','t','e' };
        const uint8_t modPathWide[]  = { '/',0,'G',0,'a',0,'m',0,'e',0,
                                         '/',0,'M',0,'o',0,'d',0,'s',0,
                                         '/',0,'H',0,'u',0,'d',0 };
        struct Pat { const uint8_t* data; size_t len; const char* name; int hits; };
        Pat pats[] = {
            { hudTplWide,    sizeof(hudTplWide),    "HudTemplate (wide)",      0 },
            { pakChunkWide,  sizeof(pakChunkWide),  "pakchunk74183 (wide)",    0 },
            { hudTplAscii,   sizeof(hudTplAscii),   "HudTemplate (ascii)",     0 },
            { modPathWide,   sizeof(modPathWide),   "/Game/Mods/Hud (wide)",   0 },
        };
        constexpr size_t kPats = sizeof(pats) / sizeof(pats[0]);

        SYSTEM_INFO sysInfo; GetSystemInfo(&sysInfo);
        uint8_t* addr = (uint8_t*)sysInfo.lpMinimumApplicationAddress;
        uint8_t* maxAddr = (uint8_t*)sysInfo.lpMaximumApplicationAddress;
        uint64_t startTick = GetTickCount64();
        constexpr uint64_t kMaxScanMs = 15000;
        constexpr size_t kMaxRegionBytes = 0x40000000;
        int regionsScanned = 0;
        int regionsSkippedHuge = 0;
        struct Hit { uintptr_t addr; uintptr_t regionBase; size_t regionSize; DWORD prot; };
        std::vector<std::vector<Hit>> hitsByPat(kPats);
        while (addr < maxAddr) {
            uint64_t now = GetTickCount64();
            if (now - startTick > kMaxScanMs) {
                Hydro::logWarn("EngineAPI:   scan TIMED OUT at %llu ms (regions=%d)",
                               (unsigned long long)(now - startTick), regionsScanned);
                break;
            }
            MEMORY_BASIC_INFORMATION mbi;
            if (VirtualQuery(addr, &mbi, sizeof(mbi)) == 0) break;
            if (mbi.State == MEM_COMMIT) {
                DWORD prot = mbi.Protect & 0xFF;
                bool readable =
                    (prot == PAGE_READONLY) || (prot == PAGE_READWRITE) ||
                    (prot == PAGE_WRITECOPY) ||
                    (prot == PAGE_EXECUTE_READ) || (prot == PAGE_EXECUTE_READWRITE) ||
                    (prot == PAGE_EXECUTE_WRITECOPY);
                bool isWritable =
                    (prot == PAGE_READWRITE) || (prot == PAGE_WRITECOPY) ||
                    (prot == PAGE_EXECUTE_READWRITE) || (prot == PAGE_EXECUTE_WRITECOPY);
                if (readable && isWritable && !(mbi.Protect & PAGE_GUARD)) {
                    if (mbi.RegionSize > kMaxRegionBytes) {
                        regionsSkippedHuge++;
                    } else {
                        regionsScanned++;
                        uint8_t* p   = (uint8_t*)mbi.BaseAddress;
                        uint8_t* end = p + mbi.RegionSize;
                        uint8_t* lastTick = p;
                        for (; p < end; p++) {
                            if (p - lastTick > 0x10000) {
                                uint64_t t = GetTickCount64();
                                if (t - startTick > kMaxScanMs) break;
                                lastTick = p;
                            }
                            for (size_t i = 0; i < kPats; i++) {
                                if (p + pats[i].len > end) continue;
                                if (hitsByPat[i].size() >= 16) continue; // cap
                                if (memcmp(p, pats[i].data, pats[i].len) == 0) {
                                    Hit h{ (uintptr_t)p, (uintptr_t)mbi.BaseAddress,
                                            mbi.RegionSize, mbi.Protect };
                                    hitsByPat[i].push_back(h);
                                    pats[i].hits++;
                                }
                            }
                        }
                    }
                }
            }
            uint8_t* prev = addr;
            size_t adv = mbi.RegionSize ? mbi.RegionSize : 0x1000;
            addr = (uint8_t*)mbi.BaseAddress + adv;
            if (addr <= prev) break;
        }
        // Heuristic: a region is "stack" if it's small (<= 8 MB), private,
        // and address is in the 0x000000xx_xxxxxxxx range typical of
        // thread stack reservations. Heap allocations live in 0x000001xx
        // and above (private+commit). Module data is in s_gm.base range.
        auto classifyRegion = [&](uintptr_t base, size_t size) -> const char* {
            if (base >= (uintptr_t)s_gm.base &&
                base <  (uintptr_t)s_gm.base + s_gm.size) return "module";
            if (base < 0x0000010000000000ULL && size <= 0x800000) return "stack?";
            return "heap";
        };
        for (size_t i = 0; i < kPats; i++) {
            const auto& hits = hitsByPat[i];
            if (hits.empty()) {
                Hydro::logInfo("EngineAPI:   ✗ '%s': NOT FOUND",
                               pats[i].name);
                continue;
            }
            Hydro::logInfo("EngineAPI:   ✓ '%s': %zu hit(s):",
                           pats[i].name, hits.size());
            for (size_t h = 0; h < hits.size() && h < 8; h++) {
                const auto& hit = hits[h];
                const char* cls = classifyRegion(hit.regionBase, hit.regionSize);
                Hydro::logInfo("EngineAPI:     [%zu] @ %p  region=%p sz=0x%zX prot=0x%X (%s)",
                               h, (void*)hit.addr,
                               (void*)hit.regionBase, hit.regionSize,
                               hit.prot, cls);
                // 64 B context for first heap hit only (most informative).
                if (h == 0 || (h > 0 && std::strcmp(cls, "heap") == 0 &&
                               std::strcmp(classifyRegion(hits[0].regionBase, hits[0].regionSize), "heap") != 0)) {
                    uintptr_t ctxStart = hit.addr >= 32 ? hit.addr - 32 : hit.addr;
                    if (ctxStart < hit.regionBase) ctxStart = hit.regionBase;
                    const uint8_t* cp = (const uint8_t*)ctxStart;
                    char hexbuf[200] = {};
                    int idx = 0;
                    for (size_t k = 0; k < 64; k++) {
                        idx += snprintf(hexbuf + idx, sizeof(hexbuf) - idx,
                                        "%02X ", cp[k]);
                        if (k == 31) idx += snprintf(hexbuf + idx,
                                                     sizeof(hexbuf) - idx, "| ");
                    }
                    Hydro::logInfo("EngineAPI:        ctx @ %p..+64: %s",
                                   (void*)ctxStart, hexbuf);
                }
            }
        }
        Hydro::logInfo("EngineAPI: process-wide hunt - %d region(s) scanned, "
                       "%d skipped (>1GB), done",
                       regionsScanned, regionsSkippedHuge);
    }
#endif

    if (!appendOk) {
        DWORD64 rva = 0;
        if (s_lastAppendFaultAddr >= (DWORD64)s_gm.base &&
            s_lastAppendFaultAddr <  (DWORD64)s_gm.base + s_gm.size) {
            rva = s_lastAppendFaultAddr - (DWORD64)s_gm.base;
        }
        Hydro::logWarn("EngineAPI: AppendState path - UAR::AppendState SEH-faulted "
                       "code=0x%08X fault@exe+0x%llX raw=0x%llX",
                       s_lastAppendFaultCode,
                       (unsigned long long)rva,
                       (unsigned long long)s_lastAppendFaultAddr);
        // Register dump at fault. The fault instruction is `mov r10d, [r9+rcx*4]`
        // - so r9 is the base of some uint32 array, rcx is an index. We want to
        // know:  what is r9 pointing at (live AR's hash table? tempState's?),
        // is rcx absurdly large (out-of-range index), what's the array size?
        Hydro::logWarn("EngineAPI:   regs at fault:");
        Hydro::logWarn("EngineAPI:     RAX=%016llX RCX=%016llX RDX=%016llX RBX=%016llX",
                       s_lastAppendFaultRegs[0], s_lastAppendFaultRegs[1],
                       s_lastAppendFaultRegs[2], s_lastAppendFaultRegs[3]);
        Hydro::logWarn("EngineAPI:     RSP=%016llX RBP=%016llX RSI=%016llX RDI=%016llX",
                       s_lastAppendFaultRegs[4], s_lastAppendFaultRegs[5],
                       s_lastAppendFaultRegs[6], s_lastAppendFaultRegs[7]);
        Hydro::logWarn("EngineAPI:     R8 =%016llX R9 =%016llX R10=%016llX R11=%016llX",
                       s_lastAppendFaultRegs[8], s_lastAppendFaultRegs[9],
                       s_lastAppendFaultRegs[10], s_lastAppendFaultRegs[11]);
        Hydro::logWarn("EngineAPI:     R12=%016llX R13=%016llX R14=%016llX R15=%016llX",
                       s_lastAppendFaultRegs[12], s_lastAppendFaultRegs[13],
                       s_lastAppendFaultRegs[14], s_lastAppendFaultRegs[15]);
        // Decode the fault: `mov r10d, [r9+rcx*4]`. The faulting address is
        // r9 + rcx*4. Compute and classify.
        DWORD64 faultAddr = s_lastAppendFaultRegs[9] + s_lastAppendFaultRegs[1] * 4;
        const char* classify = "unknown region";
        if (s_assetRegImpl &&
            faultAddr >= (DWORD64)s_assetRegImpl &&
            faultAddr <  (DWORD64)s_assetRegImpl + 0x10000) {
            classify = "INSIDE live AR (s_assetRegImpl ± 64 KB)";
        } else if (faultAddr >= (DWORD64)tempState &&
                   faultAddr <  (DWORD64)tempState + sizeof(tempState)) {
            classify = "INSIDE tempState buffer";
        } else if (faultAddr < 0x10000) {
            classify = "NULL-page region (likely null deref)";
        } else if (faultAddr >= 0x7FFF00000000ULL) {
            classify = "kernel/system memory (likely garbage pointer)";
        } else if (s_gm.base &&
                   faultAddr >= (DWORD64)s_gm.base &&
                   faultAddr <  (DWORD64)s_gm.base + s_gm.size) {
            classify = "inside main module (.rdata/.text)";
        }
        Hydro::logWarn("EngineAPI:   fault load addr = R9 + RCX*4 = 0x%016llX (%s)",
                       (unsigned long long)faultAddr, classify);
        // Sanity-check: if r9 IS readable, dump first 64 bytes of the array
        // it points at - gives us a glimpse of what TMap (or other) we hit.
        DWORD64 r9 = s_lastAppendFaultRegs[9];
        if (r9 >= 0x10000 && r9 < 0x7FFF00000000ULL) {
            MEMORY_BASIC_INFORMATION mbi = {};
            if (VirtualQuery((void*)r9, &mbi, sizeof(mbi)) &&
                (mbi.State & MEM_COMMIT) && mbi.RegionSize >= 64) {
                uint8_t* p = (uint8_t*)r9;
                Hydro::logWarn("EngineAPI:   r9 base @ 0x%016llX first 64 B:", (unsigned long long)r9);
                for (size_t row = 0; row < 64; row += 16) {
                    Hydro::logWarn("EngineAPI:     [+0x%02zX] "
                                   "%02X %02X %02X %02X %02X %02X %02X %02X "
                                   "%02X %02X %02X %02X %02X %02X %02X %02X",
                                   row,
                                   p[row+0],p[row+1],p[row+2],p[row+3],
                                   p[row+4],p[row+5],p[row+6],p[row+7],
                                   p[row+8],p[row+9],p[row+10],p[row+11],
                                   p[row+12],p[row+13],p[row+14],p[row+15]);
                }
            } else {
                Hydro::logWarn("EngineAPI:   r9=0x%016llX is unreadable (VirtualQuery)",
                               (unsigned long long)r9);
            }
        }
        // Dump the fault instruction (16 bytes) and identify the containing
        // function via .pdata.
        if (rva && s_gm.base + rva + 16 <= s_gm.base + s_gm.size) {
            uint8_t* p = s_gm.base + rva;
            Hydro::logWarn("EngineAPI:   fault instruction: "
                           "%02X %02X %02X %02X %02X %02X %02X %02X "
                           "%02X %02X %02X %02X %02X %02X %02X %02X",
                           p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7],
                           p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15]);
            uint8_t* fnEntry = funcStartViaPdata(p);
            if (fnEntry) {
                uint32_t fnSize = funcSizeViaPdata(fnEntry);
                Hydro::logWarn("EngineAPI:   fault inside fn exe+0x%zX (size=%u, "
                               "fault is at byte +%zd)",
                               (size_t)(fnEntry - s_gm.base), fnSize,
                               p - fnEntry);
            }
        }
        // Dump tempState structure further (256 bytes) so we can correlate
        // which TMap/TArray field the fault tried to read.
        Hydro::logWarn("EngineAPI:   tempState[+0x10..0xFF] hex:");
        for (size_t row = 0x10; row < 0x100; row += 16) {
            Hydro::logWarn("EngineAPI:     [+0x%02zX]: "
                           "%02X %02X %02X %02X %02X %02X %02X %02X "
                           "%02X %02X %02X %02X %02X %02X %02X %02X",
                           row,
                           tempState[row+0], tempState[row+1], tempState[row+2], tempState[row+3],
                           tempState[row+4], tempState[row+5], tempState[row+6], tempState[row+7],
                           tempState[row+8], tempState[row+9], tempState[row+10], tempState[row+11],
                           tempState[row+12], tempState[row+13], tempState[row+14], tempState[row+15]);
        }
        return false;
    }
    Hydro::logInfo("EngineAPI: AppendState path - UAR::AppendState returned successfully");
    // NOTE: We intentionally skip ~FAssetRegistryState here. The temp state's
    // heap-allocated FAssetData entries leak (bounded; ~tens of KB per merge).
    // Acceptable for first-run validation; wire up the dtor in a follow-up.
    return true;
}

bool loadAssetRegistryBin(const wchar_t* arBinPath) {
    if (!arBinPath || !*arBinPath) return false;
    if (!s_assetRegImpl) {
        Hydro::logWarn("EngineAPI: loadAssetRegistryBin: AR singleton not discovered");
        return false;
    }

    // Primary path: AppendState bridge. Avoids the
    // `Can only load into empty asset registry states` fatal that
    // UAR::Serialize hits on a populated AR. Activates only when the
    // HYDRO_AR_LOADFROMDISK_RVA + HYDRO_AR_APPENDSTATE_RVA env vars are
    // set (until we wire up proper resolvers).
    if (tryMergeViaAppendState(arBinPath)) return true;

    // Read AR.bin into memory. Use a wide-path fopen so non-ASCII install
    // paths work.
    FILE* f = nullptr;
    if (_wfopen_s(&f, arBinPath, L"rb") != 0 || !f) {
        Hydro::logWarn("EngineAPI: loadAssetRegistryBin: couldn't open %ls", arBinPath);
        return false;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) {
        fclose(f);
        Hydro::logWarn("EngineAPI: loadAssetRegistryBin: %ls is empty", arBinPath);
        return false;
    }
    std::vector<uint8_t> bytes((size_t)sz);
    size_t read = fread(bytes.data(), 1, (size_t)sz, f);
    fclose(f);
    if (read != (size_t)sz) {
        Hydro::logWarn("EngineAPI: loadAssetRegistryBin: short read (%zu/%ld)", read, sz);
        return false;
    }

    // -- Primary path: FAssetRegistryImpl::Serialize ----------------------
    // Replicates UAssetRegistryImpl::Serialize's body without the
    // interface-lock or broadcast machinery. UAR::Serialize source:
    //   if (Ar.IsObjectReferenceCollector()) return;
    //   FEventContext EventContext;       // stack zero-init
    //   { lock; GuardedData.Serialize(Ar, EventContext); }   // ← THE work
    //   Broadcast(EventContext);
    //
    // We skip the lock (single-threaded call from our bridge) and the
    // broadcast (we don't need asset-event listeners). Just synthesize
    // the FEventContext and call directly.
    //
    // FEventContext is ~112 bytes (5 TArrays + TOptional<...> + 4 bools).
    // We allocate 256 bytes zero-init for forward-compat. With all zeros,
    // the early buffer-cursor check (`mov rcx, [r8]; cmp rax, [r8+8]`)
    // sees cur=0, end=0 → cmp 4, 0 → ja → skips the bad-pointer deref
    // that crashed our prior slot-124 calls.
    if (!s_arImplSerializeFn) {
        findArStateLoadFn();   // soft-fail: leaves ptr null on resolver miss
        if (s_arImplSerializeFn) saveScanCache(s_gm);
    }
    // Diagnostic short-circuit: HYDRO_AR_STATELOAD_DRY_RUN=1 resolves the
    // function pointer + offsets but doesn't call. Useful for verifying
    // candidate selection without risking game state.
    {
        char dryBuf[8] = {};
        DWORD dryLen = GetEnvironmentVariableA("HYDRO_AR_STATELOAD_DRY_RUN",
                                               dryBuf, sizeof(dryBuf));
        if (dryLen > 0 && dryLen < sizeof(dryBuf) && dryBuf[0] == '1') {
            Hydro::logWarn("EngineAPI: loadAssetRegistryBin: ImplSerialize "
                           "DRY-RUN (HYDRO_AR_STATELOAD_DRY_RUN=1); skipping call");
            return false;
        }
    }
    if (s_arImplSerializeFn) {
        // -- Discover a real FBufferReader ctor ----------------------------
        // Using Epic's own FArchive subclass instead of our FArchiveLoader
        // shim removes every layout-mirror invariant from the call. The ctor
        // sets up the vtable, FFastPathLoadBuffer, all bitfields, and
        // CustomVersionContainer correctly. We just hand it our AR.bin
        // pointer + length and pass to UAR::Serialize.
        bool fbrOk = findFBufferReader();
        if (fbrOk) saveScanCache(s_gm);
        // Zero-init FEventContext mirror (256 bytes is forward-compatible
        // for UE 5.6's ~112-byte struct).
        alignas(8) uint8_t eventContextBuf[256] = {};
        void* guardedDataPtr =
            (uint8_t*)s_assetRegImpl + s_arGuardedDataOffset;

        // GuardedData sanity: dump first 64 bytes at s_assetRegImpl+0x30
        // (where we believe FAssetRegistryImpl GuardedData starts). The
        // first field of FAssetRegistryImpl is `FAssetRegistryState State`,
        // and State's first field is `FAssetDataMap CachedAssets` (TMap
        // shape: SetType Elements{Data*, Num, Max} + Hash{Data*, Num, Max}
        // + InlineAllocator + ...). For a fresh AR with no mods loaded,
        // we expect Num=0/Max=0 for all the inner TArrays and Data ptrs
        // either null OR pointing at UE's static EmptyTSet sentinel.
        // If we see arbitrary garbage, GuardedData is NOT at offset 0x30.
        {
            uint8_t* gd = (uint8_t*)s_assetRegImpl + 0x30;
            auto dumpRow = [&](int off) {
                Hydro::logInfo(
                    "EngineAPI:   GuardedData+0x%02X..0x%02X: "
                    "%02X %02X %02X %02X %02X %02X %02X %02X "
                    "%02X %02X %02X %02X %02X %02X %02X %02X",
                    off, off + 15,
                    gd[off + 0], gd[off + 1], gd[off + 2], gd[off + 3],
                    gd[off + 4], gd[off + 5], gd[off + 6], gd[off + 7],
                    gd[off + 8], gd[off + 9], gd[off + 10], gd[off + 11],
                    gd[off + 12], gd[off + 13], gd[off + 14], gd[off + 15]);
            };
            Hydro::logInfo("EngineAPI: GuardedData @ s_assetRegImpl+0x30 = %p:", gd);
            dumpRow(0);
            dumpRow(16);
            // Also dump the pointer at offset 0x4C0 (the early read by the
            // function we're calling: `mov rdi, [rcx+0x4C0]`).
            uint64_t fieldAt4C0 = *(uint64_t*)(gd + 0x4C0);
            Hydro::logInfo("EngineAPI:   GuardedData+0x4C0 = 0x%016llX "
                           "(this is what the function reads early)",
                           (unsigned long long)fieldAt4C0);
        }

        // -- Allocate + construct a real FBufferReader on our AR.bin bytes -
        //
        // sizeof(FBufferReader) = 0xB0 on UE 5.6 shipping. Allocate zeroed
        // and 16-byte aligned (matches MSVC heap allocations for objects
        // containing SSE-loaded fields like FCustomVersionContainer*).
        // Field offsets (post-ctor): +0x90 ReaderData, +0x98 ReaderPos,
        // +0xA0 ReaderSize, +0xA8 bFreeOnClose. Vtable at +0x00.
        alignas(16) uint8_t fbrBuf[0x100] = {};   // 256 bytes - forward-compat
        bool callOk = false;
        int64_t readerPosBefore = 0;
        int64_t readerPosAfter  = 0;
        bool ctorOk = false;
        if (fbrOk && s_fbufferReaderCtor) {
            ctorOk = sehCallFBRCtor(fbrBuf, s_fbufferReaderCtor,
                                    bytes.data(), (int64_t)bytes.size(),
                                    /*bFreeOnClose*/false,
                                    /*bIsPersistent*/true);
            if (!ctorOk) {
                size_t ctorFaultRva = 0;
                if (s_lastFBRCtorFaultAddr >= (DWORD64)s_gm.base &&
                    s_lastFBRCtorFaultAddr <  (DWORD64)s_gm.base + s_gm.size) {
                    ctorFaultRva = (size_t)(s_lastFBRCtorFaultAddr -
                                            (DWORD64)s_gm.base);
                }
                Hydro::logWarn("EngineAPI: FBufferReader ctor SEH-faulted at exe+0x%zX "
                               "code=0x%08X - falling back to FArchiveLoader shim",
                               ctorFaultRva, s_lastFBRCtorFaultCode);
            }
        }

        if (ctorOk) {
            // Sanity: vtable should be set, ReaderData should be our pointer,
            // ReaderSize should be our size, ReaderPos should be 0.
            void*   gotVtable = *(void**)(fbrBuf + 0x00);
            void*   gotData   = *(void**)(fbrBuf + 0x90);
            int64_t gotPos    = *(int64_t*)(fbrBuf + 0x98);
            int64_t gotSize   = *(int64_t*)(fbrBuf + 0xA0);
            Hydro::logInfo("EngineAPI: FBufferReader post-ctor: vt=%p (expect=%p) "
                           "ReaderData=%p (expect=%p) ReaderPos=%lld (expect=0) "
                           "ReaderSize=%lld (expect=%zu)",
                           gotVtable, s_fbufferReaderVtable,
                           gotData, (void*)bytes.data(),
                           (long long)gotPos,
                           (long long)gotSize, bytes.size());
            readerPosBefore = gotPos;

            callOk = sehCallArImplSerialize(guardedDataPtr,
                                            s_arImplSerializeFn,
                                            fbrBuf, eventContextBuf);

            readerPosAfter = *(int64_t*)(fbrBuf + 0x98);
        } else {
            // Fallback: use the legacy FArchiveLoader shim. Same layout-fragile
            // path that previously didn't read bytes; preserved so the bridge
            // still attempts ImplSerialize even when FBufferReader discovery
            // fails (string anchor missing on a future engine variant).
            Hydro::logWarn("EngineAPI: loadAssetRegistryBin: using FArchiveLoader fallback "
                           "(real FBufferReader unavailable - %s)",
                           fbrOk ? "ctor call faulted" : "discovery failed");
            Hydro::FArchiveLoader implShim(bytes.data(), bytes.size());
            callOk = sehCallArImplSerialize(guardedDataPtr,
                                            s_arImplSerializeFn,
                                            &implShim, eventContextBuf);
            // Map shim bytes-read into the real-archive accounting variable
            // so the success-heuristic path below works the same way.
            readerPosAfter = implShim.getBytesRead();
        }

        bool consumedBytes = (readerPosAfter > readerPosBefore);
        // Treat substantially-complete reads as success even when SEH caught an
        // exception: empirically UAR::Serialize on UE 5.6 reads 99.987% of the
        // archive cleanly, then raises a non-AV exception (code 0x00004000 from
        // a system DLL) somewhere in CachePathsFromState / UpdatePersistent-
        // MountPoints / Broadcast. The bulk of the merge has already mutated
        // the live AR by that point; the Lua-side asset lookup will reveal
        // whether the partial state is functionally sufficient.
        bool merged = (callOk && consumedBytes) ||
                      (consumedBytes && readerPosAfter * 10 >= (int64_t)bytes.size() * 9);
        if (merged) {
            Hydro::logInfo("EngineAPI: loadAssetRegistryBin: merged %zu bytes from "
                           "%ls via ImplSerialize (this=%p, ReaderPos %lld → %lld, "
                           "callOk=%d)",
                           bytes.size(), arBinPath, guardedDataPtr,
                           (long long)readerPosBefore, (long long)readerPosAfter,
                           callOk ? 1 : 0);
            // Even on success-by-threshold, surface the captured assert info
            // so we can identify what check() fired post-read.
            if (!callOk &&
                s_lastArSerializeFaultCode == UE_ASSERT_EXCEPTION_CODE) {
                char narrow[1024] = {};
                if (s_lastAssertMessage[0] != 0) {
                    WideCharToMultiByte(CP_UTF8, 0, s_lastAssertMessage, -1,
                                        narrow, sizeof(narrow), nullptr, nullptr);
                }
                DWORD64 pcRva = 0;
                if (s_lastAssertProgramCounter >= (DWORD64)s_gm.base &&
                    s_lastAssertProgramCounter <  (DWORD64)s_gm.base + s_gm.size) {
                    pcRva = s_lastAssertProgramCounter - (DWORD64)s_gm.base;
                }
                Hydro::logInfo("EngineAPI:   (post-merge UE assert: PC=exe+0x%llX msg='%s')",
                               (unsigned long long)pcRva,
                               narrow[0] ? narrow : "<no message captured>");
            }
            return true;
        }
        // ImplSerialize call failed - log and fall through to legacy slot-124.
        size_t faultRva = 0;
        if (!callOk &&
            s_lastArSerializeFaultAddr >= (DWORD64)s_gm.base &&
            s_lastArSerializeFaultAddr <  (DWORD64)s_gm.base + s_gm.size) {
            faultRva = (size_t)(s_lastArSerializeFaultAddr - (DWORD64)s_gm.base);
        }
        Hydro::logWarn("EngineAPI: loadAssetRegistryBin: ImplSerialize result "
                       "(callOk=%d consumedBytes=%d ReaderPos %lld→%lld "
                       "this=%p gdOff=0x%X) fault@exe+0x%zX code=0x%08X "
                       "raw=0x%llX",
                       callOk ? 1 : 0, consumedBytes ? 1 : 0,
                       (long long)readerPosBefore, (long long)readerPosAfter,
                       guardedDataPtr, s_arGuardedDataOffset,
                       faultRva, s_lastArSerializeFaultCode,
                       (unsigned long long)s_lastArSerializeFaultAddr);
        // UE 5.6 AssertExceptionCode = 0x4000 - UE called RaiseException with
        // FAssertInfo carrying the failed check()'s message string. Surface it.
        if (s_lastArSerializeFaultCode == UE_ASSERT_EXCEPTION_CODE &&
            s_lastAssertMessage[0] != 0) {
            char narrow[1024] = {};
            WideCharToMultiByte(CP_UTF8, 0, s_lastAssertMessage, -1,
                                narrow, sizeof(narrow), nullptr, nullptr);
            DWORD64 pcRva = 0;
            if (s_lastAssertProgramCounter >= (DWORD64)s_gm.base &&
                s_lastAssertProgramCounter <  (DWORD64)s_gm.base + s_gm.size) {
                pcRva = s_lastAssertProgramCounter - (DWORD64)s_gm.base;
            }
            Hydro::logWarn("EngineAPI:   UE check() failed - PC=exe+0x%llX msg='%s'",
                           (unsigned long long)pcRva, narrow);
        }
        if (faultRva && s_gm.base + faultRva + 16 <= s_gm.base + s_gm.size) {
            uint8_t* p = s_gm.base + faultRva;
            Hydro::logWarn("EngineAPI:   instruction at fault: "
                           "%02X %02X %02X %02X %02X %02X %02X %02X "
                           "%02X %02X %02X %02X %02X %02X %02X %02X",
                           p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7],
                           p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15]);
            // Walk backward 256 bytes looking for a CC-padding boundary
            // (function start sentinel) so we can identify the helper
            // when funcStartViaPdata can't (e.g., functions that opted
            // out of exception handling don't have .pdata entries).
            uint8_t* funcStart = nullptr;
            for (int back = 4; back < 4096; back++) {
                if (p - back - 1 < s_gm.base) break;
                // Three+ consecutive 0xCC = padding boundary
                if (p[-back - 1] == 0xCC && p[-back] == 0xCC &&
                    p[-back + 1] == 0xCC && p[-back + 2] != 0xCC) {
                    funcStart = p - back + 2;
                    break;
                }
            }
            if (funcStart) {
                Hydro::logWarn("EngineAPI:   helper inferred start: exe+0x%zX "
                               "(fault at +%zd within)",
                               (size_t)(funcStart - s_gm.base),
                               p - funcStart);
                // Dump 64 bytes of helper prologue
                if (funcStart + 64 <= s_gm.base + s_gm.size) {
                    Hydro::logWarn("EngineAPI:   helper prologue [+00..0F]: "
                                   "%02X %02X %02X %02X %02X %02X %02X %02X "
                                   "%02X %02X %02X %02X %02X %02X %02X %02X",
                                   funcStart[0], funcStart[1], funcStart[2], funcStart[3],
                                   funcStart[4], funcStart[5], funcStart[6], funcStart[7],
                                   funcStart[8], funcStart[9], funcStart[10], funcStart[11],
                                   funcStart[12], funcStart[13], funcStart[14], funcStart[15]);
                    Hydro::logWarn("EngineAPI:   helper prologue [+10..1F]: "
                                   "%02X %02X %02X %02X %02X %02X %02X %02X "
                                   "%02X %02X %02X %02X %02X %02X %02X %02X",
                                   funcStart[16], funcStart[17], funcStart[18], funcStart[19],
                                   funcStart[20], funcStart[21], funcStart[22], funcStart[23],
                                   funcStart[24], funcStart[25], funcStart[26], funcStart[27],
                                   funcStart[28], funcStart[29], funcStart[30], funcStart[31]);
                }
            }
            // Identify which function the fault site is inside via .pdata.
            // This tells us what helper crashed (FName decode? TArray Add?)
            // and lets us correlate with UE source.
            uint8_t* faultFnEntry = funcStartViaPdata(p);
            if (faultFnEntry) {
                uint32_t faultFnSize = funcSizeViaPdata(faultFnEntry);
                Hydro::logWarn("EngineAPI:   fault is inside function exe+0x%zX "
                               "(size=%u bytes), fault is at byte +%zd within",
                               (size_t)(faultFnEntry - s_gm.base), faultFnSize,
                               p - faultFnEntry);
                // Dump function prologue (32 bytes) so we can identify the
                // helper from its argument-shape pattern.
                if (faultFnEntry + 32 <= s_gm.base + s_gm.size) {
                    Hydro::logWarn("EngineAPI:   fault fn prologue [+00..0F]: "
                                   "%02X %02X %02X %02X %02X %02X %02X %02X "
                                   "%02X %02X %02X %02X %02X %02X %02X %02X",
                                   faultFnEntry[0], faultFnEntry[1], faultFnEntry[2], faultFnEntry[3],
                                   faultFnEntry[4], faultFnEntry[5], faultFnEntry[6], faultFnEntry[7],
                                   faultFnEntry[8], faultFnEntry[9], faultFnEntry[10], faultFnEntry[11],
                                   faultFnEntry[12], faultFnEntry[13], faultFnEntry[14], faultFnEntry[15]);
                    Hydro::logWarn("EngineAPI:   fault fn prologue [+10..1F]: "
                                   "%02X %02X %02X %02X %02X %02X %02X %02X "
                                   "%02X %02X %02X %02X %02X %02X %02X %02X",
                                   faultFnEntry[16], faultFnEntry[17], faultFnEntry[18], faultFnEntry[19],
                                   faultFnEntry[20], faultFnEntry[21], faultFnEntry[22], faultFnEntry[23],
                                   faultFnEntry[24], faultFnEntry[25], faultFnEntry[26], faultFnEntry[27],
                                   faultFnEntry[28], faultFnEntry[29], faultFnEntry[30], faultFnEntry[31]);
                }
            }
        }
        // Skip the legacy slot-124 fallback - it's known to freeze the game
        // (slot-124 thunk → FStructuredArchive adapter ctor that never
        // returns). The override path's diagnostic is the only useful
        // signal we get when ImplSerialize fails. Set HYDRO_AR_SLOT124_FALLBACK=1
        // to re-enable for direct comparison.
        char fallbackBuf[8] = {};
        DWORD fallbackLen = GetEnvironmentVariableA("HYDRO_AR_SLOT124_FALLBACK",
                                                    fallbackBuf, sizeof(fallbackBuf));
        if (!(fallbackLen > 0 && fallbackLen < sizeof(fallbackBuf) &&
              fallbackBuf[0] == '1')) {
            return false;
        }
        // Else fall through to slot-124 (freeze risk).
    }

    // -- Legacy path: IAssetRegistry::Serialize via slot 124 ---------------
    // Lazy resolve - first call runs the anchor-driven discovery, every
    // subsequent call reuses the cached fn pointer. The cache is also
    // persisted across launches via ScanCache so warm starts skip the scan.
    if (!s_arSerializeFn) {
        if (!findArSerializeFn()) {
            return false;
        }
        // Persist the discovered fn pointer into the scan cache.
        saveScanCache(s_gm);
    }

    // Real call with the AR.bin bytes. Engine reads via our `Serialize`
    // override → memcpy from `bytes` into engine state.
    //
    // For UE 5.6 the resolved fn entry points at the MSVC adjustor thunk
    // (`sub rcx, 0x28; jmp impl`). We must call with `this` =
    // s_assetRegImpl + arVtableOffset so the thunk fixes rcx back to the
    // UObject-base pointer. s_arSerializeThisOffset is set by the
    // resolver; legacy resolution paths leave it 0, in which case the
    // call uses s_assetRegImpl directly (the historical behavior).
    Hydro::FArchiveLoader shim(bytes.data(), bytes.size());
    void* thisPtr = (uint8_t*)s_assetRegImpl + s_arSerializeThisOffset;
    bool ok = sehCallArSerialize(thisPtr, s_arSerializeFn, &shim);

    auto dumpShim = [&](const char* tag) {
        Hydro::logInfo("EngineAPI: loadAssetRegistryBin shim state (%s): "
                       "ser=%d tot=%d atEnd=%d seek=%d tell=%d stub=%d other=%d "
                       "bytesRead=%lld errored=%d",
                       tag,
                       shim.getSerializeCalls(), shim.getTotalSizeCalls(),
                       shim.getAtEndCalls(), shim.getSeekCalls(),
                       shim.getTellCalls(), shim.getStubHits(),
                       shim.getOtherCalls(),
                       (long long)shim.getBytesRead(),
                       shim.errored() ? 1 : 0);
    };

    if (!ok) {
        size_t faultRva = 0;
        if (s_lastArSerializeFaultAddr >= (DWORD64)s_gm.base &&
            s_lastArSerializeFaultAddr <  (DWORD64)s_gm.base + s_gm.size) {
            faultRva = (size_t)(s_lastArSerializeFaultAddr - (DWORD64)s_gm.base);
        }
        Hydro::logError("EngineAPI: loadAssetRegistryBin: Serialize SEH-caught fault "
                        "(%ls) at exe+0x%zX code=0x%08X (raw=0x%llx)",
                        arBinPath, faultRva, s_lastArSerializeFaultCode,
                        (unsigned long long)s_lastArSerializeFaultAddr);
        // If the fault is inside the module, dump 16 bytes around it for
        // disassembly. Outside-module fault = bad pointer dereference; the
        // address itself is the bad pointer.
        if (faultRva && s_gm.base + faultRva + 16 <= s_gm.base + s_gm.size) {
            uint8_t* p = s_gm.base + faultRva;
            Hydro::logError("EngineAPI:   instruction at fault: "
                            "%02X %02X %02X %02X %02X %02X %02X %02X "
                            "%02X %02X %02X %02X %02X %02X %02X %02X",
                            p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7],
                            p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15]);
        }
        dumpShim("on crash");
        return false;
    }
    if (shim.errored()) {
        Hydro::logWarn("EngineAPI: loadAssetRegistryBin: Serialize set ArIsError "
                       "(consumed %lld of %zu bytes from %ls)",
                       (long long)shim.getBytesRead(), bytes.size(), arBinPath);
        // Soft failure - engine bailed mid-read. Often means a version
        // header mismatch or the AR.bin was cooked for a different UE.
        return false;
    }

    Hydro::logInfo("EngineAPI: loadAssetRegistryBin: merged %zu bytes from %ls "
                   "(serialize=%d totalSize=%d bytesRead=%lld)",
                   bytes.size(), arBinPath,
                   shim.getSerializeCalls(), shim.getTotalSizeCalls(),
                   (long long)shim.getBytesRead());
    return true;
}

void* getAssetRegistrySerializeFn() { return s_arSerializeFn; }

void* getFPakPlatformFile() { return s_fpakPlatformFile; }

bool mountPakAtRuntime(const wchar_t* pakPath,
                       const wchar_t* arBinPath,
                       uint32_t priority) {
    if (!pakPath || !*pakPath) {
        Hydro::logError("EngineAPI: mountPakAtRuntime - null/empty pakPath");
        return false;
    }
    if (!s_fpakPlatformFile) {
        Hydro::logError("EngineAPI: mountPakAtRuntime - FPakPlatformFile instance "
                        "not yet discovered. Run AppendState bridge once first "
                        "(it captures FFilePackageStoreBackend → FPakPlatformFile).");
        return false;
    }

    // Convert wide path to narrow for PakLoader (which uses std::string).
    std::string narrowPath;
    {
        size_t len = wcslen(pakPath);
        narrowPath.reserve(len);
        for (size_t i = 0; i < len; i++) {
            wchar_t c = pakPath[i];
            narrowPath.push_back((c < 128) ? (char)c : '?');
        }
    }

    // Lazy-init a PakLoader using our reverse-discovered instance.
    static Hydro::PakLoader s_runtimePakLoader;
    static bool s_runtimePakLoaderReady = false;
    if (!s_runtimePakLoaderReady) {
        if (!s_runtimePakLoader.initializeWithInstance(s_fpakPlatformFile)) {
            Hydro::logError("EngineAPI: mountPakAtRuntime - PakLoader init failed");
            return false;
        }
        s_runtimePakLoaderReady = true;
    }

    // Snapshot FPackageStore.MountedContainers count before mount.
    // After Mount returns true, walk Backends[0].MountedContainers again
    // to verify our ContainerId showed up (proves Layer A→B fired).
    auto readBackendMcCount = [&]() -> int32_t {
        if (!s_filePackageStoreBackend) return -1;
        int32_t lo = 0, hi = 0;
        // backend+0x40 = MountedContainers.Num (per layout: vtable 8 +
        // RWLock 8 + RWLock 8 + RWLock 8 + TArray header).
        // Actual offset captured during inspection earlier was +0x38 +0xC
        // (Data ptr at +0x38, Num at +0x44). Try +0x40..+0x4C.
        for (int off = 0x40; off <= 0x60; off += 4) {
            int32_t v = 0;
            if (!safeReadInt32((uint8_t*)s_filePackageStoreBackend + off, &v)) continue;
            // Plausible Num: 1..32. Skip implausible values.
            if (v >= 1 && v <= 32) {
                int32_t maxV = 0;
                safeReadInt32((uint8_t*)s_filePackageStoreBackend + off + 4, &maxV);
                if (maxV >= v && maxV <= 64) return v;
            }
        }
        return -1;
    };
    int32_t mcNumBefore = readBackendMcCount();
    Hydro::logInfo("EngineAPI: mountPakAtRuntime - pre-mount mc Num=%d", mcNumBefore);

    Hydro::logInfo("EngineAPI: mountPakAtRuntime - calling FPakPlatformFile::Mount('%ls', order=%u)",
                   pakPath, priority);
    auto result = s_runtimePakLoader.mountPak(narrowPath, std::string(), (int)priority);
    if (!result.success) {
        Hydro::logError("EngineAPI: mountPakAtRuntime - Mount failed: %s",
                        result.error.c_str());
        return false;
    }
    Hydro::logInfo("EngineAPI: mountPakAtRuntime - Mount returned true");

    // Post-mount verification: our pak should now be in MountedContainers.
    int32_t mcNumAfter = readBackendMcCount();
    Hydro::logInfo("EngineAPI: mountPakAtRuntime - post-mount mc Num=%d (delta %+d)",
                   mcNumAfter,
                   (mcNumAfter >= 0 && mcNumBefore >= 0) ? (mcNumAfter - mcNumBefore) : 0);
    if (mcNumAfter > mcNumBefore && mcNumBefore >= 0) {
        Hydro::logInfo("EngineAPI: mountPakAtRuntime - ✓ FPackageStore.MountedContainers grew, "
                       "our pak's container is registered");
    } else {
        Hydro::logWarn("EngineAPI: mountPakAtRuntime - Num didn't grow; Mount returned true "
                       "but the IoStore companion may not have been present (no .utoc/.ucas "
                       "alongside the .pak - Layer A skipped, Layer B unreachable)");
    }

    // Optional AR.bin merge via the existing AppendState bridge.
    if (arBinPath && *arBinPath) {
        Hydro::logInfo("EngineAPI: mountPakAtRuntime - merging AR.bin '%ls'", arBinPath);
        if (!loadAssetRegistryBin(arBinPath)) {
            Hydro::logWarn("EngineAPI: mountPakAtRuntime - AR.bin merge failed "
                           "(pak still mounted; some content may not be AR-discoverable)");
            // Pak mount succeeded; AR merge is best-effort. Return true
            // because the package store IS populated and StaticLoadObject
            // works without AR for direct path queries.
        } else {
            Hydro::logInfo("EngineAPI: mountPakAtRuntime - AR.bin merged successfully");
        }
    }
    return true;
}

} // namespace Hydro::Engine
