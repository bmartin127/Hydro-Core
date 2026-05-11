#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

/*
 * RawFunctions - resilient discovery registry for non-reflected C++ engine
 * functions.
 *
 * UE has thousands of non-reflected (no UFUNCTION marker) C++ functions that
 * mods sometimes need to call directly: FTimerManager::SetTimer,
 * UWorld::SpawnActor's underlying impl, FString::AppendTo, etc. The naive
 * way to find them is hardcoded byte signatures or vtable offsets, both of
 * which break on every game patch and every host-engine fork.
 *
 * This module generalizes the pattern HydroCore was already using piecewise
 * (HydroEvents.cpp's discoverProcessInternal, EngineAPI.cpp's GUObjectArray
 * string-anchor walk) into a uniform registry keyed by stable string names.
 * Mods register a Descriptor describing *how* to find a function (Tier 1/2/3)
 * and then call resolve(name) - discovery runs once per launch, results are
 * persisted in a separate cache file invalidated when the host module size
 * changes (i.e. game update).
 *
 * Three discovery strategies, in order of fragility:
 *
 *   Tier 1 - UFuncImpl (zero fragility)
 *     Many "non-reflected" C++ functions are the implementation body of a
 *     thin reflected wrapper. Reads the UFunction's Func pointer at
 *     UFUNC_FUNC (0xD8) and follows trampolines. The UFunction path is the
 *     stable key; Func pointer offset is a UE ABI contract.
 *
 *   Tier 2 - NthCallInUFunc (handles 1-hop indirection)
 *     When the target is the Nth CALL inside a reflected wrapper's impl
 *     body. Zydis-decodes the body bounded to 512 bytes / first RET. Call
 *     order is preserved by every modern compiler far more reliably than
 *     byte sequences are preserved across patches.
 *
 *   Tier 3 - StringRefAnchor
 *     For standalone functions that don't have a reflected parent. Locates
 *     a unique wide string in the module, walks LEA references back to the
 *     enclosing function start. The same approach already proven in
 *     EngineAPI.cpp for GUObjectArray / StaticLoadObject / LoadPackage.
 *
 * All resolve() results are cached in `hydro_raw_funcs.bin` next to the DLL.
 * Cache is keyed by host module size - a game update invalidates everything
 * and the next launch re-discovers transparently.
 */

namespace Hydro::Engine::RawFn {

enum class Strategy : uint8_t {
    UFuncImpl       = 1,   // anchor = UFunction object path; reads Func@0xD8
    NthCallInUFunc  = 2,   // anchor = UFunction path; takes Nth CALL inside Func body
    StringRefAnchor = 3,   // anchor = unique wide string; walks back to func start
};

struct Descriptor {
    Strategy     strategy;
    std::wstring anchor;       // UFunction path, or unique wide string
    int          nthCall = 0;  // 1-based; only for NthCallInUFunc
};

/// Register a discovery descriptor under a stable name. Idempotent - calling
/// twice with the same name is a no-op (registry keeps the first).
/// Cheap; safe to call from any module's init path.
void registerFn(const std::string& name, const Descriptor& desc);

/// Resolve a registered raw function address.
///
/// First call per launch performs the actual discovery (Tier 1/2/3 per
/// descriptor), then persists. Subsequent calls return the cached pointer
/// instantly. Returns nullptr if the name isn't registered, the host engine
/// hasn't booted enough for the descriptor's anchor to resolve, or the walk
/// failed - caller is expected to handle the null path explicitly.
void* resolve(const std::string& name);

/// Run discovery for every registered descriptor whose offset isn't already
/// cached. Called once from EngineAPI::initialize() after the engine layer
/// is up so resolutions become available without a per-call lazy-init cost
/// later. Logs each result. Safe to call multiple times.
void resolveAllRegistered();

/// Force a re-discovery of one entry (drops both in-memory and file cache
/// entry for that name). For debugging only.
void invalidate(const std::string& name);

/// Drop the entire on-disk + in-memory cache. Wired into the same
/// invalidation path used by the existing scan cache (game update detect).
void clearCache();

} // namespace Hydro::Engine::RawFn
