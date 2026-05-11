#pragma once

// FFI types for the `patternsleuth_bind` static lib that comes in via UE4SS.
// Anything that needs a pattern-driven resolve (GMalloc, FName, etc.) talks
// to the same `ps_scan` entry point with a config + a results struct.

#include <cstdint>

namespace Hydro::Engine {

struct PsScanConfig {
    bool guobject_array{}; bool fname_tostring{}; bool fname_ctor_wchar{};
    bool gmalloc{};
    bool static_construct_object_internal{}; bool ftext_fstring{};
    bool engine_version{}; bool fuobject_hash_tables_get{};
    bool gnatives{}; bool console_manager_singleton{}; bool gameengine_tick{};
};

struct PsCtx {
    void (*def)(wchar_t*); void (*normal)(wchar_t*); void (*verbose)(wchar_t*);
    void (*warning)(wchar_t*); void (*error)(wchar_t*);
    PsScanConfig config;
};

struct PsEngineVersion { uint16_t major; uint16_t minor; };

struct PsScanResults {
    void* guobject_array; void* fname_tostring; void* fname_ctor_wchar;
    void* gmalloc;
    void* static_construct_object_internal; void* ftext_fstring;
    PsEngineVersion engine_version;
    void* fuobject_hash_tables_get; void* gnatives;
    void* console_manager_singleton; void* gameengine_tick;
};

extern "C" bool ps_scan(PsCtx& ctx, PsScanResults& results);

// No-op log sink. Pass to PsCtx fields when you want patternsleuth to run
// quietly (e.g. during cached-result paths or non-critical resolves).
inline void ps_log_quiet(wchar_t*) {}

} // namespace Hydro::Engine
