#pragma once
#include <cstdint>

namespace Hydro::Engine {

// 8-byte FName: { ComparisonIndex, Number }.
// Matches UE 5.x shipping layout. WITH_CASE_PRESERVING_NAME adds a third
// uint32 (DisplayIndex) but shipping cooks ship with that flag off.
struct FName8 { uint32_t comparisonIndex; uint32_t number; };

// FName pool state - populated by discoverFNamePool().
extern bool  s_poolReady;
extern void* s_fnamePool;
extern int   s_poolBlocksOffset;

// FName(const TCHAR*, EFindName) constructor function pointer.
extern void* s_fnameConstructor;

bool safeConstructFName(FName8* out, const wchar_t* name, void* ctorOverride = nullptr);
bool findFNameConstructor();
int  readPoolEntryRaw(uint32_t comparisonIndex, char* buf, int bufSize, bool* isWide);
bool discoverFNamePool();

} // namespace Hydro::Engine
