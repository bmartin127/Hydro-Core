#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// Discovery registry for non-reflected C++ engine functions.
// Results persist to hydro_raw_funcs.bin; invalidates on game update.

namespace Hydro::Engine::RawFn {

enum class Strategy : uint8_t {
    UFuncImpl       = 1,   // anchor = UFunction path; reads Func@0xD8
    NthCallInUFunc  = 2,   // anchor = UFunction path; Nth CALL inside Func body
    StringRefAnchor = 3,   // anchor = wide string; walks back to func start
};

struct Descriptor {
    Strategy     strategy;
    std::wstring anchor;
    int          nthCall = 0;  // 1-based; NthCallInUFunc only
};

void registerFn(const std::string& name, const Descriptor& desc);
void* resolve(const std::string& name);
void resolveAllRegistered();
void invalidate(const std::string& name);
void clearCache();

} // namespace Hydro::Engine::RawFn
