#pragma once
#include <cstdint>
#include <string>

namespace Hydro::Engine {

// Function pointer globals for the object lookup tier.
// s_staticLoadObject   - StaticLoadObject (full disk load, 8-arg form)
// s_realStaticFindObject - StaticFindObject after behavioral validation
// s_loadPackage        - LoadPackageInternal (Tier 2 fallback in loadObject)
// s_staticFindObject   - StaticFindObject (in-memory only; may be same as s_realStaticFindObject)
// s_staticFindObjectFast - StaticFindObjectFast (useful anchor, not called directly)
extern void* s_staticLoadObject;
extern void* s_realStaticFindObject;
extern void* s_loadPackage;
extern void* s_staticFindObject;
extern void* s_staticFindObjectFast;

// Public API (declarations mirrored in EngineAPI.h).
void* findObject(const wchar_t* path);
void* loadObject(const wchar_t* path);
void* loadAsset(const wchar_t* assetPath);
void* findFirstOf(const wchar_t* className);
int   findAllOf(const wchar_t* className, void** outArray, int maxResults);

// Called during initialize() - discovers StaticLoadObject and the find variants.
bool findStaticLoadObject();

// SEH-wrapped call helpers. Declared here because EngineAPI.cpp's initialize()
// uses them in the validation loops that follow findStaticLoadObject().
void* safeCallLoadObject(void* funcAddr, const wchar_t* path, uint32_t flags = 0);
void* safeCallFindObject(void* funcAddr, const wchar_t* path);

// Behavioral validators for candidate SLO/SFO functions.
bool validateStaticFindObjectCandidate(void* fn, std::string* outName);
bool validateStaticLoadObjectResult(void* anyObj, std::string* outName);
bool validateStaticLoadObjectCandidate(void* fn);

// Actor hierarchy helpers used by spawnActor in EngineAPI.cpp.
bool  isActorSubclass(void* uclass);
void* findInWorldActor();
void* findPlayerController();

// Registers known-good engine functions with the RawFn registry and resolves them.
// Called once during initialize() after FName and GUObjectArray are up.
void seedAndResolveRawFunctions();

// Reads a name string directly from FNamePool by ComparisonIndex.
// Used by EngineAPI.cpp's getNameString and diagnostic code.
std::string readFromPool(uint32_t comparisonIndex);

} // namespace Hydro::Engine
