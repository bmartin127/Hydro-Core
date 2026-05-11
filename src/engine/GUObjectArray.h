#pragma once

// FUObjectArray discovery and raw object/item accessors.

#include <cstdint>

namespace Hydro::Engine {
    extern void* s_guObjectArray;
    bool findGUObjectArray();
}
