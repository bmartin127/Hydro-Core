#include "GMalloc.h"
#include "../EngineAPI.h"
#include "../HydroCore.h"
#include "Internal.h"
#include "Patternsleuth.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace Hydro::Engine {

void* s_gmalloc = nullptr;

bool findGMalloc() {
    if (s_gmalloc) return true;
    PsCtx ctx{ ps_log_quiet, ps_log_quiet, ps_log_quiet, ps_log_quiet, ps_log_quiet, {} };
    ctx.config.gmalloc = true;
    PsScanResults results{};
    bool ok = false;
#ifdef _WIN32
    __try { ok = ps_scan(ctx, results); }
    __except (1) { ok = false; }
#else
    ok = ps_scan(ctx, results);
#endif
    if (!ok || !results.gmalloc) {
        Hydro::logWarn("EngineAPI: ps_scan didn't return GMalloc");
        return false;
    }
    // results.gmalloc is the address of the GMalloc variable (a FMalloc**).
    // Dereference once to get the actual FMalloc* instance.
    void** gmallocPtr = (void**)results.gmalloc;
    void* gmallocInstance = nullptr;
    if (!safeReadPtr(gmallocPtr, &gmallocInstance) || !gmallocInstance) {
        Hydro::logWarn("EngineAPI: GMalloc variable at %p has null instance", results.gmalloc);
        return false;
    }
    s_gmalloc = gmallocInstance;
    Hydro::logInfo("EngineAPI: GMalloc instance at %p (variable at %p)",
        s_gmalloc, results.gmalloc);
    return true;
}

// FMalloc::Malloc(SIZE_T Size, uint32 Alignment).
// Vtable index varies - FMalloc inherits from FExec so [0]=dtor, then a few
// FExec virtuals, THEN Malloc. Empirically [3] in stock UE 5.5, but probe to
// handle drift across dev/editor/shipping builds.
using FMallocFn = void*(__fastcall*)(void* self, size_t size, uint32_t align);

static int s_mallocVtableIdx = -1;

void* gmallocAlloc(size_t size, uint32_t align) {
    if (!s_gmalloc) {
        if (!findGMalloc()) return nullptr;
    }
    void** vtable = nullptr;
    if (!safeReadPtr(s_gmalloc, (void**)&vtable) || !vtable) return nullptr;

    // Probe slots 2..7 to find Malloc. Per UE 5.5 source (MemoryBase.h + Exec.h),
    // shipping builds have:
    //   vtable[0]  ~FMalloc dtor
    //   vtable[1..5] FExec virtuals (Exec, Exec_Runtime, Exec_Dev, Exec_Editor)
    //   vtable[6]  Malloc(SIZE_T, uint32)        <- what we want
    //   vtable[7]  TryMalloc
    //   vtable[8]  Realloc
    //   vtable[9]  TryRealloc
    //   vtable[10] Free
    //   vtable[26] GetDescriptiveName            <- sanity check anchor
    //
    // Dev/editor builds are one shorter (FExec loses one virtual).
    // Widened to 2..15 - some host binaries shift the Malloc slot outside the
    // stock UE 5.5 range (5..7). The writability check still rejects non-Malloc
    // slots, so widening doesn't risk false positives.
    if (s_mallocVtableIdx < 0) {
        for (int idx = 2; idx <= 15; idx++) {
            auto candidate = (FMallocFn)vtable[idx];
            if (!candidate) continue;
            void* result = nullptr;
#ifdef _WIN32
            __try { result = candidate(s_gmalloc, 16, 8); }
            __except (1) { continue; }
#else
            result = candidate(s_gmalloc, 16, 8);
#endif
            uintptr_t r = (uintptr_t)result;
            if (r <= 0x10000 || r >= 0x7FFFFFFFFFFFULL) continue;
            // Writability check - real heap is RW; garbage pointer values fault.
            bool writable = false;
#ifdef _WIN32
            __try {
                volatile uint8_t* p = (volatile uint8_t*)result;
                uint8_t saved = p[0];
                p[0] = 0xAA;
                if (p[0] == 0xAA) writable = true;
                p[0] = saved;
            } __except (1) { writable = false; }
#endif
            if (!writable) continue;
            s_mallocVtableIdx = idx;
            Hydro::logInfo("EngineAPI: GMalloc::Malloc probed at vtable[%d] (test alloc=%p, writable)",
                idx, result);
            // Test alloc is intentionally leaked - Free hasn't been probed yet.
            break;
        }
        if (s_mallocVtableIdx < 0) {
            Hydro::logWarn("EngineAPI: Failed to probe FMalloc::Malloc in vtable[2..15]");
            return nullptr;
        }
    }

    auto fn = (FMallocFn)vtable[s_mallocVtableIdx];
    void* p = nullptr;
#ifdef _WIN32
    __try { p = fn(s_gmalloc, size, align); }
    __except (1) { p = nullptr; }
#else
    p = fn(s_gmalloc, size, align);
#endif
    return p;
}

// Buffer is GMalloc-owned so UE's FMemory::Free won't crash when it frees it.
bool buildFString(FStringMinimal& out, const wchar_t* src) {
    int len = (int)wcslen(src) + 1;  // include null terminator
    void* buf = gmallocAlloc(len * sizeof(wchar_t), 8);
    if (!buf) return false;
    memcpy(buf, src, len * sizeof(wchar_t));
    out.Data = (wchar_t*)buf;
    out.ArrayNum = len;
    out.ArrayMax = len;
    return true;
}

} // namespace Hydro::Engine
