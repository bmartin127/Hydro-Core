#pragma once

// FShaderCodeLibrary::OpenLibrary discovery and call wrapper.

namespace Hydro::Engine {
    extern void* s_openShaderLibrary;   // FShaderCodeLibrary::OpenLibrary
    bool findOpenShaderLibrary();
}
