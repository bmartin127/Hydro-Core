#pragma once

// Shared helpers for engine/<Module>.cpp: byte scanners, RIP-relative
// resolvers, PE .pdata function-bounds helpers, and the cross-module globals.

#include <cstdint>
#include <cstddef>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace Hydro::Engine {

struct GameModule { uint8_t* base; size_t size; };

extern GameModule s_gm;
extern bool       s_ready;

// Set true after a fatal init failure (e.g. SLO sanity-test crash). Stops
// init from re-running in the same process; the cache file gets invalidated
// for next launch.
extern bool       s_initFatallyFailed;

GameModule findGameModule();

bool safeReadPtr(void* addr, void** out);
bool safeReadInt32(void* addr, int32_t* out);
bool safeReadU64(void* addr, uint64_t* out);

bool      matchPattern(const uint8_t* data, const char* pattern, size_t* outLen);
uint8_t*  resolveRip4(uint8_t* addr);

uint8_t*               findWideString(uint8_t* base, size_t size, const wchar_t* str);
uint8_t*               findLeaRef(uint8_t* base, size_t size, uint8_t* target);
std::vector<uint8_t*>  findAllLeaRefs(uint8_t* base, size_t size, uint8_t* target);
std::vector<uint8_t*>  findAllRipRelativeRefs(uint8_t* base, size_t size, uint8_t* target);
std::unordered_set<uint8_t*> findE8CallersOf(uint8_t* base, size_t size, uint8_t* target);

uint8_t* walkToFuncStart(uint8_t* addr, int maxBack = 16384);

#ifdef _WIN32
const RUNTIME_FUNCTION* lookupRuntimeFunction(uint32_t targetRva);
#endif
uint8_t* funcStartViaPdata(uint8_t* addr);

std::vector<uint8_t*> collectForwardCallTargets(uint8_t* start, int maxBytes = 130);

} // namespace Hydro::Engine
