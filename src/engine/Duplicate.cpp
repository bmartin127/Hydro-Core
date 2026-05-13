#include "Duplicate.h"
#include "../EngineAPI.h"
#include "../HydroCore.h"
#include "Internal.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace Hydro::Engine {

void* s_staticDuplicateObjectEx = nullptr;
void* s_staticDuplicateObject   = nullptr;

// -- Discovery ---------------------------------------------------------
//
// UE 5.6 cooked WBP runtime instantiation requires deep-copying the class
// WidgetTree into the widget instance. The standard NewObject-with-template
// path UE 5.6 uses internally goes through an InstancingGraph filter that
// skips `UWidgetTree::RootWidget` (no `CPF_InstancedReference`), leaving
// the instance tree empty. StaticDuplicateObject bypasses that - archive-
// based serialization writes every UPROPERTY regardless of Instanced /
// Transient flags. See project_wbp_init_failure memory.
//
// Resolver path (disassembly-verified on DMG UE 5.6 shipping):
//
//   Wide string L"FDuplicateDataWriter"  (only in its GetArchiveName virtual)
//   ↓ LEA xref
//   FDuplicateDataWriter::GetArchiveName  (~33 byte virtual)
//   ↓ scan .rdata for qword == GetArchiveName
//   FDuplicateDataWriter vtable
//   ↓ LEA xref (sole - only the ctor places this vtable into an object)
//   FDuplicateDataWriter::FDuplicateDataWriter ctor  (~275 bytes)
//   ↓ E8 caller (sole - only SDOEx constructs a Writer)
//   StaticDuplicateObjectEx
//
// Each hop produces exactly one result on PGO/LTO shipping, so no
// statistical convergence or set intersection needed. The Writer anchor
// works where the Reader anchor failed because Reader's call graph is
// heavily inlined while Writer's ctor stays a real E8-callable function.

// Helper: find the vtable of a class given its name. Returns nullptr on
// failure. Logs progress at each hop.
static uint8_t* findClassVtable(const wchar_t* className,
    uint8_t* textBase, size_t textSize,
    uint8_t* rdataBase, size_t rdataSize,
    RUNTIME_FUNCTION* funcs, size_t numFuncs)
{
    auto findFunc = [&](uint8_t* addr) -> RUNTIME_FUNCTION* {
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
    auto inText = [&](uint8_t* p) { return p >= textBase && p < textBase + textSize; };

    uint8_t* strAddr = findWideString(s_gm.base, s_gm.size, className);
    if (!strAddr) {
        Hydro::logWarn("EngineAPI: SDO: anchor '%ls' not found", className);
        return nullptr;
    }
    Hydro::logInfo("EngineAPI: SDO: '%ls' string at exe+0x%zX",
        className, (size_t)(strAddr - s_gm.base));

    auto nameLeas = findAllLeaRefs(s_gm.base, s_gm.size, strAddr);
    if (nameLeas.empty()) return nullptr;

    // Smallest containing function = GetArchiveName virtual.
    uint8_t* getArchiveName = nullptr;
    uint32_t ganSize = UINT32_MAX;
    for (uint8_t* lea : nameLeas) {
        auto* f = findFunc(lea);
        if (!f) continue;
        uint32_t sz = f->EndAddress - f->BeginAddress;
        if (sz < ganSize) { ganSize = sz; getArchiveName = s_gm.base + f->BeginAddress; }
    }
    if (!getArchiveName) return nullptr;
    Hydro::logInfo("EngineAPI: SDO: '%ls'::GetArchiveName at exe+0x%zX (size=%u)",
        className, (size_t)(getArchiveName - s_gm.base), ganSize);

    // Scan .rdata for the qword == GetArchiveName and walk back to vtable base.
    for (size_t off = 0; off + 8 <= rdataSize; off += 8) {
        uint8_t** slot = (uint8_t**)(rdataBase + off);
        if (*slot != getArchiveName) continue;
        uint8_t* vtStart = (uint8_t*)slot;
        for (int back = 0; back < 64; back++) {
            uint8_t** prev = (uint8_t**)(vtStart - 8);
            if ((uint8_t*)prev < rdataBase || !inText(*prev)) break;
            vtStart = (uint8_t*)prev;
        }
        Hydro::logInfo("EngineAPI: SDO: '%ls' vtable at exe+0x%zX",
            className, (size_t)(vtStart - s_gm.base));
        return vtStart;
    }
    return nullptr;
}

bool findStaticDuplicateObject() {
    if (!s_gm.base) return false;

    auto* dosHeader = (IMAGE_DOS_HEADER*)s_gm.base;
    auto* ntHeaders = (IMAGE_NT_HEADERS*)(s_gm.base + dosHeader->e_lfanew);
    auto& excDir = ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    auto* funcs = (RUNTIME_FUNCTION*)(s_gm.base + excDir.VirtualAddress);
    size_t numFuncs = excDir.Size / sizeof(RUNTIME_FUNCTION);

    auto* sections = IMAGE_FIRST_SECTION(ntHeaders);
    uint8_t* rdataBase = nullptr; size_t rdataSize = 0;
    uint8_t* textBase  = nullptr; size_t textSize  = 0;
    for (uint16_t i = 0; i < ntHeaders->FileHeader.NumberOfSections; i++) {
        if (memcmp(sections[i].Name, ".rdata", 6) == 0) {
            rdataBase = s_gm.base + sections[i].VirtualAddress;
            rdataSize = sections[i].Misc.VirtualSize;
        } else if (memcmp(sections[i].Name, ".text", 5) == 0) {
            textBase = s_gm.base + sections[i].VirtualAddress;
            textSize = sections[i].Misc.VirtualSize;
        }
    }
    if (!rdataBase || !textBase) return false;

    auto findFunc = [&](uint8_t* addr) -> RUNTIME_FUNCTION* {
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

    // Anchor on Writer (not Reader) - Writer's ctor stays a real E8-callable
    // function in PGO shipping, with SDOEx as its sole non-inline caller.
    uint8_t* writerVt = findClassVtable(L"FDuplicateDataWriter",
        textBase, textSize, rdataBase, rdataSize, funcs, numFuncs);
    if (!writerVt) return false;

    // LEA xrefs to the Writer vtable. Expected: 1 (the ctor).
    auto vtLeas = findAllLeaRefs(s_gm.base, s_gm.size, writerVt);
    if (vtLeas.empty()) {
        Hydro::logWarn("EngineAPI: SDO: no LEA xrefs to Writer vtable");
        return false;
    }
    // Containing function = Writer ctor (smallest, since the ctor is the
    // only place anyone places this vtable into an object).
    uint8_t* writerCtor = nullptr;
    uint32_t writerCtorSize = UINT32_MAX;
    for (uint8_t* lea : vtLeas) {
        auto* f = findFunc(lea);
        if (!f) continue;
        uint32_t sz = f->EndAddress - f->BeginAddress;
        if (sz < writerCtorSize) {
            writerCtorSize = sz;
            writerCtor = s_gm.base + f->BeginAddress;
        }
    }
    if (!writerCtor) {
        Hydro::logWarn("EngineAPI: SDO: Writer ctor not resolvable via .pdata");
        return false;
    }
    Hydro::logInfo("EngineAPI: SDO: Writer ctor at exe+0x%zX (size=%u)",
        (size_t)(writerCtor - s_gm.base), writerCtorSize);

    // E8 callers of the Writer ctor. SDOEx is the sole/largest caller.
    auto callers = findE8CallersOf(s_gm.base, s_gm.size, writerCtor);
    std::unordered_map<uint8_t*, uint32_t> uniqueCallers;
    for (uint8_t* call : callers) {
        auto* f = findFunc(call);
        if (!f) continue;
        uniqueCallers[s_gm.base + f->BeginAddress] = f->EndAddress - f->BeginAddress;
    }
    Hydro::logInfo("EngineAPI: SDO: Writer ctor has %zu unique E8 caller function(s)",
        uniqueCallers.size());
    if (uniqueCallers.empty()) {
        Hydro::logWarn("EngineAPI: SDO: Writer ctor has no E8 callers");
        return false;
    }
    // Largest caller = SDOEx.
    uint8_t* sdoEx = nullptr;
    uint32_t sdoExSize = 0;
    for (auto& [fn, sz] : uniqueCallers) {
        if (sz > sdoExSize) { sdoExSize = sz; sdoEx = fn; }
        Hydro::logInfo("EngineAPI: SDO:   caller exe+0x%zX size=%u",
            (size_t)(fn - s_gm.base), sz);
    }
    s_staticDuplicateObjectEx = sdoEx;
    Hydro::logInfo("EngineAPI: StaticDuplicateObjectEx at exe+0x%zX (size=%u)",
        (size_t)(sdoEx - s_gm.base), sdoExSize);

    // Note: we deliberately do NOT auto-resolve the 7-arg public wrapper
    // here. On UE 5.6 PGO/LTO shipping, the true wrapper is inlined into
    // all its callers - the only surviving E8 callers of SDOEx are
    // random thunks (often ~50 bytes) that bear no relationship to the
    // public StaticDuplicateObject ABI. Calling such a thunk with the
    // 7-arg fastcall layout passes garbage in the wrong registers and
    // produces a degenerate duplicate (empty/nil property tree). Direct
    // SDOEx + hand-built params struct (Path B) is the reliable route
    // on shipping. The wrapper field stays nullptr.
    s_staticDuplicateObject = nullptr;
    return true;
}

// -- Public call wrapper -----------------------------------------------
//
// FObjectDuplicationParameters layout (UE 5.6, disassembly-verified on
// DMG shipping - confirms `Engine/Source/Runtime/CoreUObject/Public/
// UObject/UObjectGlobals.h` declaration):
//
//   +0x00  UObject*               SourceObject
//   +0x08  UObject*               DestOuter
//   +0x10  FName                  DestName            (8 bytes)
//   +0x18  EObjectFlags           FlagMask            (u32)
//   +0x1C  EInternalObjectFlags   InternalFlagMask    (u32)
//   +0x20  EObjectFlags           ApplyFlags          (u32)
//   +0x24  EInternalObjectFlags   ApplyInternalFlags  (u32)
//   +0x28  uint32                 PortFlags
//   +0x2C  EDuplicateMode::Type   DuplicateMode       (i32, Normal=0)
//   +0x30  bool                   bAssignExternalPackages  (default true)
//   +0x31  bool                   bSkipPostLoad
//   +0x38  UClass*                DestClass           (null → use src's class)
//   +0x40  TMap<UObject*,UObject*> DuplicationSeed    (~80 B inline; empty = zero)
//   +0x90  TMap<UObject*,UObject*>* CreatedObjects    (null OK)
//
// Buffer of 0xC0 bytes covers it with margin. The TMap at +0x40 is INLINE
// (not a pointer) - leaving it zero-initialized is valid (empty TMap =
// {Data=null, ArrayNum=0, ArrayMax=0, ...}).
//
// SEH-guarded: a faulted call may leak SEH-acquired locks, same hazard as
// staticConstructObject. Caller treats nullptr return as soft failure.
void* staticDuplicateObject(void* source, void* outer) {
    if (!s_staticDuplicateObjectEx) {
        Hydro::logWarn("EngineAPI: staticDuplicateObject called but SDO not resolved");
        return nullptr;
    }
    if (!source) {
        Hydro::logWarn("EngineAPI: staticDuplicateObject called with null source");
        return nullptr;
    }

    void* result = nullptr;

    if (s_staticDuplicateObject) {
        // Path A - call the 7-arg public wrapper.
        //
        // x64 Windows fastcall:
        //   rcx       = SourceObject
        //   rdx       = DestOuter
        //   r8        = DestName       (FName by value, 8 bytes)
        //   r9        = FlagMask       (EObjectFlags u32)
        //   [rsp+0x20]= DestClass      (ptr)
        //   [rsp+0x28]= DuplicateMode  (i32)
        //   [rsp+0x30]= InternalFlagsMask (u32)
        using SDOFn = void* (__fastcall*)(
            void* SourceObject, void* DestOuter,
            uint64_t DestName, uint32_t FlagMask,
            void* DestClass, int32_t DuplicateMode, uint32_t InternalFlagsMask);
        auto fn = (SDOFn)s_staticDuplicateObject;
        __try {
            result = fn(source, outer,
                /*DestName=NAME_None*/   0ull,
                /*FlagMask=RF_AllFlags*/ 0xFFFFFFFFu,
                /*DestClass=nullptr → use src's class*/ nullptr,
                /*DuplicateMode=Normal*/ 0,
                /*InternalFlagsMask=All*/ 0xFFFFFFFFu);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Hydro::logError("EngineAPI: staticDuplicateObject (wrapper) faulted "
                            "(src=%p outer=%p)", source, outer);
            return nullptr;
        }
        return result;
    }

    // Path B - hand-build FObjectDuplicationParameters and call SDOEx.
    //
    // Generous 1KB buffer in case the struct has trailing fields we
    // haven't fully enumerated (UE 5.6 layouts can grow across point
    // releases). With everything zero by default and only the minimum
    // fields populated, SDOEx should treat unset fields as defaults.
    //
    // Critical: bAssignExternalPackages = false (zero). With true, SDOEx
    // walks into external-package logic that touches state we haven't
    // initialized (likely the path that faulted previously). For our
    // single-asset WidgetTree duplicate, external-package handling is
    // unnecessary anyway.
    alignas(16) uint8_t paramBuf[1024] = {};
    *(void**)(paramBuf + 0x00) = source;                 // SourceObject
    *(void**)(paramBuf + 0x08) = outer;                  // DestOuter
    // DestName (+0x10) = NAME_None ({0,0}) - zero correct.
    // FlagMask (+0x18) = RF_AllFlags = 0x0FFFFFFF (high bits beyond the
    // defined flag range can confuse SDOEx's internal bit-mask logic).
    *(uint32_t*)(paramBuf + 0x18) = 0x0FFFFFFFu;
    // InternalFlagMask (+0x1C) = AllFlags = 0xFFFFFFFF (SDOEx will AND
    // with 0xF3A7FFFF immediately to scrub disallowed bits).
    *(uint32_t*)(paramBuf + 0x1C) = 0xFFFFFFFFu;
    // ApplyFlags / ApplyInternalFlags / PortFlags / DuplicateMode at
    // +0x20..+0x2C all zero (NoFlags / None / 0 / Normal).
    // bAssignExternalPackages (+0x30) = 0 (false) - avoid the external-
    // package code path that faults on uninitialized state.
    // bSkipPostLoad (+0x31) = 0 (false).
    // DestClass (+0x38) = null - SDOEx uses source's class.
    // DuplicationSeed (+0x40, ~80 bytes) = zero (empty TMap valid).
    // CreatedObjects (+0x90 onward) = null.

    using SDOExFn = void* (__fastcall*)(void* /*FObjectDuplicationParameters&*/);
    auto fn = (SDOExFn)s_staticDuplicateObjectEx;
    __try {
        result = fn(paramBuf);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Hydro::logError("EngineAPI: staticDuplicateObject (SDOEx fallback) faulted "
                        "(src=%p outer=%p)", source, outer);
        return nullptr;
    }
    return result;
}

} // namespace Hydro::Engine
