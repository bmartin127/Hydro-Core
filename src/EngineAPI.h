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
 * Mods call through this layer. UE4SS is only used in dllmain.cpp for
 * the DLL injection lifecycle.
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

/// Force-load a UObject at `path` via StaticLoadObject.
/// Tries several flag combinations to bypass shipping-build version checks.
/// Falls through to StaticFindObject. Use after loadAsset + findObject fail.
void* loadObject(const wchar_t* path);

/// Register a runtime-mounted pak's shader library via FShaderCodeLibrary::OpenLibrary.
/// `libraryName` is the token between "ShaderArchive-" and the first "-" in the archive filename.
/// `mountDir` is the directory where the archive lives inside the mount.
bool openShaderLibrary(const wchar_t* libraryName, const wchar_t* mountDir);

/// Get the discovered FShaderCodeLibrary::OpenLibrary function pointer. Returns nullptr if not found.
void* getOpenShaderLibraryFn();

/// Rescan a virtual path via IAssetRegistry::ScanPathsSynchronous (reflected UFUNCTION).
/// Pass e.g. L"/Game/Mods/" after pak mount. Set forceRescan=true to invalidate cached entries.
bool scanAssetRegistryPaths(const wchar_t* virtualPath, bool forceRescan = false);

/// Rescan by .uasset filesystem path via IAssetRegistry::ScanFilesSynchronous.
/// Bypasses FPackageName on hosts (e.g. Palworld) where the virtual mount point isn't registered.
/// Returns true on ProcessEvent dispatch, not confirmation of indexing.
bool scanAssetRegistryFiles(const wchar_t* uassetFilename, bool forceRescan = false);

/// Retry AssetRegistry discovery if not yet complete. Rate-limited internally; safe to call every tick.
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

/// Get actors of a class via UGameplayStatics::GetAllActorsOfClass.
/// Writes up to maxResults pointers into outArray; returns count written.
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

/// Discover AActor::DispatchBeginPlay by scanning for the virtual BeginPlay dispatch.
/// Scores candidates by structural shape; returns the best match or nullptr.
void* findDispatchBeginPlay(int beginPlayVtableOffset);

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

/// Get FProperty::PropertyFlags (CPF_Parm, CPF_ReturnParm, etc.).
uint64_t getPropertyFlags(void* prop);

// FProperty flag constants
constexpr uint64_t CPF_Parm       = 0x0000000000000080;
constexpr uint64_t CPF_OutParm    = 0x0000000000000100;
constexpr uint64_t CPF_ReturnParm = 0x0000000000000400;

/// Construct an FName from a wide string. Returns the ComparisonIndex.
/// Uses the discovered FName constructor.
uint32_t makeFName(const wchar_t* str);

/// Convert an FName ComparisonIndex to a string. Pool read first; Conv_NameToString fallback. Cached.
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

/// Get an FProperty's name string (FField::NamePrivate, not the UObject name slot).
std::string getFieldName(void* field);

// UFunction metadata

uint16_t getUFunctionParmsSize(void* ufunc);
uint16_t getUFunctionRetOffset(void* ufunc);
uint32_t getUFunctionFlags(void* ufunc);

// UClass metadata

/// Read UClass::ClassFlags. Offset discovered on first call; returns 0 on failure.
uint32_t getClassFlags(void* cls);

// UEnum metadata

/// Read UEnum::Names as (comparisonIndex, value) pairs. Offset probed on first call.
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

/// Get the current ENetMode (0=Standalone, 1=DedicatedServer, 2=ListenServer, 3=Client).
/// Returns 0 on failure. Lazy-discovers UWorld::GetNetMode via reflection.
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

// UFunction layout (UE 5.5 fallbacks; PARMS_SIZE / RET_VAL_OFFSET derived from property chain)
constexpr int UFUNC_CHILD_PROPS     = 0x50;  // FProperty* ChildProperties (from UStruct)
constexpr int UFUNC_PARMS_SIZE      = 0xB6;  // uint16 ParmsSize (fallback)
constexpr int UFUNC_RET_VAL_OFFSET  = 0xB8;  // uint16 ReturnValueOffset (fallback)

// FField layout (UE 5.x - bootstrap minimum, kept hardcoded)
constexpr int FFIELD_NEXT           = 0x18;  // FField* Next
constexpr int FFIELD_NAME           = 0x20;  // FName NamePrivate (fallback only)

// FProperty layout - bootstrap fallbacks (runtime-discovered values take precedence)
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

/// Probe FProperty internal offsets against AActor anchors. Lazy; returns false if AActor not ready yet.
bool discoverPropertyLayout();

/// Return the Offset_Internal of a named property on a UClass. Cached per (class, name); -1 if not found.
int32_t findReflectedFieldOffset(void* uclassPtr, const wchar_t* fieldName);

} // namespace Hydro::Engine
