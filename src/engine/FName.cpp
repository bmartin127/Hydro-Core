#include "FName.h"
#include "../EngineAPI.h"
#include "../HydroCore.h"
#include "Internal.h"

#include <cstring>
#include <cstdint>

#ifdef _WIN32
#include <windows.h>
#endif

namespace Hydro::Engine {

bool  s_poolReady        = false;
void* s_fnamePool        = nullptr;
int   s_poolBlocksOffset = -1;
void* s_fnameConstructor = nullptr;

// FName(const TCHAR*, EFindName) where FindType 1 = FNAME_Add.
using FNameCtorFn = void(__fastcall*)(void* outFName, const wchar_t* name, int32_t findType);

bool safeConstructFName(FName8* out, const wchar_t* name, void* ctorOverride) {
    void* ctor = ctorOverride ? ctorOverride : s_fnameConstructor;
    if (!ctor || !out || !name) return false;
#ifdef _WIN32
    __try {
        auto fn = (FNameCtorFn)ctor;
        fn(out, name, 1); // FNAME_Add = 1
        return true;
    } __except(1) { return false; }
#else
    auto fn = (FNameCtorFn)ctor;
    fn(out, name, 1);
    return true;
#endif
}

bool findFNameConstructor() {
    // Tier 1: direct byte pattern modeled on patternsleuth's fname.rs.
    // The trailing E8 targets the constructor directly.
    Hydro::logInfo("EngineAPI: Searching for FName constructor (Tier 1: direct pattern)...");
    {
        const char* pat = "EB 07 48 8D 15 ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 41 B8 01 00 00 00 E8";
        size_t patLen = 0;
        matchPattern(s_gm.base, pat, &patLen);

        for (size_t i = 0; i + patLen + 4 < s_gm.size; i++) {
            if (!matchPattern(s_gm.base + i, pat, nullptr)) continue;
            uint8_t* callAddr = s_gm.base + i + patLen;
            uint8_t* target = resolveRip4(callAddr);
            if (target >= s_gm.base && target < s_gm.base + s_gm.size) {
                s_fnameConstructor = target;
                Hydro::logInfo("EngineAPI: FName constructor at exe+0x%zX (Tier 1 pattern at exe+0x%zX)",
                    (size_t)(target - s_gm.base), i);
                return true;
            }
        }
    }

    // Tier 2: string xref patterns (from patternsleuth fname.rs lines 147-163).
    // Find known strings, then match instruction patterns around the LEA.
    Hydro::logInfo("EngineAPI: Searching for FName constructor (Tier 2: string xref)...");
    const wchar_t* anchorStrings[] = {
        L"MovementComponent0",
        L"TGPUSkinVertexFactoryUnlimited",
    };

    for (auto* anchorStr : anchorStrings) {
        uint8_t* strAddr = findWideString(s_gm.base, s_gm.size, anchorStr);
        if (!strAddr) continue;

        Hydro::logInfo("EngineAPI: Found '%ls' at exe+0x%zX", anchorStr, (size_t)(strAddr - s_gm.base));

        // Find LEA rdx, [rip+strAddr] (48 8D 15 xx xx xx xx)
        for (size_t i = 0; i + 20 < s_gm.size; i++) {
            uint8_t* p = s_gm.base + i;
            if (p[0] != 0x48 || p[1] != 0x8D || p[2] != 0x15) continue;
            int32_t disp = *(int32_t*)(p + 3);
            if (p + 7 + disp != strAddr) continue;

            // Pattern A: lea rdx,[str]; lea rcx,[fname]; call ctor
            if (p[7] == 0x48 && p[8] == 0x8D && p[9] == 0x0D) {
                if (p[14] == 0xE8) {
                    uint8_t* target = resolveRip4(p + 15);
                    if (target >= s_gm.base && target < s_gm.base + s_gm.size) {
                        FName8 validate = {0xFFFF, 0xFFFF};
                        if (safeConstructFName(&validate, L"None", target) &&
                            validate.comparisonIndex == 0 && validate.number == 0) {
                            s_fnameConstructor = target;
                            Hydro::logInfo("EngineAPI: FName constructor at exe+0x%zX (Tier 2A via '%ls', validated)",
                                (size_t)(target - s_gm.base), anchorStr);
                            return true;
                        }
                    }
                }
            }

            // Pattern B: lea rdx,[str]; lea r8,[?]; mov r9b,1; call wrapper->ctor
            if (p[7] == 0x4C && p[8] == 0x8D && p[9] == 0x05) {
                if (p[14] == 0x41 && p[15] == 0xB1 && p[16] == 0x01 && p[17] == 0xE8) {
                    uint8_t* target = resolveRip4(p + 18);
                    if (target >= s_gm.base && target < s_gm.base + s_gm.size) {
                        for (int off = 0; off < 64; off++) {
                            if (target[off] == 0xE8) {
                                uint8_t* innerTarget = resolveRip4(target + off + 1);
                                if (innerTarget >= s_gm.base && innerTarget < s_gm.base + s_gm.size) {
                                    FName8 validate = {0xFFFF, 0xFFFF};
                                    if (safeConstructFName(&validate, L"None", innerTarget) &&
                                        validate.comparisonIndex == 0 && validate.number == 0) {
                                        s_fnameConstructor = innerTarget;
                                        Hydro::logInfo("EngineAPI: FName constructor at exe+0x%zX (Tier 2B via '%ls', validated)",
                                            (size_t)(innerTarget - s_gm.base), anchorStr);
                                        return true;
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // Pattern C: mov r8d,1; lea rdx,[str]; lea rcx,[fname]; jmp ctor
            if (i >= 6 && p[-6] == 0x41 && p[-5] == 0xB8 && p[-4] == 0x01 &&
                p[-3] == 0x00 && p[-2] == 0x00 && p[-1] == 0x00) {
                if (p[7] == 0x48 && p[8] == 0x8D && p[9] == 0x0D && p[14] == 0xE9) {
                    uint8_t* target = resolveRip4(p + 15);
                    if (target >= s_gm.base && target < s_gm.base + s_gm.size) {
                        FName8 validate = {0xFFFF, 0xFFFF};
                        if (safeConstructFName(&validate, L"None", target) &&
                            validate.comparisonIndex == 0 && validate.number == 0) {
                            s_fnameConstructor = target;
                            Hydro::logInfo("EngineAPI: FName constructor at exe+0x%zX (Tier 2C via '%ls', validated)",
                                (size_t)(target - s_gm.base), anchorStr);
                            return true;
                        }
                    }
                }
            }
        }
    }

    Hydro::logWarn("EngineAPI: FName constructor not found");
    return false;
}

int readPoolEntryRaw(uint32_t comparisonIndex, char* buf, int bufSize, bool* isWide) {
    int blockIdx    = comparisonIndex >> 16;
    int entryOffset = (comparisonIndex & 0xFFFF) * 2; // stride = 2

    void* blockPtr = nullptr;
    if (!safeReadPtr((uint8_t*)s_fnamePool + s_poolBlocksOffset + blockIdx * 8, &blockPtr) || !blockPtr)
        return 0;

    uint16_t header = 0;
    if (!safeReadInt32((uint8_t*)blockPtr + entryOffset, (int32_t*)&header))
        return 0;
    // Only lower 16 bits are the header
    header &= 0xFFFF;

    *isWide = header & 1;
    int len = (header >> 6) & 0x3FF;
    if (len <= 0 || len > 1024 || len > bufSize) return 0;

    uint8_t* namePtr = (uint8_t*)blockPtr + entryOffset + 2;
    int bytesToRead = *isWide ? len * 2 : len;

#ifdef _WIN32
    __try {
        memcpy(buf, namePtr, bytesToRead);
    } __except(1) { return 0; }
#else
    memcpy(buf, namePtr, bytesToRead);
#endif

    return len;
}

// Checks that candidate+blocksOffset[0] points to a heap block whose first
// 6 bytes look like a valid FNameEntry for "None" (header + 4 ASCII chars).
static bool tryValidatePool(void* candidate, int blocksOffset) {
    void* block0 = nullptr;
    if (!safeReadPtr((uint8_t*)candidate + blocksOffset, &block0) || !block0)
        return false;

    uint8_t data[8] = {};
#ifdef _WIN32
    __try {
        memcpy(data, block0, 8);
    } __except(1) { return false; }
#else
    memcpy(data, block0, 8);
#endif

    uint16_t header = *(uint16_t*)data;
    bool isWide = header & 1;
    int len = (header >> 6) & 0x3FF;

    if (isWide || len != 4) return false;
    if (memcmp(data + 2, "None", 4) != 0) return false;

    return true;
}

bool discoverFNamePool() {
    static const int kBlocksOffsets[] = { 0x10, 0x18, 0x08, 0x20, 0x28, 0x30, 0x38, 0x40 };

    // Strategy 1: walk the FName constructor's call tree (2 levels deep).
    // The pool global is accessed via LEA or MOV [rip+disp] from a sub-function
    // of the constructor (not from the constructor itself, which just hashes).
    if (s_fnameConstructor) {
        uint8_t* func = (uint8_t*)s_fnameConstructor;

        constexpr int MAX_TARGETS = 64;
        uint8_t* targets[MAX_TARGETS];
        int numTargets = 0;
        targets[numTargets++] = func;

        int level1End = 1;
        for (int i = 0; i < 2048 && numTargets < MAX_TARGETS; i++) {
            if (func[i] == 0xCC && i > 0 && func[i-1] == 0xCC) break;
            if (func[i] == 0xE8) {
                int32_t rel = *(int32_t*)(func + i + 1);
                uint8_t* t = func + i + 5 + rel;
                if (t >= s_gm.base && t < s_gm.base + s_gm.size) {
                    bool dup = false;
                    for (int j = 0; j < numTargets; j++) if (targets[j] == t) { dup = true; break; }
                    if (!dup) targets[numTargets++] = t;
                }
                i += 4;
            }
        }
        level1End = numTargets;

        for (int k = 1; k < level1End && numTargets < MAX_TARGETS; k++) {
            uint8_t* callee = targets[k];
            for (int i = 0; i < 512 && numTargets < MAX_TARGETS; i++) {
                if (callee[i] == 0xCC && i > 0 && callee[i-1] == 0xCC) break;
                if (callee[i] == 0xE8) {
                    int32_t rel = *(int32_t*)(callee + i + 1);
                    uint8_t* t = callee + i + 5 + rel;
                    if (t >= s_gm.base && t < s_gm.base + s_gm.size) {
                        bool dup = false;
                        for (int j = 0; j < numTargets; j++) if (targets[j] == t) { dup = true; break; }
                        if (!dup) targets[numTargets++] = t;
                    }
                    i += 4;
                }
            }
        }

        Hydro::logInfo("EngineAPI: FNamePool: scanning %d functions (2-level call tree)", numTargets);

        // Scan each function for LEA or MOV [rip+disp32] to a pool candidate.
        // MOV [rip+disp] means resolved is a pointer-to-pool; dereference it.
        for (int t = 0; t < numTargets; t++) {
            uint8_t* scan = targets[t];
            for (int i = 0; i + 7 < 512; i++) {
                uint8_t rex = scan[i];
                if (rex != 0x48 && rex != 0x4C && rex != 0x4D) continue;
                if (scan[i+1] != 0x8D && scan[i+1] != 0x8B) continue;  // LEA or MOV
                if ((scan[i+2] & 0xC7) != 0x05) continue;

                int32_t disp = *(int32_t*)(scan + i + 3);
                void* resolved = scan + i + 7 + disp;
                if ((uint8_t*)resolved < s_gm.base || (uint8_t*)resolved >= s_gm.base + s_gm.size)
                    continue;

                bool isMov = (scan[i+1] == 0x8B);
                void* candidate = resolved;

                if (isMov) {
                    void* ptr = nullptr;
                    if (!safeReadPtr(resolved, &ptr) || !ptr) continue;
                    candidate = ptr;
                }

                for (int off : kBlocksOffsets) {
                    if (tryValidatePool(candidate, off)) {
                        s_fnamePool = candidate;
                        s_poolBlocksOffset = off;
                        s_poolReady = true;
                        Hydro::logInfo("EngineAPI: FNamePool at %p (blocksOffset=0x%X) via %s in func[%d] exe+0x%zX",
                            candidate, off, isMov ? "MOV" : "LEA", t, (size_t)(scan - s_gm.base));
                        return true;
                    }
                }
            }
        }
        Hydro::logWarn("EngineAPI: FNamePool not found via call tree (%d funcs)", numTargets);
    }

    // Strategy 2: data section scan.
    // FNameEntryAllocator embedded at offset 0 of FNamePool has a distinctive layout:
    //   +0x00: SRWLOCK (8 bytes, usually 0)
    //   +0x08: CurrentBlock (uint32, typically 1-100)
    //   +0x0C: CurrentByteCursor (uint32, 0..131072)
    //   +0x10: Blocks[0] -> heap ptr -> FNameEntry starting with "None"
    Hydro::logInfo("EngineAPI: FNamePool: trying data section scan...");
    {
        auto* dosHeader = (IMAGE_DOS_HEADER*)s_gm.base;
        auto* ntHeaders = (IMAGE_NT_HEADERS*)(s_gm.base + dosHeader->e_lfanew);
        auto* sec = IMAGE_FIRST_SECTION(ntHeaders);
        int numSec = ntHeaders->FileHeader.NumberOfSections;

        for (int s = 0; s < numSec; s++, sec++) {
            if (!(sec->Characteristics & IMAGE_SCN_MEM_WRITE)) continue;
            uint8_t* secStart = s_gm.base + sec->VirtualAddress;
            size_t secSize = sec->Misc.VirtualSize;

            for (size_t off = 0; off + 0x40 <= secSize; off += 8) {
                uint8_t* addr = secStart + off;

                for (int bOff : kBlocksOffsets) {
                    if ((size_t)(off + bOff + 8) > secSize) continue;

                    void* block0 = *(void**)(addr + bOff);
                    if (!block0) continue;
                    // Blocks[0] must be a heap pointer, not inside the module image
                    if ((uint8_t*)block0 >= s_gm.base && (uint8_t*)block0 < s_gm.base + s_gm.size) continue;

                    if (tryValidatePool(addr, bOff)) {
                        s_fnamePool = addr;
                        s_poolBlocksOffset = bOff;
                        s_poolReady = true;
                        Hydro::logInfo("EngineAPI: FNamePool at %p (blocksOffset=0x%X) via data scan (exe+0x%zX)",
                            addr, bOff, (size_t)(addr - s_gm.base));
                        return true;
                    }
                }
            }
        }
    }

    Hydro::logWarn("EngineAPI: FNamePool not found by any method");
    return false;
}

} // namespace Hydro::Engine
