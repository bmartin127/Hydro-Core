#include "Shaders.h"
#include "../EngineAPI.h"
#include "../HydroCore.h"
#include "Internal.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include <cstring>

namespace Hydro::Engine {

// Forward decls for cross-module symbols resolved at link time.
void* gmallocAlloc(size_t size, uint32_t align);

void* s_openShaderLibrary = nullptr;

// Discovery: locate FShaderCodeLibrary::OpenLibrary via the function-name
// string UE bakes into the binary via __FUNCTION__ macros.
//
// Single LEA ref to the string in .text on UE 5.6 shipping - no ambiguity.
// If multiple refs ever surface in a future engine version, extend with a
// Tier 2 caller-side xref via the "InitForRuntime" string.
//
// .pdata binary search is used instead of walking back for prologue patterns.
// The prologue walk fails on functions whose first instruction is
// `movaps [rsp+disp32], xmmN` (SSE register save), which is common in
// heavy renderer code - the walker overshots into a sibling function.
bool findOpenShaderLibrary() {
    if (!s_gm.base) return false;

    // __FUNCTION__ emits ASCII, not UTF-16.
    const char* needle = "FShaderCodeLibrary::OpenLibrary";
    size_t needleLen = strlen(needle);
    uint8_t* strAddr = nullptr;
    for (size_t i = 0; i + needleLen <= s_gm.size; i++) {
        if (memcmp(s_gm.base + i, needle, needleLen) == 0) {
            strAddr = s_gm.base + i;
            break;
        }
    }
    if (!strAddr) {
        Hydro::logWarn("EngineAPI: 'FShaderCodeLibrary::OpenLibrary' string not found");
        return false;
    }
    Hydro::logInfo("EngineAPI: 'FShaderCodeLibrary::OpenLibrary' string at exe+0x%zX",
        (size_t)(strAddr - s_gm.base));

    uint8_t* leaAddr = findLeaRef(s_gm.base, s_gm.size, strAddr);
    if (!leaAddr) {
        Hydro::logWarn("EngineAPI: No LEA ref to OpenLibrary string");
        return false;
    }
    Hydro::logInfo("EngineAPI: LEA at exe+0x%zX", (size_t)(leaAddr - s_gm.base));

    auto* dosHeader = (IMAGE_DOS_HEADER*)s_gm.base;
    auto* ntHeaders = (IMAGE_NT_HEADERS*)(s_gm.base + dosHeader->e_lfanew);
    auto& excDir = ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (excDir.VirtualAddress == 0 || excDir.Size == 0) {
        Hydro::logWarn("EngineAPI: PE has no exception directory - cannot resolve OpenLibrary");
        return false;
    }
    auto* funcs = (RUNTIME_FUNCTION*)(s_gm.base + excDir.VirtualAddress);
    size_t numFuncs = excDir.Size / sizeof(RUNTIME_FUNCTION);
    uint32_t leaRva = (uint32_t)(leaAddr - s_gm.base);

    size_t lo = 0, hi = numFuncs;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (leaRva < funcs[mid].BeginAddress) {
            hi = mid;
        } else if (leaRva >= funcs[mid].EndAddress) {
            lo = mid + 1;
        } else {
            uint32_t beginRva = funcs[mid].BeginAddress;
            uint32_t endRva   = funcs[mid].EndAddress;
            s_openShaderLibrary = s_gm.base + beginRva;
            Hydro::logInfo(
                "EngineAPI: FShaderCodeLibrary::OpenLibrary at exe+0x%X (.pdata fn 0x%X-0x%X, size 0x%X bytes)",
                beginRva, beginRva, endRva, endRva - beginRva);
            return true;
        }
    }

    Hydro::logWarn("EngineAPI: LEA at exe+0x%X has no containing .pdata function - bad anchor or stripped PE",
        leaRva);
    return false;
}

void* getOpenShaderLibraryFn() { return s_openShaderLibrary; }

// UE 5.5+ signature: static bool OpenLibrary(FString const&, FString const&, bool bMonolithicOnly = false)
// rcx = &Name, rdx = &Directory, r8b = bMonolithicOnly
bool openShaderLibrary(const wchar_t* libraryName, const wchar_t* mountDir) {
    if (!s_openShaderLibrary || !libraryName || !mountDir) return false;

    // Buffers must be GMalloc-owned: UE's FMemory::Free will be called on them.
    FStringMinimal libString = {};
    FStringMinimal dirString = {};
    if (!buildFString(libString, libraryName) || !buildFString(dirString, mountDir)) {
        Hydro::logWarn("EngineAPI: OpenShaderLibrary skipped - couldn't allocate FString via GMalloc");
        return false;
    }

    Hydro::logInfo("EngineAPI: OpenShaderLibrary(name='%ls' @ %p, dir='%ls' @ %p)",
        libraryName, libString.Data, mountDir, dirString.Data);

    using OpenLibraryFn = bool(__fastcall*)(FStringMinimal*, FStringMinimal*, bool);
    auto fn = (OpenLibraryFn)s_openShaderLibrary;

    bool ok = false;
#ifdef _WIN32
    __try {
        ok = fn(&libString, &dirString, false);
    } __except (1) {
        Hydro::logError("EngineAPI: OpenShaderLibrary CRASHED");
        return false;
    }
#else
    ok = fn(&libString, &dirString, false);
#endif
    Hydro::logInfo("EngineAPI: OpenShaderLibrary returned %s", ok ? "true" : "false");
    return ok;
}

} // namespace Hydro::Engine
