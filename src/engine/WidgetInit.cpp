#include "WidgetInit.h"
#include "../EngineAPI.h"
#include "../HydroCore.h"
#include "Internal.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include <cstring>
#include <vector>

namespace Hydro::Engine {

void* s_duplicateAndInitFromWidgetTree = nullptr;

// -- Discovery ---------------------------------------------------------
//
// D&IFWT has no unique narrow strings in shipping (ensure() macros get
// stripped or merged in PGO/LTO output), so we anchor structurally on
// the gate inside `UWidgetBlueprintGeneratedClass::InitializeWidgetStatic`
// that decides whether to call D&IFWT:
//
//   mov rax, [rcx + 0x2D8]    ; UserWidget->WidgetTree (UE 5.6 instance offset)
//   ... possible spill ...
//   test rax, rax
//   jne  <skip-DIFWT>          ; ★ if WidgetTree non-null → skip
//   ... build NamedSlotContentToMerge TMap ...
//   call DuplicateAndInitializeFromWidgetTree   ; the target we want
//
// The widget-tree field offset (0x2D8) is host-specific. UE 5.6 stock
// shipping uses it; if a future UE point release shifts the layout the
// resolver returns false and the bypass is unavailable until we re-probe.
//
// The fall-through window contains many E8 calls: WBPGC::StaticClass
// helpers, the NamedSlot setup, D&IFWT itself, then the TMap teardown
// destructors. Position alone (first/last/largest) doesn't isolate
// D&IFWT - but its prologue is structurally unique: it reads the
// `GetInitializingFromWidgetTree` thread-local via `mov rax, gs:[0x58]`
// (TLS table access on Windows x64). None of the array/TMap helpers
// touch TLS, so that's a clean fingerprint.
bool findDuplicateAndInitFromWidgetTree() {
    if (!s_gm.base) return false;

    // Locate .text bounds via PE headers.
    auto* dosHeader = (IMAGE_DOS_HEADER*)s_gm.base;
    auto* ntHeaders = (IMAGE_NT_HEADERS*)(s_gm.base + dosHeader->e_lfanew);
    auto* sections = IMAGE_FIRST_SECTION(ntHeaders);
    uint8_t* textBase = nullptr;
    size_t textSize = 0;
    for (uint16_t i = 0; i < ntHeaders->FileHeader.NumberOfSections; i++) {
        if (memcmp(sections[i].Name, ".text", 5) == 0) {
            textBase = s_gm.base + sections[i].VirtualAddress;
            textSize = sections[i].Misc.VirtualSize;
            break;
        }
    }
    if (!textBase) return false;

    // Pattern: 48 8B 81 D8 02 00 00  (mov rax, [rcx+0x2D8])
    // Then within a 32-byte window: 48 85 C0 (test rax,rax) followed by
    // 0F 85 ?? ?? ?? ?? (jne rel32).
    static const uint8_t kMovPattern[] = {
        0x48, 0x8B, 0x81, 0xD8, 0x02, 0x00, 0x00
    };
    constexpr size_t kMovLen = sizeof(kMovPattern);

    std::vector<uint8_t*> sites;
    for (size_t i = 0; i + kMovLen + 9 < textSize; i++) {
        uint8_t* p = textBase + i;
        if (memcmp(p, kMovPattern, kMovLen) != 0) continue;
        // Search up to 32 bytes after for test+jne sequence.
        uint8_t* scanEnd = p + kMovLen + 32;
        if (scanEnd > textBase + textSize - 9) scanEnd = textBase + textSize - 9;
        for (uint8_t* q = p + kMovLen; q < scanEnd; q++) {
            if (q[0] == 0x48 && q[1] == 0x85 && q[2] == 0xC0 &&
                q[3] == 0x0F && q[4] == 0x85)
            {
                sites.push_back(q + 3);  // start of jne instruction
                break;
            }
        }
    }

    Hydro::logInfo("EngineAPI: D&IFWT: %zu gate candidate(s) found", sites.size());
    if (sites.empty()) {
        Hydro::logWarn("EngineAPI: D&IFWT: gate pattern not found "
                       "(WidgetTree offset shifted? non-5.6 host?)");
        return false;
    }

    // .pdata for size lookup of call targets.
    auto& excDir = ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    auto* funcs = (RUNTIME_FUNCTION*)(s_gm.base + excDir.VirtualAddress);
    size_t numFuncs = excDir.Size / sizeof(RUNTIME_FUNCTION);
    auto findFunc = [&](uint8_t* addr) -> const RUNTIME_FUNCTION* {
        uint32_t rva = (uint32_t)(addr - s_gm.base);
        size_t lo = 0, hi = numFuncs;
        while (lo < hi) {
            size_t mid = (lo + hi) / 2;
            if (rva < funcs[mid].BeginAddress)        hi = mid;
            else if (rva >= funcs[mid].EndAddress)    lo = mid + 1;
            else return &funcs[mid];
        }
        return nullptr;
    };

    // For each gate site: decode jne rel32, scan [jne_end, jne_target) for
    // E8 calls, pick the call whose target's prologue reads gs:[0x58]
    // (TLS access - D&IFWT's TScopeCounter on GetInitializingFromWidgetTree).
    //
    // The 9-byte TLS-read encoding is: 65 48 8B 04 25 58 00 00 00
    // ("mov rax, gs:[0x58]"). UE host can also emit equivalent forms:
    //   65 4C 8B 04 25 58 00 00 00  ("mov r8,  gs:[0x58]")
    //   65 4C 8B 0C 25 58 00 00 00  ("mov r9,  gs:[0x58]")
    //   65 4C 8B 14 25 58 00 00 00  ("mov r10, gs:[0x58]")
    //   65 4C 8B 1C 25 58 00 00 00  ("mov r11, gs:[0x58]")
    // Common pattern: 65 [48|4C] 8B ?? 25 58 00 00 00. Scan the target's
    // first 128 bytes for any of these.
    auto prologueReadsTls = [&](uint8_t* fn) -> bool {
        size_t scanLen = 128;
        if (fn + scanLen > textBase + textSize) scanLen = (textBase + textSize) - fn;
        for (size_t k = 0; k + 9 <= scanLen; k++) {
            if (fn[k] != 0x65) continue;
            if (fn[k+1] != 0x48 && fn[k+1] != 0x4C) continue;
            if (fn[k+2] != 0x8B) continue;
            // ModRM byte fn[k+3]: bottom 3 bits of mod=00, r/m=100 (SIB).
            // Top 3 bits = reg field, can be anything.
            if ((fn[k+3] & 0xC7) != 0x04) continue;
            if (fn[k+4] != 0x25) continue;
            // 32-bit disp = 0x00000058
            if (fn[k+5] != 0x58) continue;
            if (fn[k+6] != 0x00 || fn[k+7] != 0x00 || fn[k+8] != 0x00) continue;
            return true;
        }
        return false;
    };

    uint8_t* best = nullptr;

    for (uint8_t* jne : sites) {
        // jne encoding: 0F 85 rel32 → 6 bytes total.
        uint8_t* jneEnd = jne + 6;
        int32_t rel = 0;
        memcpy(&rel, jne + 2, 4);
        uint8_t* jneTarget = jneEnd + rel;
        if (jneTarget <= jneEnd || jneTarget > textBase + textSize) continue;
        size_t windowLen = (size_t)(jneTarget - jneEnd);
        if (windowLen > 4096) windowLen = 4096;

        Hydro::logInfo("EngineAPI: D&IFWT: gate at exe+0x%zX → fall-through window %zu bytes",
            (size_t)(jne - s_gm.base), windowLen);

        for (size_t k = 0; k + 5 <= windowLen; k++) {
            if (jneEnd[k] != 0xE8) continue;
            int32_t callRel = 0;
            memcpy(&callRel, jneEnd + k + 1, 4);
            uint8_t* target = jneEnd + k + 5 + callRel;
            if (target < textBase || target >= textBase + textSize) continue;
            const RUNTIME_FUNCTION* fn = findFunc(target);
            if (!fn) continue;
            uint32_t fnSize = fn->EndAddress - fn->BeginAddress;
            // Resolve to chain head before fingerprint check - PGO can
            // chunk the function so the call target isn't the head.
            uint8_t* head = funcStartViaPdata(target);
            if (!head) head = target;
            bool tlsHit = prologueReadsTls(head);
            Hydro::logInfo("EngineAPI: D&IFWT:   E8 @ +%zu → exe+0x%zX (head exe+0x%zX, "
                           "pdata chunk size %u) tls=%s",
                k, (size_t)(target - s_gm.base),
                (size_t)(head - s_gm.base), fnSize,
                tlsHit ? "YES" : "no");
            if (tlsHit && !best) {
                best = head;  // first TLS-using callee in window = D&IFWT
            }
        }
        if (best) break;
    }

    if (!best) {
        Hydro::logWarn("EngineAPI: D&IFWT: no viable E8 call target in any gate fall-through");
        return false;
    }

    // Walk the .pdata chain back to the true function start (PGO/LTO may
    // have split D&IFWT across multiple .pdata entries; we want the head).
    uint8_t* start = funcStartViaPdata(best);
    if (!start) start = best;

    s_duplicateAndInitFromWidgetTree = start;
    Hydro::logInfo("EngineAPI: DuplicateAndInitializeFromWidgetTree at exe+0x%zX",
        (size_t)(start - s_gm.base));
    return true;
}

// -- Public call wrapper -----------------------------------------------
//
// Signature (UE 5.6, source-verified):
//   void UUserWidget::DuplicateAndInitializeFromWidgetTree(
//       UWidgetTree* InWidgetTree,
//       const TMap<FName, UWidget*>& NamedSlotContentToMerge);
//
// x64 Windows __thiscall (== fastcall with implicit `this`):
//   rcx = this (UUserWidget*)
//   rdx = InWidgetTree
//   r8  = &NamedSlotContentToMerge  (TMap ref → pointer)
//
// TMap inline layout (UE 5.x): TSet<TPair<...>> wrapping a SparseArray.
// Empty TMap = all zeros. A generous 256-byte zero buffer covers any
// UE 5.x TMap layout shift.
//
// The function reads `widget->WidgetTree` indirectly only through the
// NewObject path (sets it via [r12+0x2D8] = new_tree). Caller is
// responsible for nulling the field first if the gate would otherwise
// trip; this wrapper does not pre-clear since the field offset isn't
// stable across engine versions and we want the wrapper to be a pure
// pass-through. (Lua callers null via PropertyMarshal before calling.)
//
// SEH-guarded: a faulted call may leak any locks the function acquired
// before the fault (FObjectInstancingGraph, GC critical section) -
// same hazard model as staticConstructObject / staticDuplicateObject.
// Caller treats `false` as soft failure.
bool duplicateAndInitializeWidgetTree(void* widget, void* srcWidgetTree) {
    if (!s_duplicateAndInitFromWidgetTree) {
        Hydro::logWarn("EngineAPI: duplicateAndInitializeWidgetTree: not resolved");
        return false;
    }
    if (!widget || !srcWidgetTree) {
        Hydro::logWarn("EngineAPI: duplicateAndInitializeWidgetTree: null arg "
                       "(widget=%p src=%p)", widget, srcWidgetTree);
        return false;
    }

    alignas(16) uint8_t emptyTMap[256] = {};

    using DIFWTFn = void (__fastcall*)(void* /*this*/, void* /*InWidgetTree*/,
                                       void* /*const TMap&*/);
    auto fn = (DIFWTFn)s_duplicateAndInitFromWidgetTree;
    __try {
        fn(widget, srcWidgetTree, emptyTMap);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Hydro::logError("EngineAPI: duplicateAndInitializeWidgetTree faulted "
                        "(widget=%p src=%p)", widget, srcWidgetTree);
        return false;
    }
    return true;
}

} // namespace Hydro::Engine
