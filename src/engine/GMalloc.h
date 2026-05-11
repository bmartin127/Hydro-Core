// GMalloc - UE FMalloc discovery and allocation helpers.
#pragma once
#include <cstddef>
#include <cstdint>

namespace Hydro::Engine {

extern void* s_gmalloc;    // UE's global FMalloc*

bool  findGMalloc();
void* gmallocAlloc(size_t size, uint32_t align = 8);

} // namespace Hydro::Engine
