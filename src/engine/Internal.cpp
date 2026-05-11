#include "Internal.h"
#include "../SEH.h"

#include <cstring>
#include <cstdlib>

#ifdef _WIN32
#include <psapi.h>
#endif

#include <Zydis/Zydis.h>

namespace Hydro::Engine {

// Storage for the cross-module globals declared in Internal.h.
GameModule s_gm = {};
bool       s_ready = false;
bool       s_initFatallyFailed = false;

GameModule findGameModule() {
#ifdef _WIN32
    HMODULE modules[1024]; DWORD needed;
    if (!K32EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed))
        return {nullptr, 0};
    HMODULE best = nullptr; size_t largest = 0;
    for (DWORD i = 0; i < needed / sizeof(HMODULE); i++) {
        MODULEINFO mi; char name[MAX_PATH];
        if (!K32GetModuleInformation(GetCurrentProcess(), modules[i], &mi, sizeof(mi))) continue;
        K32GetModuleBaseNameA(GetCurrentProcess(), modules[i], name, MAX_PATH);
        if (strstr(name, "UE4SS") || strstr(name, "HydroCore") || strstr(name, "patternsleuth")) continue;
        if (mi.SizeOfImage > largest) { largest = mi.SizeOfImage; best = modules[i]; }
    }
    if (!best) return {nullptr, 0};
    MODULEINFO gi; K32GetModuleInformation(GetCurrentProcess(), best, &gi, sizeof(gi));
    return {(uint8_t*)gi.lpBaseOfDll, gi.SizeOfImage};
#else
    return {nullptr, 0};
#endif
}

bool safeReadPtr(void* addr, void** out) {
    HYDRO_SEH_TRY(*out = *(void**)addr);
}

bool safeReadInt32(void* addr, int32_t* out) {
    HYDRO_SEH_TRY(*out = *(int32_t*)addr);
}

bool safeReadU64(void* addr, uint64_t* out) {
    HYDRO_SEH_TRY(*out = *(uint64_t*)addr);
}

bool matchPattern(const uint8_t* data, const char* pattern, size_t* outLen) {
    size_t i = 0, di = 0;
    while (pattern[i]) {
        while (pattern[i] == ' ') i++;
        if (!pattern[i]) break;
        if (pattern[i] == '?') {
            i++; if (pattern[i] == '?') i++;
            di++;
        } else {
            uint8_t b = (uint8_t)strtol(pattern + i, nullptr, 16);
            if (data[di] != b) return false;
            i += 2; di++;
        }
    }
    if (outLen) *outLen = di;
    return true;
}

uint8_t* resolveRip4(uint8_t* addr) {
    int32_t rel = *(int32_t*)addr;
    return addr + 4 + rel;
}

uint8_t* findWideString(uint8_t* base, size_t size, const wchar_t* str) {
    size_t strBytes = wcslen(str) * 2;
    for (size_t i = 0; i + strBytes <= size; i++) {
        if (memcmp(base + i, str, strBytes) == 0) return base + i;
    }
    return nullptr;
}

// Finds the first `lea r64, [rip+disp32]` referencing `target`. Matches
// REX-prefixed encoding (48/4C 8D ?? ?? ?? ?? ??) with ModR/M mod=00, rm=101.
// Destination register isn't filtered, so any of rax..r15 matches.
uint8_t* findLeaRef(uint8_t* base, size_t size, uint8_t* target) {
    for (size_t i = 0; i + 7 < size; i++) {
        if ((base[i] == 0x48 || base[i] == 0x4C) && base[i+1] == 0x8D) {
            uint8_t modrm = base[i+2];
            if ((modrm & 0xC7) != 0x05) continue;
            int32_t disp = *(int32_t*)(base + i + 3);
            if (base + i + 7 + disp == target) return base + i;
        }
    }
    return nullptr;
}

// All LEA xrefs to `target`. Same encoding match as findLeaRef. Used by
// resolvers that converge statistically across many xrefs (e.g. SCO anchors
// on a log string referenced from ~700 inlined empty-name checks; walking
// forward from each to tally near-call targets picks SCO as the winner).
std::vector<uint8_t*> findAllLeaRefs(uint8_t* base, size_t size, uint8_t* target) {
    std::vector<uint8_t*> results;
    for (size_t i = 0; i + 7 < size; i++) {
        if ((base[i] == 0x48 || base[i] == 0x4C) && base[i+1] == 0x8D) {
            uint8_t modrm = base[i+2];
            if ((modrm & 0xC7) != 0x05) continue;
            int32_t disp = *(int32_t*)(base + i + 3);
            if (base + i + 7 + disp == target) results.push_back(base + i);
        }
    }
    return results;
}

// Like findAllLeaRefs but also matches non-LEA RIP-relative operand sites
// (mov reg, [rip+disp]; movups/movaps/movdqu xmm, [rip+disp]; mov [rip+disp], reg).
// MSVC PGO sometimes loads a 16-byte constant via SSE instead of LEA - the
// AssetRegistry version GUID on UE 5.6 shipping has zero LEA refs but several
// SSE-load refs, which is what motivated this.
//
// Strategy: at each position, treat next 4 bytes as candidate disp32 and check
// if it RIP-addresses `target`. If so, walk back up to 5 bytes to confirm a
// known instruction prefix that produces a valid RIP-relative form (mod=00,
// rm=101). Accepted forms below.
std::vector<uint8_t*> findAllRipRelativeRefs(uint8_t* base, size_t size,
                                             uint8_t* target) {
    std::vector<uint8_t*> results;
    if (size < 8) return results;
    for (size_t i = 0; i + 8 < size; i++) {
        int32_t disp = *(int32_t*)(base + i);
        uint8_t* candidate = base + i + 4 + disp;
        if (candidate != target) continue;
        if (i == 0) continue;
        uint8_t modrm = base[i - 1];
        if ((modrm & 0xC7) != 0x05) continue;  // mod=00, rm=101
        size_t modrmIdx = i - 1;
        if (modrmIdx >= 1) {
            uint8_t op = base[modrmIdx - 1];
            uint8_t pre = (modrmIdx >= 2) ? base[modrmIdx - 2] : 0;
            uint8_t pre2 = (modrmIdx >= 3) ? base[modrmIdx - 3] : 0;
            bool match = false;
            // 48/4C/49 8D = LEA r64; 48/4C/49 8B = MOV r64
            if ((pre == 0x48 || pre == 0x4C || pre == 0x49) &&
                (op == 0x8D || op == 0x8B)) {
                match = true;
            } else if (op == 0x8B || op == 0x89) {
                // MOV r32 ↔ [rip+disp], no REX
                match = true;
            } else if (pre == 0x0F && (op == 0x28 || op == 0x10 ||
                                        op == 0x11)) {
                // 0F 28/10/11 = MOVAPS / MOVUPS xmm
                match = true;
            } else if (pre2 == 0x66 && pre == 0x0F &&
                       (op == 0x28 || op == 0x6F || op == 0x7F)) {
                // 66 0F 28/6F/7F = MOVAPD / MOVDQA
                match = true;
            } else if (pre2 == 0xF3 && pre == 0x0F &&
                       (op == 0x6F || op == 0x7F || op == 0x10)) {
                // F3 0F 6F/7F/10 = MOVDQU / MOVSS
                match = true;
            }
            if (match) {
                size_t instStart = modrmIdx - 1;
                if (pre == 0x48 || pre == 0x4C || pre == 0x49 ||
                    pre == 0x0F) instStart -= 1;
                if (pre2 == 0x66 || pre2 == 0xF3) instStart -= 1;
                results.push_back(base + instStart);
            }
        }
    }
    return results;
}

// Every direct near-call (E8 disp32) whose target equals `target`. Returns
// the set of containing functions, deduped via .pdata. 0xE8 also appears as
// data inside other instructions (immediate operands, jump tables); those
// rarely happen to encode a valid disp into `target`, and any false positives
// wash out at set-intersect time in the AR.Serialize resolver (only functions
// calling BOTH inner callees survive).
//
// Used by the AR.Serialize resolver to climb from leaf __FUNCTION__ anchors
// up the call chain to UAssetRegistryImpl::Serialize, bypassing the vtable.
std::unordered_set<uint8_t*> findE8CallersOf(uint8_t* base, size_t size, uint8_t* target) {
    std::unordered_set<uint8_t*> callers;
    if (size < 5) return callers;
    for (size_t i = 0; i + 5 <= size; i++) {
        if (base[i] != 0xE8) continue;
        int32_t disp = *(int32_t*)(base + i + 1);
        uint8_t* nextIp = base + i + 5;
        if (nextIp + disp != target) continue;
        uint8_t* fs = funcStartViaPdata(base + i);
        if (fs) callers.insert(fs);
    }
    return callers;
}

// Walk backwards from `addr` looking for CC padding (the int3 byte the
// linker uses to pad between functions). Use sparingly - on PGO/LTO builds
// padding can be missing and 0xCC legitimately appears mid-function inside
// immediate constants and jump-table fillers. Prefer funcStartViaPdata.
uint8_t* walkToFuncStart(uint8_t* addr, int maxBack) {
    uint8_t* p = addr;
    for (int i = 0; i < maxBack; i++) {
        if (p[-1] == 0xCC) return p;
        p--;
    }
    return addr;
}

// Binary-search the PE exception directory (.pdata) for the RUNTIME_FUNCTION
// covering `targetRva`. Returns null if it's not inside any entry.
#ifdef _WIN32
const RUNTIME_FUNCTION* lookupRuntimeFunction(uint32_t targetRva) {
    if (!s_gm.base) return nullptr;
    auto* dosHeader = (IMAGE_DOS_HEADER*)s_gm.base;
    auto* ntHeaders = (IMAGE_NT_HEADERS*)(s_gm.base + dosHeader->e_lfanew);
    auto& excDir = ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (excDir.VirtualAddress == 0 || excDir.Size == 0) return nullptr;
    auto* funcs = (RUNTIME_FUNCTION*)(s_gm.base + excDir.VirtualAddress);
    size_t numFuncs = excDir.Size / sizeof(RUNTIME_FUNCTION);
    size_t lo = 0, hi = numFuncs;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (targetRva < funcs[mid].BeginAddress)        hi = mid;
        else if (targetRva >= funcs[mid].EndAddress)    lo = mid + 1;
        else return &funcs[mid];
    }
    return nullptr;
}
#endif

// Resolve the function containing `addr` via .pdata, following CHAININFO
// chains to the real prologue. PGO/LTO splits a function into multiple
// regions; only the primary region's UNWIND_INFO has the real prologue, and
// continuation regions set UNW_FLAG_CHAININFO with a pointer to the parent.
// Without the chain walk you can land mid-function on bytes that use r11 as
// a frame pointer left set by the parent's prologue, and calling that as if
// it were a function entry crashes on uninitialized r11.
uint8_t* funcStartViaPdata(uint8_t* addr) {
#ifdef _WIN32
    if (!s_gm.base || addr < s_gm.base || addr >= s_gm.base + s_gm.size) return nullptr;
    uint32_t targetRva = (uint32_t)(addr - s_gm.base);
    const RUNTIME_FUNCTION* rf = lookupRuntimeFunction(targetRva);
    // Bound the chain walk; any reasonable function spans few regions.
    for (int hops = 0; rf && hops < 16; hops++) {
        // Low bit set means UnwindInfoAddress points at another RUNTIME_FUNCTION
        // (rare alternate encoding); strip it and read UNWIND_INFO normally.
        uint32_t uiRva = rf->UnwindInfoAddress & ~uint32_t(1);
        if (uiRva == 0) break;
        const uint8_t* ui = s_gm.base + uiRva;
        uint8_t verFlags = ui[0];
        uint8_t flags    = verFlags >> 3;
        if ((flags & 0x4) == 0) {
            // Not chained - this is the real prologue.
            return s_gm.base + rf->BeginAddress;
        }
        uint8_t countCodes = ui[2];
        // Unwind codes are 2 bytes each, padded to a 4-byte boundary
        // (i.e. count rounded UP to even).
        uint32_t codesBytes = (((uint32_t)countCodes + 1u) & ~1u) * 2u;
        const RUNTIME_FUNCTION* parent =
            (const RUNTIME_FUNCTION*)(ui + 4 + codesBytes);
        uint32_t parentRva = parent->BeginAddress;
        if (parentRva == 0 || parentRva == rf->BeginAddress) break;
        rf = lookupRuntimeFunction(parentRva);
    }
    return rf ? (s_gm.base + rf->BeginAddress) : nullptr;
#else
    (void)addr;
    return nullptr;
#endif
}

// Walk forward from `start` for up to `maxBytes`, collecting every
// `call rel32` target. Stops on RET, JMP, or any undecodable byte. Indirect
// calls (FF /2) are skipped since they don't have a static target.
//
// Used by the SCO resolver: from each LEA-to-error-string site we walk
// forward looking for the near-call into SCO that follows the inlined
// empty-name check. Tallying targets across many sites makes SCO the
// clear winner regardless of which PGO/inline shape any one site has.
std::vector<uint8_t*> collectForwardCallTargets(uint8_t* start, int maxBytes) {
    std::vector<uint8_t*> targets;

    ZydisDecoder decoder;
    if (ZYAN_FAILED(ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64))) {
        return targets;
    }

    int pos = 0;
    while (pos < maxBytes) {
        ZydisDecodedInstruction inst;
        ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
        if (ZYAN_FAILED(ZydisDecoderDecodeFull(&decoder, start + pos, maxBytes - pos,
                                                &inst, ops))) {
            break;
        }

        if (inst.mnemonic == ZYDIS_MNEMONIC_CALL &&
            (inst.attributes & ZYDIS_ATTRIB_IS_RELATIVE) &&
            inst.raw.imm[0].size > 0) {
            int64_t disp = (int64_t)inst.raw.imm[0].value.s;
            uint8_t* nextIp = start + pos + inst.length;
            uint8_t* target = nextIp + disp;
            if (target >= s_gm.base && target < s_gm.base + s_gm.size) {
                targets.push_back(target);
            }
        }

        // Stop on definitive control-flow exits; past these we'd be reading
        // code outside this basic block.
        if (inst.mnemonic == ZYDIS_MNEMONIC_RET ||
            inst.mnemonic == ZYDIS_MNEMONIC_JMP) {
            break;
        }

        pos += inst.length;
    }

    return targets;
}

} // namespace Hydro::Engine
