#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

/*
 * EngineAPI: HydroCore's engine reflection layer.
 *
 * Pure C++ with no UE4SS headers touched here. All engine interaction
 * goes through:
 *   - Pattern scanning to discover function addresses
 *   - Raw vtable offsets for ProcessEvent (0x278)
 *   - Direct GUObjectArray iteration for object discovery
 *   - SEH wrappers for crash safety
 *
 * This is the foundation that mods call through. UE4SS is used in
 * dllmain.cpp for the DLL injection lifecycle and nothing else.
 */

namespace Hydro::Engine {

// Bootstrap

/// Initialize the engine reflection layer.
/// Finds all required function addresses via pattern scanning.
/// Must be called from the game thread (on_update with delay).
/// Returns true if all critical functions were found.
bool initialize();

/// Is the API ready to use?
bool isReady();

/// Refresh the UWorld pointer (call before spawning if map may have changed).
/// Re-scans GUObjectArray for the current active world.
bool refreshWorld();

// Object loading

/// Find an in-memory UObject by path (e.g., "/Script/Engine.Actor").
/// Uses StaticFindObject - only finds already-loaded objects.
void* findObject(const wchar_t* path);

/// Load an asset from a pak file by path (e.g., "/Game/BP_TestCube").
/// Uses AssetRegistry::GetAssetByObjectPath -> GetAsset via ProcessEvent.
/// Matches the dispatch path BPModLoaderMod follows.
void* loadAsset(const wchar_t* assetPath);

/// Force-load a UObject at `path` via StaticLoadObject - actually triggers
/// package loading from disk, including from runtime-mounted paks whose
/// contents aren't yet in the AssetRegistry. Tries several flag
/// combinations (NoVerify|NoWarn, None, etc.) to get past shipping-build
/// version checks. Falls through to StaticFindObject as a last resort.
/// Use this as the third-tier fallback after loadAsset + findObject.
void* loadObject(const wchar_t* path);

/// Register a runtime-mounted pak's shader library with UE's renderer.
/// UE auto-mounts shader bytecode from base-game paks at startup, but a
/// pak mounted later doesn't get its `ShaderArchive-<lib>-*.ushaderbytecode`
/// auto-registered - materials fall back to defaults until we explicitly
/// open the library.
///
/// Calls `FShaderCodeLibrary::OpenLibrary(libraryName, mountDir)`.
/// `libraryName` is the token between `ShaderArchive-` and the first `-`
/// in the cooked filename (typically the modder's project name).
/// `mountDir` is the path where the archive lives inside the mount, e.g.
/// `../../../DummyModdableGame/Content/`.
///
/// Returns true on success, false on failure or if discovery hasn't run.
bool openShaderLibrary(const wchar_t* libraryName, const wchar_t* mountDir);

/// Get the discovered FShaderCodeLibrary::OpenLibrary function pointer.
/// Caller is responsible for constructing properly-allocated FString args
/// (use UE4SS's RC::Unreal::FString for GMalloc-allocated buffers).
/// Returns nullptr if discovery hasn't run or failed.
void* getOpenShaderLibraryFn();

/// Trigger an AssetRegistry rescan of a virtual path so newly-mounted paks
/// become discoverable via GetAsset / GetAssetByObjectPath. Calls the
/// reflected UFUNCTION `IAssetRegistry::ScanPathsSynchronous` - no
/// dependency on pattern-scanned StaticLoadObject / LoadPackage. Pass
/// e.g. L"/Game/Mods/" to scan all mod assets after pak mount. Set
/// forceRescan=true to invalidate any prior cached entries.
/// Returns true on successful ProcessEvent dispatch.
bool scanAssetRegistryPaths(const wchar_t* virtualPath, bool forceRescan = false);

/// Trigger an AssetRegistry rescan of a specific .uasset filesystem path so a
/// runtime-mounted pak whose virtual mount point isn't registered with
/// `FPackageName` becomes discoverable. Calls the reflected UFUNCTION
/// `IAssetRegistry::ScanFilesSynchronous` - this takes a filesystem path
/// list rather than a virtual /Game/ path, which on Palworld's UE 5.1 fork
/// bypasses the broken `FPackageName::TryConvertLongPackageNameToFilename`
/// step that drops our /Game/Mods/* paths silently. Use after pak mount when
/// `scanAssetRegistryPaths` has returned without indexing anything (verified
/// by walking GUObjectArray for UPackage objects under the prefix).
/// Returns true on successful ProcessEvent dispatch (NOT confirmation that
/// indexing happened - caller must verify via UPackage census).
bool scanAssetRegistryFiles(const wchar_t* uassetFilename, bool forceRescan = false);

/// Engine-tick-driven AssetRegistry retry. AR CDOs are instantiated later
/// in engine startup than HydroCore's bootstrap, so the initial discovery
/// pass typically misses them. This function does nothing if AR is already
/// discovered; otherwise it retries once and is rate-limited internally so
/// it doesn't burn CPU on the game thread. Safe to call every frame.
void tryDeferredAssetRegistryDiscovery();

// Actor spawning

/// Spawn an actor in the world.
/// actorClass: UClass* for the actor to spawn
/// x, y, z: world coordinates
/// Returns spawned AActor* or nullptr on failure.
void* spawnActor(void* actorClass, double x, double y, double z);

/// Find the local player's character via UE's own
/// `UGameplayStatics::GetPlayerCharacter(World, index)`. Uses UE's internal
/// player-controller list - O(1), and is the canonical way to find the
/// player in any UE game. Returns ACharacter* or nullptr.
void* getPlayerCharacter(int playerIndex);

/// Find the local player's pawn via UE's `UGameplayStatics::GetPlayerPawn`.
/// Returns the pawn (may be a non-Character Pawn) or nullptr.
void* getPlayerPawn(int playerIndex);

/// Get every actor of the given class via UE's
/// `UGameplayStatics::GetAllActorsOfClass(World, Class, out)`. Uses UE's
/// hash-table-backed iteration (`TActorIterator`) - O(matching actors),
/// not O(all UObjects). Writes up to `maxResults` actor pointers into
/// `outArray`, returns the count written.
int getAllActorsOfClass(void* actorClass, void** outArray, int maxResults);

// Reflection API (for Lua bindings)

/// Get an object's UClass*.
void* getClass(void* obj);

/// Find a UFunction on a UClass by name. Walks the Children chain.
void* findFunction(void* uclass, const wchar_t* funcName);

/// Find an FProperty on a UClass by name. Walks the ChildProperties chain.
void* findProperty(void* uclass, const wchar_t* propName);

/// Call ProcessEvent on an object with a function and params buffer.
bool callFunction(void* obj, void* func, void* params);

/// Return the discovered ProcessEvent function address (nullptr if not ready).
/// Used by Hydro.Events for its inline hook.
void* getProcessEventAddress();

/// Scan the game module for AActor::DispatchBeginPlay.
///
/// DispatchBeginPlay is the non-virtual engine funnel that every actor's
/// BeginPlay passes through - both level-load actors (via InitializeActorsForPlay)
/// and runtime-spawned actors (via PostActorConstruction). A single inline hook
/// on this address catches every actor regardless of subclass overrides.
///
/// Discovery is dynamic: we scan for the `call [reg+beginPlayVtableOffset]`
/// pattern (DispatchBeginPlay's final virtual dispatch to BeginPlay), walk back
/// to each enclosing function start, then score candidates by structural shape
/// (size, exactly one virtual call at the offset, presence of flag writes).
/// The highest-scoring unique candidate wins. Multiple diagnostic candidates
/// are logged so future engine-version shifts are debuggable.
///
/// Returns the function address, or nullptr if discovery couldn't confidently
/// pick a candidate. The caller should fall back to another strategy in that
/// case rather than guess.
void* findDispatchBeginPlay(int beginPlayVtableOffset);

/// Game module accessors. The "game module" is the largest non-injector
/// DLL/exe in the process - i.e. the host's main binary. RawFn discovery
/// scans this range; nothing else in HydroCore needs it directly today,
/// but exposing it lets adjacent modules (RawFunctions, future tools)
/// reuse the same canonical handle EngineAPI picked at init time.
/// Both return zero/null if initialize() hasn't run yet.
uint8_t* getGameModuleBase();
size_t getGameModuleSize();

/// Read a pointer safely (returns false on access violation).
bool readPtr(void* addr, void** out);

/// Read an int32 safely.
bool readInt32(void* addr, int32_t* out);

/// Get the FFieldClass name index for an FProperty (identifies its type).
/// Returns the FName ComparisonIndex of the property's class (e.g., "IntProperty").
uint32_t getPropertyTypeNameIndex(void* prop);

/// Get an FProperty's offset within its owning UObject.
int32_t getPropertyOffset(void* prop);

/// Get an FProperty's element size.
int32_t getPropertyElementSize(void* prop);

/// Get the next FProperty in the chain (ChildProperties linked list).
void* getNextProperty(void* prop);

/// Get the first FProperty on a UStruct/UClass.
void* getChildProperties(void* ustruct);

/// Get FProperty flags (CPF_Parm, CPF_ReturnParm, etc.)
/// FProperty::PropertyFlags is a uint64 at offset ~0x38.
uint64_t getPropertyFlags(void* prop);

// FProperty flag constants
constexpr uint64_t CPF_Parm       = 0x0000000000000080;
constexpr uint64_t CPF_OutParm    = 0x0000000000000100;
constexpr uint64_t CPF_ReturnParm = 0x0000000000000400;

/// Construct an FName from a wide string. Returns the ComparisonIndex.
/// Uses the discovered FName constructor.
uint32_t makeFName(const wchar_t* str);

// FName resolution (index -> string)

/// Convert an FName ComparisonIndex to a display string. Uses the engine's
/// own Conv_NameToString Blueprint function via ProcessEvent - no pool
/// reading, no UE4SS. Results are cached so each index is only resolved once.
std::string getNameString(uint32_t nameIdx);

/// Get a UObject's FName as a display string.
std::string getObjectName(void* obj);

// Class hierarchy

/// Get the parent UClass/UStruct (SuperStruct pointer).
void* getSuper(void* ustruct);

/// Get a UObject's Outer (package/owner) pointer.
void* getOuter(void* obj);

/// Build the full path string for a UObject by walking its Outer chain.
/// Returns paths like "/Script/Engine.Actor" or "/Game/Mods/BP_TestCube".
std::string getObjectPath(void* obj);

// GUObjectArray access

int32_t getObjectCount();
void* getObjectAt(int32_t index);
void* getObjClass(void* obj);
uint32_t getNameIndex(void* obj);

/// Get an FField's (FProperty's) name as a display string. FField stores
/// its name at offset FFIELD_NAME (0x20), not at the UObject offset (0x18).
std::string getFieldName(void* field);

// UFunction metadata

uint16_t getUFunctionParmsSize(void* ufunc);
uint16_t getUFunctionRetOffset(void* ufunc);
uint32_t getUFunctionFlags(void* ufunc);

// UClass metadata

/// Read UClass::ClassFlags. Offset is discovered on first call by probing
/// known classes ("/Script/CoreUObject.Object" always has CLASS_Native set).
/// Returns 0 on failure.
uint32_t getClassFlags(void* cls);

// UEnum metadata

/// Read the Names TArray<TPair<FName, int64>> from a UEnum. Output is a
/// list of (comparisonIndex, value) pairs. Offset is probed on first call.
/// Returns the number of entries read, or 0 on failure.
int readEnumNames(void* uenum, std::vector<std::pair<uint32_t, int64_t>>& out);

// Object discovery

/// Find the first UObject whose class name matches.
/// Iterates GUObjectArray. className compared via FName index.
void* findFirstOf(const wchar_t* className);

/// Find all UObjects whose class name matches.
/// Results written to outArray (caller-provided). Returns count found.
int findAllOf(const wchar_t* className, void** outArray, int maxResults);

/// Get the current UWorld pointer (refreshed automatically).
void* getWorld();

/// Get the current network mode by calling UWorld::GetNetMode via reflection.
/// Returns the ENetMode value:
///   0 = NM_Standalone       (no networked session - singleplayer or main menu)
///   1 = NM_DedicatedServer  (headless server)
///   2 = NM_ListenServer     (player-hosted P2P session)
///   3 = NM_Client           (joined a remote session)
/// Returns 0 (Standalone) if the world isn't ready yet, GetNetMode UFunction
/// can't be discovered, or the call fails - never throws. Lazy-caches the
/// resolved UFunction pointer; the call itself runs every invocation since
/// NetMode changes when the player travels into / out of a session.
int getNetMode();

// FFieldClass name constants
// Pre-constructed FName indices for type dispatch.
// Call initPropertyTypeNames() after FName constructor is discovered.

struct PropertyTypeNames {
    uint32_t intProperty;
    uint32_t int64Property;
    uint32_t floatProperty;
    uint32_t doubleProperty;
    uint32_t boolProperty;
    uint32_t strProperty;
    uint32_t nameProperty;
    uint32_t textProperty;
    uint32_t objectProperty;
    uint32_t classProperty;
    uint32_t structProperty;
    uint32_t arrayProperty;
    uint32_t enumProperty;
    uint32_t byteProperty;
    uint32_t uint32Property;
    uint32_t uint16Property;
    uint32_t int16Property;
    uint32_t int8Property;
    bool initialized;
};

/// Initialize property type name indices. Call after EngineAPI::initialize().
bool initPropertyTypeNames();

/// Get the property type names table.
const PropertyTypeNames& getPropertyTypeNames();

// UObject layout constants (UE 5.5)

constexpr int UOBJ_VTABLE      = 0x00;
constexpr int UOBJ_FLAGS       = 0x08;
constexpr int UOBJ_INDEX       = 0x0C;
constexpr int UOBJ_CLASS       = 0x10;
constexpr int UOBJ_NAME        = 0x18;
constexpr int UOBJ_OUTER       = 0x20;

constexpr int VTABLE_PROCESS_EVENT = 79;  // 0x278 / 8

// FUObjectItem layout
constexpr int FUOBJ_OBJECT     = 0x00;
constexpr int FUOBJ_FLAGS      = 0x08;
constexpr int FUOBJ_SIZE       = 0x18;  // 24 bytes per item

// FChunkedFixedUObjectArray layout
constexpr int FARRAY_OBJECTS   = 0x00;  // FUObjectItem** chunk table
constexpr int FARRAY_PREALLOC  = 0x08;  // FUObjectItem* prealloc (may be null)
constexpr int FARRAY_MAX_ELEMS = 0x10;  // int32 MaxElements
constexpr int FARRAY_NUM_ELEMS = 0x14;  // int32 NumElements
constexpr int CHUNK_SIZE       = 65536; // elements per chunk

// UFunction layout (UE 5.5 reference values - fallbacks only)
constexpr int UFUNC_CHILD_PROPS     = 0x50;  // FProperty* ChildProperties (from UStruct)
// UFUNC_PARMS_SIZE / UFUNC_RET_VAL_OFFSET are no longer constants - derived
// from the parameter property chain in getUFunctionParmsSize/RetOffset. Kept
// only as a one-time bootstrap fallback if the property chain walk fails.
constexpr int UFUNC_PARMS_SIZE      = 0xB6;  // uint16 ParmsSize  (fallback)
constexpr int UFUNC_RET_VAL_OFFSET  = 0xB8;  // uint16 ReturnValueOffset (fallback)

// FField layout (UE 5.x - bootstrap minimum, kept hardcoded)
constexpr int FFIELD_NEXT           = 0x18;  // FField* Next
constexpr int FFIELD_NAME           = 0x20;  // FName NamePrivate (fallback only)

// FProperty layout - bootstrap fallbacks. Real values are discovered at
// runtime via discoverPropertyLayout() and accessed through the
// getPropertyOffset / getPropertyElementSize / getPropertyFlags accessors.
// Precedence: discovered > cached > these fallbacks.
constexpr int FPROP_ELEMENT_SIZE    = 0x34;  // int32 ElementSize    (fallback)
constexpr int FPROP_OFFSET_INTERNAL = 0x44;  // int32 Offset_Internal (fallback)

// UStruct layout (UE 5.x - bootstrap minimum, kept hardcoded)
constexpr int USTRUCT_CHILDREN      = 0x48;  // UField* Children (linked list of UFunctions)
constexpr int USTRUCT_CHILD_PROPS   = 0x50;  // FField* ChildProperties

// UField layout
constexpr int UFIELD_NEXT           = 0x28;  // UField* Next

// UFunction native pointer
constexpr int UFUNC_FUNC            = 0xD8;  // FNativeFuncPtr Func
constexpr int UFUNC_FLAGS           = 0xB0;  // EFunctionFlags

// -- Reflection-driven layout discovery (Layer 2/3/4) ---
// HydroCore self-adapts to any UE5 host by probing FProperty/UStruct internal
// offsets at startup against known-good engine objects, then reading every
// property through the discovered offsets. The hardcoded constants above are
// fallbacks for stock UE 5.5; real values are picked up from `s_layout`.
// All getters below transparently use the discovered value if available,
// falling back to the stock-UE-5.5 constant otherwise. Public signatures
// (getPropertyOffset, etc.) are unchanged - discovery is internal.

/// Probe FProperty internal offsets (Offset_Internal, ElementSize, PropertyFlags)
/// using AActor::PrimaryActorTick as the anchor. Lazy: runs once, caches in
/// static globals + scan cache. Safe to call before AActor reflection is up;
/// returns false in that case so the caller can defer.
bool discoverPropertyLayout();

/// Walk a UClass's property chain (using discovered offsets) and return the
/// Offset_Internal of the property whose name matches `fieldName`. Cached
/// per (uclassPtr, fieldName). Returns -1 if the field isn't found.
int32_t findReflectedFieldOffset(void* uclassPtr, const wchar_t* fieldName);

} // namespace Hydro::Engine
