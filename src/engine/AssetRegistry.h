#pragma once
#include <cstdint>

namespace Hydro::Engine {

// -- AR globals ---------------------------------------------------------------
// Defined in AssetRegistry.cpp; declared non-static so EngineAPI.cpp's
// initialize() and ScanCache paths can read/write them directly.

extern void* s_assetRegHelpersCDO;       // Default__AssetRegistryHelpers
extern void* s_getAssetFunc;             // AssetRegistryHelpers:GetAsset UFunction
extern void* s_assetRegImpl;             // runtime UAssetRegistryImpl* (CDO == singleton in UE 5.6)
extern void* s_getByPathFunc;            // AssetRegistry:GetAssetByObjectPath UFunction

// IAssetRegistry::Serialize (slot 124) - legacy fallback path.
// Resolved via source-counted vtable slot from the IAssetRegistry secondary base.
// The thunk does `sub rcx, s_arSerializeThisOffset` before jumping to the real body.
extern void* s_arSerializeFn;
extern uint16_t s_arSerializeThisOffset;

// FAssetRegistryImpl::Serialize(FArchive&, FEventContext&) - primary path.
// Located via GUID-literal -> SerializeVersion -> SerializeHeader -> Load walk.
extern void* s_arImplSerializeFn;
extern uint16_t s_arGuardedDataOffset;   // s_assetRegImpl offset to FAssetRegistryImpl GuardedData

// AppendState path - env-var-gated until robust resolvers land.
extern void* s_loadFromDiskFn;           // FAssetRegistryState::LoadFromDisk
extern void* s_uarAppendStateFn;         // FAssetRegistryImpl::AppendState (inner, 4-arg)

// FBufferReader discovery for the ImplSerialize call.
extern void* s_fbufferReaderCtor;
extern void* s_fbufferReaderVtable;
extern size_t s_fbufferReaderSize;

// FPackageStore live inspection / FPakPlatformFile reverse-discovery.
// Captured inside tryMergeViaAppendState; consumed by mountPakAtRuntime.
extern void* s_filePackageStoreBackend;  // FFilePackageStoreBackend* found via Backends walk
extern void* s_fpakPlatformFile;         // FPakPlatformFile* reverse-found via backend ptr scan
extern void* s_fpakPlatformFileVtable;   // *(void**)s_fpakPlatformFile

// -- Public API (mirrored declarations from EngineAPI.h) ----------------------
bool discoverAssetRegistry();
bool scanAssetRegistryPaths(const wchar_t* virtualPath, bool forceRescan);
bool searchAllAssets(bool bSynchronousSearch);
bool scanAssetRegistryFiles(const wchar_t* uassetFilename, bool forceRescan);
void tryDeferredAssetRegistryDiscovery();
bool loadAssetRegistryBin(const wchar_t* arBinPath);
void* getAssetRegistrySerializeFn();
void* getFPakPlatformFile();
bool mountPakAtRuntime(const wchar_t* pakPath,
                       const wchar_t* arBinPath,
                       uint32_t priority);

} // namespace Hydro::Engine
