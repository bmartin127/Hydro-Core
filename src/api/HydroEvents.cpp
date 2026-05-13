///
/// @module Hydro.Events
/// @description Hook into engine events and UFunction calls.
///   Register callbacks on any UFunction. When the engine calls that
///   function, your Lua callback fires with the target object.
///
///   Two x64 inline detours are installed into the engine's function
///   dispatch pipeline: one on UObject::ProcessEvent and one on
///   UObject::ProcessInternal. Some Blueprint event dispatch paths
///   (including generated C++ stubs for BlueprintImplementableEvent
///   events like ReceiveBeginPlay) enter via ProcessInternal rather
///   than the outer ProcessEvent, so we have to catch both to hook
///   actor lifecycle events reliably. No UFunction struct is mutated,
///   so the base Actor's BeginPlay dispatch stays intact and character
///   animation blueprints keep working.
///
///   For game-specific events, prefer dedicated Tier 2 modules.
///   See CONTRIBUTING_TIER2.md.
///
/// @depends EngineAPI (ProcessEvent address, findObject, UFUNC_FUNC)
/// @engine_systems UObject::ProcessEvent, UObject::ProcessInternal
///

#include "HydroEvents.h"
#include "ModuleRegistry.h"
#include "../HydroCore.h"
#include "../EngineAPI.h"
#include "../LuaUObject.h"

extern "C" {
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>
}

#ifdef _WIN32
#include <windows.h>
#endif

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <atomic>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include <Zydis/Zydis.h>
#include <vector>

namespace Hydro::API {

// Hook registry
//
// Hooks are keyed by the FName index of the UFunction's name, not by its
// UFunction* pointer. This matters because Blueprint subclasses generate
// their own per-class override UFunctions - e.g., hooking
// "/Script/Engine.Actor:ReceiveBeginPlay" should also fire when a BP
// subclass's own ReceiveBeginPlay override runs. Matching by pointer
// would miss every subclass override. Matching by FName index catches
// all of them: every UFunction with the name "ReceiveBeginPlay", no
// matter which class owns it.

struct HookCallback {
    lua_State* L;
    int callbackRef;
    std::string modId;
    std::string funcPath; // original path for diagnostics
};

struct HookedName {
    std::string firstPath;
    std::vector<HookCallback> callbacks;
};

// FName index -> hook data. Accessed from the game thread during detour
// dispatch and from Lua during hook(). The mutex protects registration;
// the detour snapshots callbacks under the lock then fires them outside.
static std::unordered_map<uint32_t, HookedName> s_hooks;
static std::mutex s_hooksMutex;
static bool s_hookInstalled = false;

// Trace listener registry
//
// A trace logs every PE/PI/BP call on a single target UObject for a fixed
// number of seconds. Used by Hydro.Reflect.trace() to build live event
// sequence recordings that Slice 3's scaffolder feeds into module
// templates (e.g., "when a creature spawns, these events fire in this
// order").
//
// Piggybacks on the existing inline hook detours - does NOT install a
// second inline prologue patch (two inline hooks on the same function
// don't compose cleanly). Each detour checks s_traces.empty() as a fast
// path before taking the trace mutex.

struct TraceListener {
    void* target;               // UObject instance being watched
    uint64_t expireTickCount;   // absolute tick count at which this trace ends
    std::string outPath;
    std::ofstream out;
    uint64_t eventsLogged;
    uint64_t startTickCount;    // for relative timestamps in the output
};

// std::vector so empty() is one load and iteration is pointer-walk friendly.
// Use unique_ptr because TraceListener has a non-copyable std::ofstream.
static std::vector<std::unique_ptr<TraceListener>> s_traces;
static std::mutex s_tracesMutex;
// Monotonic counter incremented once per tickTraces() call. Not a true game
// tick - it's "how many post-tick callbacks have fired since DLL load."
// Used as a relative timestamp in trace output and for expiring listeners.
static uint64_t s_tickCounter = 0;

// FName index is stored at the UObjectBase::NamePrivate field. On UE5.5
// that's offset 0x18 from the UObject base for a ComparisonIndex (uint32).
constexpr int UOBJECT_NAME_OFFSET = 0x18;

static uint32_t readNameIdx(void* ufunc) {
    if (!ufunc) return 0;
    uint32_t idx = 0;
    Engine::readInt32((uint8_t*)ufunc + UOBJECT_NAME_OFFSET, (int32_t*)&idx);
    return idx;
}

// Inline hook state
//
// We install two separate inline hooks: one on ProcessEvent (outer dispatch
// entry point for script calls), and one on ProcessInternal (the inner VM
// entry point that BP events sometimes bypass straight to). Both detours
// share the same hook map - each one reads the UFunction from its argument
// frame and looks up matching hooks by FName index.

using ProcessEventFn = void(__fastcall*)(void*, void*, void*);
using ProcessInternalFn = void(__fastcall*)(void*, void*, void*);
using BeginPlayFn = void(__fastcall*)(void*);

static ProcessEventFn s_peTrampoline = nullptr;
static ProcessInternalFn s_piTrampoline = nullptr;
static BeginPlayFn s_bpTrampoline = nullptr;
static bool s_peHooked = false;
static bool s_piHooked = false;
static bool s_bpHooked = false;

// Cached ReceiveBeginPlay UFunction so the BeginPlay vtable-hook detour
// can synthesize a fireHooksForFunction call using the same FName index
// that mods registered via Events.hook("/Script/Engine.Actor:ReceiveBeginPlay").
static void* s_receiveBeginPlayFunc = nullptr;

// FFrame::Node offset - the UFunction pointer embedded in the execution
// frame passed to ProcessInternal. UE5.5 layout.
constexpr int FFRAME_NODE_OFFSET = 0x10;

// Minimal x64 length decoder

// Decode and relocate a function prologue using Zydis. Copies instructions
// from `src` to `dst` one at a time until at least `minBytes` have been
// consumed, fixing up rip-relative displacements so the copied code runs
// correctly from its new location. Returns bytes consumed from `src`, or
// 0 on failure (undecodable, rip-relative beyond ±2GB, etc.).
//
// This replaces the hand-rolled opcode table that only handled ~10 opcodes.
// Zydis decodes the full x86-64 ISA, so prologues using any valid
// instruction (including calls, jumps, rip-relative loads) work.
static int relocatePrologue(const uint8_t* src, uint8_t* dst, int minBytes) {
    ZydisDecoder decoder;
    if (ZYAN_FAILED(ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64))) {
        logError("[Hydro.Events] Zydis decoder init failed");
        return 0;
    }

    int pos = 0;
    const int kMaxPrologue = 64;
    while (pos < minBytes) {
        if (pos >= kMaxPrologue) {
            logError("[Hydro.Events] Prologue exceeded %d bytes without reaching min %d",
                     kMaxPrologue, minBytes);
            return 0;
        }

        ZydisDecodedInstruction inst;
        ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
        if (ZYAN_FAILED(ZydisDecoderDecodeFull(&decoder, src + pos, kMaxPrologue - pos,
                                                &inst, ops))) {
            logError("[Hydro.Events] Zydis failed to decode instruction at +%d (first byte 0x%02X)",
                     pos, src[pos]);
            return 0;
        }

        // Copy the raw bytes. Fixup happens after, in-place.
        memcpy(dst + pos, src + pos, inst.length);

        // If this instruction has a rip-relative operand (disp or imm), the
        // value in the encoded bytes is an offset from the instruction's
        // *original* end address. When we relocate to `dst`, the target
        // address needs the same end address, so we adjust the displacement
        // by the difference between old and new instruction positions.
        if (inst.attributes & ZYDIS_ATTRIB_IS_RELATIVE) {
            int64_t shift = (int64_t)src - (int64_t)dst;  // constant per-call; shift>0 if dst is earlier

            // Displacement (e.g. MOV rax, [rip+disp32])
            if (inst.raw.disp.size > 0) {
                int64_t newDisp = (int64_t)inst.raw.disp.value + shift;
                if (inst.raw.disp.size == 32) {
                    if (newDisp < INT32_MIN || newDisp > INT32_MAX) {
                        logError("[Hydro.Events] rip-relative disp32 out of range after relocate "
                                 "(orig=%lld new=%lld shift=%lld) at +%d",
                                 (long long)inst.raw.disp.value, (long long)newDisp, (long long)shift, pos);
                        return 0;
                    }
                    int32_t v = (int32_t)newDisp;
                    memcpy(dst + pos + inst.raw.disp.offset, &v, 4);
                } else {
                    // 8/16-bit displacements can't span the trampoline distance.
                    logError("[Hydro.Events] rip-relative %u-bit disp not supported at +%d",
                             inst.raw.disp.size, pos);
                    return 0;
                }
            }

            // Immediate (e.g. E8 call rel32, E9 jmp rel32, Jcc rel8/32)
            if (inst.raw.imm[0].is_relative && inst.raw.imm[0].size > 0) {
                int64_t newImm = inst.raw.imm[0].value.s + shift;
                if (inst.raw.imm[0].size == 32) {
                    if (newImm < INT32_MIN || newImm > INT32_MAX) {
                        logError("[Hydro.Events] relative imm32 out of range after relocate "
                                 "(orig=%lld new=%lld) at +%d",
                                 (long long)inst.raw.imm[0].value.s, (long long)newImm, pos);
                        return 0;
                    }
                    int32_t v = (int32_t)newImm;
                    memcpy(dst + pos + inst.raw.imm[0].offset, &v, 4);
                } else {
                    // A short conditional jump (rel8) in a prologue would need
                    // promotion to rel32 - out of scope for now.
                    logError("[Hydro.Events] relative imm%u-bit not supported at +%d",
                             inst.raw.imm[0].size, pos);
                    return 0;
                }
            }
        }

        pos += inst.length;
    }
    return pos;
}

// Install/uninstall the inline hooks

// Detours defined further down, forward declared so installers can take their address.
static void __fastcall hydroProcessEventDetour(void* obj, void* func, void* params);
static void __fastcall hydroProcessInternalDetour(void* obj, void* stack, void* result);

// Follow trampoline jmps to find the real function body. Handles the two
// forms we typically see from incremental linkers and PLT thunks:
//   E9 XX XX XX XX              jmp rel32
//   FF 25 XX XX XX XX  <ptr>    jmp qword [rip+disp32]
// Stops when the first instruction is anything else. Recurses through
// chained jmps (up to 8 levels - enough for any real trampoline chain).
static void* resolveJmpTrampoline(void* addr, int depth = 0) {
    if (!addr || depth >= 8) return addr;
    const uint8_t* p = (const uint8_t*)addr;
    if (p[0] == 0xE9) {
        int32_t rel = *(const int32_t*)(p + 1);
        void* target = (uint8_t*)addr + 5 + rel;
        return resolveJmpTrampoline(target, depth + 1);
    }
    if (p[0] == 0xFF && p[1] == 0x25) {
        int32_t disp = *(const int32_t*)(p + 2);
        void** slot = (void**)((uint8_t*)addr + 6 + disp);
        void* target = nullptr;
        if (Engine::readPtr(slot, &target) && target) {
            return resolveJmpTrampoline(target, depth + 1);
        }
    }
    return addr;
}

// Core inline-hook installer. Target gets a 14-byte absolute jmp to the
// detour; an executable trampoline is allocated that runs the original
// prologue bytes and jumps back to target+N. Returns the trampoline
// pointer on success, nullptr on failure.
static void* installInlineHookAt(void* target, void* detour, const char* label) {
    if (!target) {
        logError("[Hydro.Events] %s address not available", label);
        return nullptr;
    }

    // Follow simple thunks. MSVC sometimes emits the function entry as a
    // jmp to the real implementation (especially when /INCREMENTAL is on or
    // for imported symbols). Copying a jmp with rip-relative addressing
    // into a trampoline would break the relative reference, so resolve it
    // to the real target address and hook THAT instead.
    const uint8_t* bytes = (const uint8_t*)target;
    for (int iterations = 0; iterations < 4; iterations++) {
        if (bytes[0] == 0xE9) {
            // jmp rel32: target = pc + 5 + rel32
            int32_t rel = *(int32_t*)(bytes + 1);
            void* followed = (uint8_t*)target + 5 + rel;
            logInfo("[Hydro.Events] %s at %p is E9 thunk → %p", label, target, followed);
            target = followed;
            bytes = (const uint8_t*)target;
            continue;
        }
        if (bytes[0] == 0xFF && bytes[1] == 0x25) {
            // jmp [rip+rel32]: real addr = *(pc + 6 + rel32)
            int32_t rel = *(int32_t*)(bytes + 2);
            void** slot = (void**)((uint8_t*)target + 6 + rel);
            void* followed = *slot;
            logInfo("[Hydro.Events] %s at %p is FF 25 thunk → %p", label, target, followed);
            target = followed;
            bytes = (const uint8_t*)target;
            continue;
        }
        break;
    }

    // Pre-allocate a trampoline of generous size. We decode instructions
    // *into* the trampoline (via relocatePrologue) so rip-relative fixups
    // can land directly in the final executable location - this matters
    // because the fixup math uses the trampoline's address as the new PC.
    //
    // The trampoline MUST land within ±2GB of `target`: prologue bytes may
    // contain rip-relative instructions whose disp32/imm32 fields would
    // otherwise overflow when relocated. VirtualAlloc's default placement
    // picks arbitrary high addresses far outside that range.
    const size_t trampolineSize = 64 + 14;  // 64 bytes for prologue + 14 for jmp-back
    uint8_t* tramp = nullptr;
    {
        uintptr_t t = (uintptr_t)target;
        const uintptr_t kGranularity = 0x10000;  // 64KB, Windows alloc granularity
        const uintptr_t kMaxSearch = (uintptr_t)0x70000000;  // ~1.75GB - safely inside ±2GB
        // Probe addresses just below `target` first, then just above. Most
        // game modules map in the low half of the 64-bit address space, so
        // searching downward usually hits free regions quickly.
        for (uintptr_t offset = kGranularity; offset < kMaxSearch && !tramp; offset += kGranularity) {
            if (t > offset) {
                tramp = (uint8_t*)VirtualAlloc((void*)(t - offset), trampolineSize,
                    MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
            }
            if (!tramp) {
                tramp = (uint8_t*)VirtualAlloc((void*)(t + offset), trampolineSize,
                    MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
            }
        }
    }
    if (!tramp) {
        // Fallback: any address. Zydis will refuse to install if it needs
        // to copy rip-relative bytes that can't be fixed up from this
        // location, so behavior stays safe - we just lose the ability to
        // hook a few functions that would have worked with a nearby tramp.
        tramp = (uint8_t*)VirtualAlloc(
            nullptr, trampolineSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    }
    if (!tramp) {
        logError("[Hydro.Events] Failed to allocate %s trampoline", label);
        return nullptr;
    }

    int savedBytes = relocatePrologue((const uint8_t*)target, tramp, 14);
    if (savedBytes == 0 || savedBytes > 64) {
        logError("[Hydro.Events] Could not decode/relocate %s prologue", label);
        VirtualFree(tramp, 0, MEM_RELEASE);
        return nullptr;
    }

    // Append absolute jmp back to target + savedBytes
    uint64_t returnAddr = (uint64_t)target + savedBytes;
    tramp[savedBytes + 0] = 0xFF;
    tramp[savedBytes + 1] = 0x25;
    tramp[savedBytes + 2] = 0x00;
    tramp[savedBytes + 3] = 0x00;
    tramp[savedBytes + 4] = 0x00;
    tramp[savedBytes + 5] = 0x00;
    memcpy(tramp + savedBytes + 6, &returnAddr, sizeof(uint64_t));

    DWORD oldProtect = 0;
    if (!VirtualProtect(target, 14, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        logError("[Hydro.Events] VirtualProtect on %s failed", label);
        VirtualFree(tramp, 0, MEM_RELEASE);
        return nullptr;
    }

    uint8_t* t = (uint8_t*)target;
    uint64_t detourAddr = (uint64_t)detour;
    t[0] = 0xFF;
    t[1] = 0x25;
    t[2] = 0x00;
    t[3] = 0x00;
    t[4] = 0x00;
    t[5] = 0x00;
    memcpy(t + 6, &detourAddr, sizeof(uint64_t));

    VirtualProtect(target, 14, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), target, 14);

    logInfo("[Hydro.Events] Inline hook installed on %s at %p (saved %d bytes, trampoline at %p)",
            label, target, savedBytes, tramp);
    return tramp;
}

// Discover ProcessInternal via UE4SS's ExecuteUbergraph trick.
//
// ProcessInternal isn't exposed via a vtable entry we can pattern-match
// easily. But every UFunction has a `Func` native pointer at offset 0xD8,
// and the `/Script/CoreUObject.Object:ExecuteUbergraph` UFunction's Func
// pointer IS ProcessInternal - that's how the engine wires BP script
// dispatch. Read it out, follow any jmp trampolines, done.
static void* discoverProcessInternal() {
    void* ubergraph = Engine::findObject(L"/Script/CoreUObject.Object:ExecuteUbergraph");
    if (!ubergraph) {
        logError("[Hydro.Events] ExecuteUbergraph UFunction not found");
        return nullptr;
    }
    void* funcPtr = nullptr;
    if (!Engine::readPtr((uint8_t*)ubergraph + Engine::UFUNC_FUNC, &funcPtr) || !funcPtr) {
        logError("[Hydro.Events] ExecuteUbergraph Func pointer unreadable");
        return nullptr;
    }
    void* resolved = resolveJmpTrampoline(funcPtr);
    logInfo("[Hydro.Events] ProcessInternal at %p (via ExecuteUbergraph.Func=%p)",
            resolved, funcPtr);
    return resolved;
}

// Hook AActor::BeginPlay via vtable swap. BeginPlay is a C++ virtual
// that the engine calls directly (no ProcessEvent). The vtable offset
// is 0x3A0 on UE 5.5 - we probe a small window around it.
//
// We use a vtable swap (replacing the pointer in the vtable) rather
// than an inline hook (patching the function's first bytes) because
// UE4SS may have already inline-hooked BeginPlay. Two inline hooks
// on the same function don't compose cleanly, but a vtable swap
// chains through naturally: engine → our detour → original vtable
// entry (possibly UE4SS's detour) → UE4SS trampoline → real function.
static void __fastcall hydroBeginPlayDetour(void* actor);

static bool installBeginPlayVtableSwap() {
    // Name is historical - we don't patch vtables, we inline-hook the
    // function pointer stored at AActor's CDO vtable slot 0x3A0 (UE 5.5).
    //
    // This is the same technique UE4SS uses (resolve via Default__Actor
    // CDO, follow jmp thunks, inline-hook the resolved address) and what
    // every mature UE5 mod loader converges on: Palworld, Hogwarts, Ark
    // Ascended, Satisfactory community tooling all target this exact path.
    //
    // Why it catches BP actors even though AActor::BeginPlay is virtual:
    //   BP_ThirdPersonCharacter::BeginPlay → Super → ACharacter::BeginPlay
    //   → Super → APawn::BeginPlay → Super → AActor::BeginPlay (HOOK HERE)
    //   → ReceiveBeginPlay → ProcessEvent → BP event graph
    // Only AActor::BeginPlay calls ReceiveBeginPlay, so every well-behaved
    // actor chain reaches our hook. Subclasses that override BeginPlay
    // without calling Super are the one legal-but-rare pattern we miss.
    //
    // Resolving off the AActor CDO (not any subclass) is essential:
    // BP_ThirdPersonCharacter's vtable slot may contain ACharacter::BeginPlay,
    // but AActor's own CDO vtable gives us AActor's implementation - the
    // one every Super::BeginPlay() chain lands on.
    //
    // Our earlier attempts failed silently because the hand-rolled prologue
    // decoder couldn't handle a `0xE8 call rel32` 13 bytes into BeginPlay's
    // body and fell back to vtable offset 0x3A8 (a different, unrelated
    // function). Zydis + near-target trampoline allocation fix that.

    void* actorCDO = Engine::findObject(L"/Script/Engine.Default__Actor");
    if (!actorCDO) {
        logWarn("[Hydro.Events] AActor CDO not found, BeginPlay hook unavailable");
        return false;
    }

    void** vtable = nullptr;
    if (!Engine::readPtr(actorCDO, (void**)&vtable) || !vtable) {
        logWarn("[Hydro.Events] AActor CDO vtable unreadable");
        return false;
    }

    // Try the known UE 5.5 offset first, then a few neighbors for version
    // resilience. We only use this to anchor the DispatchBeginPlay scan -
    // the BeginPlay vtable slot itself is never hooked.
    static const int kCandidates[] = {
        0x3A0, 0x398, 0x3A8, 0x390, 0x3B0
    };

    for (int off : kCandidates) {
        void* fn = nullptr;
        if (!Engine::readPtr((uint8_t*)vtable + off, &fn) || !fn) continue;

        void* tramp = installInlineHookAt(fn,
            (void*)&hydroBeginPlayDetour, "AActor::BeginPlay");
        if (!tramp) {
            logWarn("[Hydro.Events] Inline hook on AActor::BeginPlay failed at vtable offset 0x%X (fn=%p)",
                    off, fn);
            continue;
        }

        s_bpTrampoline = (BeginPlayFn)tramp;
        logInfo("[Hydro.Events] AActor::BeginPlay inline hook installed (vtable offset 0x%X, fn=%p, detour=%p)",
                off, fn, (void*)&hydroBeginPlayDetour);
        return true;
    }

    logWarn("[Hydro.Events] Could not locate AActor::BeginPlay via any vtable offset");
    return false;
}

static bool installInlineHook() {
    // Diagnostic gates: HYDRO_SKIP_PE_HOOK / HYDRO_SKIP_PI_HOOK = "1" disables
    // the respective inline hook. Used to isolate whether our detour code is
    // the crash source for specific UFunction dispatches (e.g., spawn on
    // UE 5.5 DMG). Default: hooks installed (existing behavior).
    bool skipPE = false, skipPI = false;
    if (char* v = std::getenv("HYDRO_SKIP_PE_HOOK")) skipPE = (v[0] == '1');
    if (char* v = std::getenv("HYDRO_SKIP_PI_HOOK")) skipPI = (v[0] == '1');
    if (skipPE) logWarn("[Hydro.Events] ProcessEvent hook DISABLED via HYDRO_SKIP_PE_HOOK=1");
    if (skipPI) logWarn("[Hydro.Events] ProcessInternal hook DISABLED via HYDRO_SKIP_PI_HOOK=1");

    // Install ProcessEvent hook
    if (!s_peHooked && !skipPE) {
        void* peTarget = Engine::getProcessEventAddress();
        void* peTramp = installInlineHookAt(peTarget,
            (void*)&hydroProcessEventDetour, "ProcessEvent");
        if (peTramp) {
            s_peTrampoline = (ProcessEventFn)peTramp;
            s_peHooked = true;
        }
    }

    // Install ProcessInternal hook - catches BP dispatches that bypass PE
    if (!s_piHooked && !skipPI) {
        void* piTarget = discoverProcessInternal();
        void* piTramp = installInlineHookAt(piTarget,
            (void*)&hydroProcessInternalDetour, "ProcessInternal");
        if (piTramp) {
            s_piTrampoline = (ProcessInternalFn)piTramp;
            s_piHooked = true;
        }
    }

    // Install AActor::BeginPlay hook via vtable swap. Catches the C++
    // virtual that the engine calls directly without going through PE
    // or PI. Uses vtable swap instead of inline hook because UE4SS may
    // have already inline-hooked BeginPlay; vtable swaps compose cleanly
    // with existing inline hooks.
    if (!s_bpHooked) {
        // Cache the ReceiveBeginPlay UFunction so the detour can
        // synthesize a fireHooksForFunction call with the right FName.
        if (!s_receiveBeginPlayFunc) {
            s_receiveBeginPlayFunc = Engine::findObject(
                L"/Script/Engine.Actor:ReceiveBeginPlay");
        }

        if (installBeginPlayVtableSwap()) {
            s_bpHooked = true;
        }
    }

    s_hookInstalled = s_peHooked || s_piHooked || s_bpHooked;
    return s_hookInstalled;
}

// Trace dispatch: iterate active traces, write a JSONL line for each
// entry whose target matches `obj`. Fast-path exits on empty list so
// the common case (no active traces) is a single load+branch.
//
// Called from all three detours (PE, PI, BP) after the hook dispatch.
// The `src` tag lets us distinguish PE/PI/BP in the trace output so
// tools downstream can reason about which dispatch path fired.
static void fireTracesForObject(const char* src, void* obj, void* func) {
    if (s_traces.empty()) return;

    std::lock_guard<std::mutex> lock(s_tracesMutex);
    if (s_traces.empty()) return; // re-check under the lock

    // Resolve the function name once for any matching trace. Cheap
    // (FNamePool direct read) so we can afford to do it per fire when
    // any trace is active.
    const char* funcName = nullptr;
    std::string funcNameBuf;
    uint32_t funcNameIdx = func ? readNameIdx(func) : 0;

    for (auto& t : s_traces) {
        if (t->target != obj) continue;
        if (!t->out.is_open()) continue;

        if (!funcName) {
            if (funcNameIdx != 0) {
                funcNameBuf = Engine::getNameString(funcNameIdx);
            }
            funcName = funcNameBuf.empty() ? "<unknown>" : funcNameBuf.c_str();
        }

        uint64_t rel = s_tickCounter - t->startTickCount;
        // One JSONL line per event. Fields kept short for file size.
        t->out << "{\"t\":" << rel
               << ",\"src\":\"" << src
               << "\",\"func\":\"" << funcName
               << "\",\"obj\":\"0x" << std::hex << (uintptr_t)obj << std::dec << "\"}\n";
        t->eventsLogged++;

        // Flush every 16 events to survive crashes but not thrash.
        if ((t->eventsLogged & 0xF) == 0) t->out.flush();
    }
}

bool addTrace(void* target, int seconds, const std::string& outPath) {
    if (!target || seconds <= 0 || outPath.empty()) return false;

    // Ensure the inline detours are installed. If Lua hook() has already
    // been called this is a no-op; otherwise we install now so the detour
    // fires for this trace.
    if (!s_hookInstalled) {
        if (!installInlineHook()) {
            logError("[Hydro.Events] addTrace: detours not installable");
            return false;
        }
    }

    auto t = std::make_unique<TraceListener>();
    t->target = target;
    t->outPath = outPath;
    t->eventsLogged = 0;

    // Ensure parent directory exists
    try {
        std::filesystem::path p(outPath);
        if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path());
    } catch (...) {}

    t->out.open(outPath, std::ios::out | std::ios::trunc);
    if (!t->out.is_open()) {
        logError("[Hydro.Events] addTrace: failed to open %s", outPath.c_str());
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(s_tracesMutex);
        t->startTickCount = s_tickCounter;
        // Assume ~60 tps (conservative - we tick on engine post-tick callback).
        t->expireTickCount = s_tickCounter + (uint64_t)seconds * 60;
        s_traces.push_back(std::move(t));
    }

    logInfo("[Hydro.Events] trace installed: target=%p seconds=%d path=%s",
            target, seconds, outPath.c_str());
    return true;
}

// Forward-declared near the top of the file? No - pumpCoroutines is defined
// after tickEvents in source order. Declare it here so tickEvents can call it.
static void pumpCoroutines();

// Engine-tick-driven scheduler pump. The PE detour also pumps via the same
// pumpCoroutines() helper, but that only fires when a UFunction is called
// through ProcessEvent - pre-world-load on Palworld, PE is dormant for
// seconds at a time and waiting Lua scripts never resume. Driving the same
// pump from RegisterEngineTickPostCallback (which fires every frame) keeps
// `wait()`-based coroutines progressing regardless of UFunction traffic.
void tickEvents() {
    pumpCoroutines();
}

void tickTraces() {
    s_tickCounter++;

    if (s_traces.empty()) return;

    std::lock_guard<std::mutex> lock(s_tracesMutex);
    if (s_traces.empty()) return;

    // Remove expired traces
    auto it = s_traces.begin();
    while (it != s_traces.end()) {
        auto& t = *it;
        if (s_tickCounter >= t->expireTickCount) {
            if (t->out.is_open()) {
                t->out.flush();
                t->out.close();
            }
            logInfo("[Hydro.Events] trace expired: target=%p events=%llu path=%s",
                    t->target, (unsigned long long)t->eventsLogged, t->outPath.c_str());
            it = s_traces.erase(it);
        } else {
            ++it;
        }
    }
}

// The detour

// Shared callback dispatch: fire every hook whose FName index matches.
// Called by both detours with the UFunction resolved from their own
// argument shape. The source tag lets us log which detour a matched
// hook came through - useful for verifying both dispatch paths work.
static void fireHooksForFunction(const char* source, void* obj, void* func) {
    // Raw counter - tracks EVERY detour call regardless of name resolution.
    // Written to disk periodically so we can distinguish "detour is dead"
    // from "detour is alive but only sees already-seen names".
    static std::atomic<uint64_t> s_peCount{0};
    static std::atomic<uint64_t> s_piCount{0};
    static std::atomic<uint64_t> s_bpCount{0};
    std::atomic<uint64_t>* counter =
        source[0] == 'P' ? (source[1] == 'E' ? &s_peCount : &s_piCount) : &s_bpCount;
    uint64_t n = counter->fetch_add(1) + 1;
    if (n % 100 == 1) {
        // Write every 100th call to a heartbeat file with flush, so we can
        // confirm the detour keeps firing during gameplay.
        FILE* f = fopen("HydroEvents-heartbeat.log", "w");
        if (f) {
            fprintf(f, "PE=%llu PI=%llu BP=%llu\n",
                    (unsigned long long)s_peCount.load(),
                    (unsigned long long)s_piCount.load(),
                    (unsigned long long)s_bpCount.load());
            fflush(f);
            fclose(f);
        }
    }

    uint32_t nameIdx = readNameIdx(func);
    if (nameIdx == 0) return;

    // One-shot diagnostic: log each distinct (source, nameIdx) the first
    // time we see it. Proves the detour is actually being called and
    // shows which function names flow through - useful for diagnosing
    // "my hook never fires" issues (wrong nameIdx, wrong detour path).
    //
    // Writes to a dedicated HydroEvents-diag.log with flush-per-write so
    // the data survives hard game termination. The main UE4SS log buffers
    // and loses anything in-flight when the game is killed or crashes.
    static std::mutex s_firstSeenMutex;
    static std::unordered_set<uint64_t> s_firstSeen;
    static FILE* s_diagFile = nullptr;
    uint64_t key = ((uint64_t)source[0] << 32) | nameIdx;
    {
        std::lock_guard<std::mutex> lock(s_firstSeenMutex);
        if (s_firstSeen.insert(key).second) {
            logInfo("[Hydro.Events] %s detour first-seen nameIdx=0x%X (hooks.size=%zu)",
                    source, nameIdx, s_hooks.size());
            if (!s_diagFile) {
                s_diagFile = fopen("HydroEvents-diag.log", "w");
            }
            if (s_diagFile) {
                fprintf(s_diagFile, "%s nameIdx=0x%X (hooks=%zu)\n",
                        source, nameIdx, s_hooks.size());
                fflush(s_diagFile);
            }
        }
    }

    std::vector<HookCallback> callbacksCopy;
    {
        std::lock_guard<std::mutex> lock(s_hooksMutex);
        auto it = s_hooks.find(nameIdx);
        if (it != s_hooks.end()) {
            callbacksCopy = it->second.callbacks;
        }
    }

    if (callbacksCopy.empty()) return;

    // A matched hook is the interesting signal - log which detour it came
    // through so we can confirm PE vs PI dispatch end-to-end.
    logInfo("[Hydro.Events] %s detour matched hook (nameIdx=0x%X, %d callbacks)",
            source, nameIdx, (int)callbacksCopy.size());

    for (const auto& cb : callbacksCopy) {
        if (cb.callbackRef == LUA_NOREF || !cb.L) continue;
        lua_rawgeti(cb.L, LUA_REGISTRYINDEX, cb.callbackRef);
        if (lua_isfunction(cb.L, -1)) {
            Lua::pushUObject(cb.L, obj);
            int err = lua_pcall(cb.L, 1, 0, 0);
            if (err != 0) {
                const char* msg = lua_tostring(cb.L, -1);
                logError("[Hydro.Events] Hook error (%s): %s",
                         cb.modId.c_str(), msg ? msg : "unknown");
                lua_pop(cb.L, 1);
            }
        } else {
            lua_pop(cb.L, 1);
        }
    }
}

// Forward decl - defined later in the file near the Lua bindings.
static void pumpCoroutines();

static void __fastcall hydroProcessEventDetour(void* obj, void* func, void* params) {
    // Fast path: no hooks registered and no traces active.
    if (!s_hooks.empty()) {
        fireHooksForFunction("PE", obj, func);
    }
    if (!s_traces.empty()) {
        fireTracesForObject("PE", obj, func);
    }
    // Coroutine waits piggyback on PE firing - same thread as the engine,
    // so Lua is safe. Non-yielding scripts pay no cost here (empty list).
    pumpCoroutines();
    s_peTrampoline(obj, func, params);
}

// ProcessInternal detour - signature is (UObject* this, FFrame& Stack, void* result).
// The UFunction being executed lives in FFrame::Node somewhere near the
// start of the struct after the FOutputDevice base. Offset varies by UE
// version and compiler padding; we probe a few candidate slots on the
// first call to find whichever yields a non-null pointer with a valid
// FName index, then lock that offset in for all subsequent calls.
static void __fastcall hydroProcessInternalDetour(void* obj, void* stack, void* result) {
    // Fast path: nothing to dispatch at all.
    if (s_hooks.empty() && s_traces.empty()) {
        s_piTrampoline(obj, stack, result);
        return;
    }

    static int s_nodeOffset = -1;

    void* func = nullptr;
    if (stack) {
        if (s_nodeOffset >= 0) {
            Engine::readPtr((uint8_t*)stack + s_nodeOffset, &func);
        } else {
            // First call: probe candidate Node offsets until one reads a
            // plausible UFunction pointer. Locks in for the rest of the run.
            static const int kCandidates[] = { 0x10, 0x18, 0x20, 0x28, 0x08 };
            for (int off : kCandidates) {
                void* candidate = nullptr;
                if (!Engine::readPtr((uint8_t*)stack + off, &candidate)) continue;
                if (!candidate) continue;
                uint32_t nameIdx = readNameIdx(candidate);
                if (nameIdx != 0 && nameIdx < 0x200000) {
                    s_nodeOffset = off;
                    func = candidate;
                    logInfo("[Hydro.Events] PI FFrame::Node locked in at offset 0x%X",
                            off);
                    break;
                }
            }
        }
    }

    if (func) {
        if (!s_hooks.empty()) fireHooksForFunction("PI", obj, func);
        if (!s_traces.empty()) fireTracesForObject("PI", obj, func);
    }
    s_piTrampoline(obj, stack, result);
}

// AActor::BeginPlay detour. This is a bare C++ virtual with signature
// void(AActor* this) - no UFunction dispatch involved, which is why
// the PE/PI hooks can't catch it. We synthesize a fireHooksForFunction
// call using the cached ReceiveBeginPlay UFunction so that mods which
// hooked "ReceiveBeginPlay" by name fire here too.
static void __fastcall hydroBeginPlayDetour(void* actor) {
    // Lazy-resolve ReceiveBeginPlay UFunction: it may not have existed in
    // GUObjectArray at hook-install time (engine loads UFunctions with the
    // engine assets). Retry on every call until found, then stop.
    if (!s_receiveBeginPlayFunc) {
        s_receiveBeginPlayFunc = Engine::findObject(
            L"/Script/Engine.Actor:ReceiveBeginPlay");
        static bool s_loggedResolved = false;
        if (s_receiveBeginPlayFunc && !s_loggedResolved) {
            s_loggedResolved = true;
            logInfo("[Hydro.Events] BP detour: lazy-resolved ReceiveBeginPlay UFunction at %p",
                    s_receiveBeginPlayFunc);
        }
    }

    // One-shot diagnostic: log the first time this detour fires regardless
    // of whether we have a UFunction to synthesize with. Confirms the
    // inline hook is actually being hit by the engine. Writes to a
    // dedicated file with immediate flush so the message survives crashes.
    static bool s_loggedFirstHit = false;
    if (!s_loggedFirstHit) {
        s_loggedFirstHit = true;
        logInfo("[Hydro.Events] BP detour fired for first time (actor=%p, s_receiveBeginPlayFunc=%p)",
                actor, s_receiveBeginPlayFunc);
        FILE* f = fopen("HydroEvents-bp.log", "w");
        if (f) {
            fprintf(f, "BP detour fired actor=%p s_receiveBeginPlayFunc=%p\n",
                    actor, s_receiveBeginPlayFunc);
            fflush(f);
            fclose(f);
        }
    }

    if (!s_hooks.empty() && s_receiveBeginPlayFunc) {
        fireHooksForFunction("BP", actor, s_receiveBeginPlayFunc);
    }
    if (!s_traces.empty()) {
        fireTracesForObject("BP", actor, s_receiveBeginPlayFunc);
    }
    s_bpTrampoline(actor);
}

// Lookup: read _MOD_ID from the calling Lua chunk's environment

static std::string getCallerModId(lua_State* L) {
    lua_Debug ar;
    for (int level = 1; level < 16; level++) {
        if (lua_getstack(L, level, &ar) == 0) break;
        if (lua_getinfo(L, "f", &ar) == 0) continue;
        if (!lua_isfunction(L, -1)) { lua_pop(L, 1); continue; }
        lua_getfenv(L, -1);
        if (lua_istable(L, -1)) {
            lua_getfield(L, -1, "_MOD_ID");
            if (lua_isstring(L, -1)) {
                std::string modId = lua_tostring(L, -1);
                lua_pop(L, 3);
                return modId;
            }
            lua_pop(L, 1);
        }
        lua_pop(L, 2);
    }
    return "";
}

// Public install/uninstall (kept for API compatibility)

bool installProcessEventHook() {
    // No eager install - the inline hook is installed lazily on first hook().
    return true;
}

void uninstallProcessEventHook() {
    std::lock_guard<std::mutex> lock(s_hooksMutex);
    for (auto& [nameIdx, hook] : s_hooks) {
        for (auto& cb : hook.callbacks) {
            if (cb.callbackRef != LUA_NOREF && cb.L) {
                luaL_unref(cb.L, LUA_REGISTRYINDEX, cb.callbackRef);
            }
        }
    }
    s_hooks.clear();
    // We deliberately do not restore the ProcessEvent prologue. Leaving the
    // detour in place with an empty hook map is effectively free (the fast
    // path is a single branch), and reverting an inline hook while the game
    // may still be mid-ProcessEvent call is unsafe.
}

// Lua API

/// Register a hook on a UFunction by path.
///
/// The first time a mod calls hook(), HydroCore installs an inline detour
/// on the global ProcessEvent function. Subsequent hooks just add entries
/// to the lookup table - no UFunction struct is ever mutated, so the
/// engine's dispatch behavior for unrelated functions is untouched.
///
/// @param funcPath string - Full UFunction path (e.g., "/Script/Engine.Actor:ReceiveBeginPlay")
/// @param callback function - Called when the function fires. Receives (self).
/// @returns boolean - true if the hook was registered
/// @throws string - If funcPath is invalid or function not found
/// @example
/// local Events = require("Hydro.Events")
/// Events.hook("/Script/Engine.Actor:ReceiveBeginPlay", function(self)
///     print("An actor began play: " .. tostring(self))
/// end)
static int l_events_hook(lua_State* L) {
    const char* funcPath = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);

    std::wstring widePath(funcPath, funcPath + strlen(funcPath));
    void* func = Engine::findObject(widePath.c_str());
    if (!func) {
        return luaL_error(L, "UFunction not found: %s", funcPath);
    }

    if (!installInlineHook()) {
        return luaL_error(L, "Failed to install ProcessEvent hook");
    }

    uint32_t nameIdx = readNameIdx(func);
    if (nameIdx == 0) {
        return luaL_error(L, "Could not read UFunction name index for %s", funcPath);
    }

    HookCallback cb;
    cb.L = L;
    cb.modId = getCallerModId(L);
    if (cb.modId.empty()) cb.modId = "unknown";
    cb.funcPath = funcPath;
    lua_pushvalue(L, 2);
    cb.callbackRef = luaL_ref(L, LUA_REGISTRYINDEX);

    {
        std::lock_guard<std::mutex> lock(s_hooksMutex);
        auto& hook = s_hooks[nameIdx];
        if (hook.firstPath.empty()) hook.firstPath = funcPath;
        hook.callbacks.push_back(std::move(cb));
    }

    logInfo("[Hydro.Events] Hooked %s (mod: %s, nameIdx=0x%X)",
            funcPath, s_hooks[nameIdx].callbacks.back().modId.c_str(), nameIdx);
    lua_pushboolean(L, 1);
    return 1;
}

// --- Coroutine scheduler (for wait()) ----------------------------------
//
// Top-level mod scripts run inside a Lua coroutine so they can call
// `wait(seconds)` to yield. The coroutine's state is kept alive via a
// registry ref; the scheduler resumes it when its wake time is due.
// Same PE-detour pump as periodics.
//
// `wait()` is a global: `wait(1.5)` inside any mod script yields the
// calling coroutine for 1.5 seconds. Scripts that never call wait just
// run to completion on their first resume with no additional cost.

struct PendingCoroutine {
    lua_State* co;
    int regRef;          // LUA_REGISTRYINDEX ref on main state to keep `co` from GC
    uint64_t wakeMs;
    std::string modId;
};
static std::vector<PendingCoroutine> s_coroutines;
static std::mutex s_coroutinesMutex;
static lua_State* s_mainLuaState = nullptr;  // set on first wait() call

// Lua-exposed global: wait(seconds). Yields the current coroutine.
static int l_global_wait(lua_State* L) {
    double seconds = luaL_checknumber(L, 1);
    if (seconds < 0) seconds = 0;
    lua_pushnumber(L, seconds);
    return lua_yield(L, 1);
}

// Called by LuaRuntime after the top-level resume yields. LuaRuntime has
// already pinned `co` in the registry at `regRef` - we just own that ref
// from here on and release it when the coroutine completes or errors.
void registerYieldedCoroutine(lua_State* mainL, lua_State* co,
                              double seconds, const std::string& modId,
                              int regRef) {
    if (!s_mainLuaState) s_mainLuaState = mainL;

    PendingCoroutine p;
    p.co = co;
    p.regRef = regRef;
    p.wakeMs = GetTickCount64() + (uint64_t)(seconds * 1000.0);
    p.modId = modId;
    {
        std::lock_guard<std::mutex> lock(s_coroutinesMutex);
        s_coroutines.push_back(std::move(p));
    }
    installInlineHook();  // ensure PE detour is running to pump us
}

// Pump due coroutines: resume any whose wake time has elapsed; re-register
// them if they yield again, release their ref if they complete/error.
static void pumpCoroutines() {
    uint64_t now = GetTickCount64();

    // Move due coroutines to a local buffer so we can resume them without
    // holding the mutex (Lua calls can register more coroutines).
    std::vector<PendingCoroutine> due;
    {
        std::lock_guard<std::mutex> lock(s_coroutinesMutex);
        for (auto it = s_coroutines.begin(); it != s_coroutines.end();) {
            if (now >= it->wakeMs) {
                due.push_back(std::move(*it));
                it = s_coroutines.erase(it);
            } else {
                ++it;
            }
        }
    }

    for (auto& p : due) {
        int status = lua_resume(p.co, 0);
        if (status == LUA_YIELD) {
            // Another wait() - extract new seconds from top-of-stack.
            double s = 0;
            if (lua_isnumber(p.co, -1)) s = lua_tonumber(p.co, -1);
            lua_pop(p.co, 1);
            p.wakeMs = GetTickCount64() + (uint64_t)(s * 1000.0);
            std::lock_guard<std::mutex> lock(s_coroutinesMutex);
            s_coroutines.push_back(std::move(p));
        } else {
            if (status != 0) {
                const char* msg = lua_tostring(p.co, -1);
                logError("[Hydro.Events] Coroutine error (%s): %s",
                         p.modId.c_str(), msg ? msg : "unknown");
            }
            if (s_mainLuaState && p.regRef != LUA_NOREF) {
                luaL_unref(s_mainLuaState, LUA_REGISTRYINDEX, p.regRef);
            }
        }
    }
}

// Exposed so LuaRuntime can install the `wait` global in each mod env.
int globalWaitBinding(lua_State* L) {
    return l_global_wait(L);
}

/// Remove all hooks registered by the calling mod.
///
/// @returns number - Count of hooks removed
static int l_events_unhookAll(lua_State* L) {
    std::string modId = getCallerModId(L);

    int removed = 0;
    std::lock_guard<std::mutex> lock(s_hooksMutex);
    for (auto it = s_hooks.begin(); it != s_hooks.end(); ) {
        auto& hook = it->second;
        auto before = hook.callbacks.size();
        hook.callbacks.erase(
            std::remove_if(hook.callbacks.begin(), hook.callbacks.end(),
                [&](const HookCallback& cb) {
                    if (cb.modId == modId) {
                        if (cb.callbackRef != LUA_NOREF) {
                            luaL_unref(L, LUA_REGISTRYINDEX, cb.callbackRef);
                        }
                        return true;
                    }
                    return false;
                }),
            hook.callbacks.end());
        removed += (int)(before - hook.callbacks.size());

        if (hook.callbacks.empty()) {
            it = s_hooks.erase(it);
        } else {
            ++it;
        }
    }

    lua_pushinteger(L, removed);
    return 1;
}

// Module registration

static const luaL_Reg events_functions[] = {
    {"hook",      l_events_hook},
    {"unhookAll", l_events_unhookAll},
    {nullptr,     nullptr}
};

void registerEventsModule(lua_State* L) {
    buildModuleTable(L, events_functions);
}

} // namespace Hydro::API
