#pragma once

/// Structured Exception Handling helpers for guarded memory reads
/// and blind function calls during engine discovery.
///
/// Engine reflection has to dereference pointers that may or may not
/// be valid (pattern scans, vtable walks, RIP-relative offsets). A
/// single bad pointer would otherwise crash the whole game. These
/// macros wrap each access in Windows SEH so a fault returns false
/// instead of propagating.
///
/// HYDRO_SEH_TRY cannot coexist with C++ exception handling or with
/// local objects that require unwinding in the same function. Keep
/// wrapped functions small and use only primitive types.

#ifdef _WIN32

/// Run `expr`, returning true on success and false on access violation.
/// Usage:
///   static bool safeReadPtr(void* addr, void** out) {
///       HYDRO_SEH_TRY(*out = *(void**)addr);
///   }
#define HYDRO_SEH_TRY(expr)                     \
    __try {                                     \
        expr;                                   \
        return true;                            \
    } __except (1) {                            \
        return false;                           \
    }

/// Like HYDRO_SEH_TRY but runs a failure block before returning false.
/// Used when the caller needs to signal a crash occurred separately
/// from the function's return value (see PakLoader mount calls).
#define HYDRO_SEH_TRY_OR(expr, on_fail)         \
    __try {                                     \
        expr;                                   \
        return true;                            \
    } __except (1) {                            \
        on_fail;                                \
        return false;                           \
    }

#else
// Non-Windows stubs. HydroCore targets Windows x64 (UE5 shipping builds
// run Win64 even under Proton), so these exist purely so editor tooling
// and static analysis can parse the file on Linux/macOS. The on_fail
// block is intentionally unreachable on the success path and would run
// for real if anyone ever ports the engine reflection to a platform
// with an equivalent of SEH.

#define HYDRO_SEH_TRY(expr)                     \
    do {                                        \
        expr;                                   \
        return true;                            \
    } while (0)

#define HYDRO_SEH_TRY_OR(expr, on_fail)         \
    do {                                        \
        expr;                                   \
        return true;                            \
        (void)(on_fail);                        \
    } while (0)

#endif
