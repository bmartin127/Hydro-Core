#pragma once
#include <cstdint>

namespace Hydro::Engine {

// Spawn-system globals. Discovered in EngineAPI.cpp's discoverEngineObjects()
// during initialize(); consumed by spawnActor + the SpawnParams-layout
// resolver. World is refreshed on every spawn (refreshWorld lives in
// EngineAPI.cpp because getPlayerCharacter / getPlayerPawn also call it).
extern void* s_world;                // UWorld* (refreshed per spawn)
extern void* s_gameplayStaticsCDO;   // Default__GameplayStatics
extern void* s_spawnFunc;            // BeginDeferredActorSpawnFromClass UFunction*
extern void* s_finishSpawnFunc;      // FinishSpawningActor UFunction*

// Pattern-scans for AActor::DispatchBeginPlay given the vtable offset of
// AActor::BeginPlay in the current engine version. Returns the function entry
// or nullptr on no confident match.
void* findDispatchBeginPlay(int beginPlayVtableOffset);

// Spawn an actor of `actorClass` at the given world-space position. Resolves
// BeginDeferredActorSpawnFromClass param layout via reflection on first call,
// falls back to a hardcoded UE 5.5 SpawnParams struct if reflection isn't
// ready yet. Returns the spawned AActor* or nullptr.
void* spawnActor(void* actorClass, double x, double y, double z);

} // namespace Hydro::Engine
