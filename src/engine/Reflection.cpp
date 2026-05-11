#include "Reflection.h"
#include "../EngineAPI.h"
#include "../HydroCore.h"
#include "Internal.h"
#include "FName.h"
#include "Layout.h"

#include <unordered_map>

#ifdef _WIN32
#include <windows.h>
#endif

namespace Hydro::Engine {

// -- findFunctionOnClass ----------------------------------------------------

// Walks UClass::Children (UField linked list) and up the SuperStruct chain.
// Inherited functions live on ancestor UClasses, so we must climb all the way
// up rather than stopping at the concrete class.
void* findFunctionOnClass(void* uclass, const wchar_t* funcName) {
    if (!uclass || !funcName || !s_fnameConstructor) return nullptr;

    FName8 targetName = {};
    if (!safeConstructFName(&targetName, funcName)) return nullptr;

    void* current = uclass;
    int classDepth = 0;
    while (current && classDepth < 32) {
        void* child = nullptr;
        safeReadPtr((uint8_t*)current + USTRUCT_CHILDREN, &child);

        int count = 0;
        while (child && count < 500) {
            uint32_t nameIdx = 0;
            safeReadInt32((uint8_t*)child + UOBJ_NAME, (int32_t*)&nameIdx);

            if (nameIdx == targetName.comparisonIndex) {
                return child;
            }

            void* next = nullptr;
            safeReadPtr((uint8_t*)child + UFIELD_NEXT, &next);
            child = next;
            count++;
        }

        void* super = nullptr;
        safeReadPtr((uint8_t*)current + (s_superOffset < 0 ? 0x40 : s_superOffset), &super);
        if (super == current) break;
        current = super;
        classDepth++;
    }

    return nullptr;
}

// -- findFunction / findProperty --------------------------------------------

void* findFunction(void* uclass, const wchar_t* funcName) {
    return findFunctionOnClass(uclass, funcName);
}

void* findProperty(void* uclass, const wchar_t* propName) {
    if (!uclass || !propName || !s_fnameConstructor) return nullptr;

    FName8 targetName = {};
    if (!safeConstructFName(&targetName, propName)) return nullptr;

    // Walk ChildProperties on this class, then up the SuperStruct chain.
    // Inherited properties live on ancestor classes' FField list, not the
    // subclass. Use discovered chain-walk offsets (Layer 2) with hardcoded
    // fallback - UE 5.1-derived hosts shift these by +8 bytes vs. stock UE 5.5.
    int chOff   = (s_layout.childPropsOffset >= 0) ? s_layout.childPropsOffset : USTRUCT_CHILD_PROPS;
    int nextOff = (s_layout.fieldNextOffset  >= 0) ? s_layout.fieldNextOffset  : FFIELD_NEXT;
    int nameOff = (s_layout.fieldNameOffset  >= 0) ? s_layout.fieldNameOffset  : FFIELD_NAME;

    void* current = uclass;
    int classDepth = 0;
    while (current && classDepth < 32) {
        void* prop = nullptr;
        safeReadPtr((uint8_t*)current + chOff, &prop);

        int count = 0;
        while (prop && count < 500) {
            uint32_t nameIdx = 0;
            safeReadInt32((uint8_t*)prop + nameOff, (int32_t*)&nameIdx);

            if (nameIdx == targetName.comparisonIndex) {
                return prop;
            }

            void* next = nullptr;
            safeReadPtr((uint8_t*)prop + nextOff, &next);
            prop = next;
            count++;
        }

        void* super = nullptr;
        safeReadPtr((uint8_t*)current + (s_superOffset < 0 ? 0x40 : s_superOffset), &super);
        if (super == current) break;
        current = super;
        classDepth++;
    }
    return nullptr;
}

// -- Property field readers -------------------------------------------------

uint64_t getPropertyFlags(void* prop) {
    if (!prop) return 0;
    if (!s_layout.initialized) discoverPropertyLayout();
    int off = (s_layout.flags >= 0) ? s_layout.flags : FPROP_FLAGS;
    uint64_t flags = 0;
    safeReadU64((uint8_t*)prop + off, &flags);
    return flags;
}

void* getArrayInner(void* arrayProp) {
    if (!arrayProp) return nullptr;
    if (!s_layout.initialized) discoverPropertyLayout();
    if (s_layout.arrayInner < 0) return nullptr;
    void* inner = nullptr;
    safeReadPtr((uint8_t*)arrayProp + s_layout.arrayInner, &inner);
    return inner;
}

void* getStructStruct(void* structProp) {
    if (!structProp) return nullptr;
    if (!s_layout.initialized) discoverPropertyLayout();
    if (s_layout.structStruct < 0) return nullptr;
    void* sstruct = nullptr;
    safeReadPtr((uint8_t*)structProp + s_layout.structStruct, &sstruct);
    return sstruct;
}

// -- UFunction parameter layout cache --------------------------------------
//
// UFunction::ParmsSize and ReturnValueOffset are redundant with what the
// parameter FProperty chain already encodes:
//   ParmsSize         = max(Offset_Internal + ElementSize) across all params
//   ReturnValueOffset = Offset_Internal of the CPF_ReturnParm property
//
// Walking the chain (using Layer 2's discovered offsets) eliminates
// UFUNC_PARMS_SIZE / UFUNC_RET_VAL_OFFSET as load-bearing constants - they
// become bootstrap fallbacks only. Cache is keyed per UFunction pointer;
// evicted when Layer 2 offsets are updated (clearUFuncLayoutCache).

static std::unordered_map<void*, UFuncLayoutCache> s_ufuncLayouts;

void clearUFuncLayoutCache() { s_ufuncLayouts.clear(); }

static UFuncLayoutCache computeUFunctionLayout(void* ufunc) {
    UFuncLayoutCache out = {0, 0, false, false};
    if (!ufunc) return out;

    void* prop = getChildProperties(ufunc);

    int count = 0;
    int paramCount = 0;
    while (prop && count < 64) {
        int32_t off    = getPropertyOffset(prop);
        int32_t sz     = getPropertyElementSize(prop);
        uint64_t pflags = getPropertyFlags(prop);

        // Defensive: skip clearly-bogus values rather than poison the result.
        if (off >= 0 && off < 4096 && sz > 0 && sz < 1024) {
            uint32_t end = (uint32_t)off + (uint32_t)sz;
            if (end > out.parmsSize && end < 4096)
                out.parmsSize = (uint16_t)end;

            if (pflags & CPF_ReturnParm) {
                out.retOffset = (uint16_t)off;
                out.hasReturn = true;
            }
            paramCount++;
        }

        prop = getNextProperty(prop);
        count++;
    }

    out.derived = (paramCount > 0);
    return out;
}

static UFuncLayoutCache getUFuncLayoutCached(void* ufunc) {
    auto it = s_ufuncLayouts.find(ufunc);
    if (it != s_ufuncLayouts.end()) return it->second;

    UFuncLayoutCache layout = computeUFunctionLayout(ufunc);

    // Fallback: if the chain walk yielded nothing (e.g. native UFunction with
    // no reflected parameters, or Layer 2 not yet up), fall back to the raw
    // hardcoded offsets. Better wrong than zero - avoids silent ProcessEvent
    // failures on bootstrap.
    if (!layout.derived) {
        uint16_t ps = 0, ro = 0;
#ifdef _WIN32
        __try {
            ps = *(uint16_t*)((uint8_t*)ufunc + UFUNC_PARMS_SIZE);
            ro = *(uint16_t*)((uint8_t*)ufunc + UFUNC_RET_VAL_OFFSET);
        } __except(1) {}
#endif
        layout.parmsSize = ps;
        layout.retOffset = ro;
        layout.hasReturn = (ro > 0 && ro < ps);
    }

    s_ufuncLayouts[ufunc] = layout;
    return layout;
}

uint16_t getUFunctionParmsSize(void* ufunc) {
    if (!ufunc) return 0;
    return getUFuncLayoutCached(ufunc).parmsSize;
}

uint16_t getUFunctionRetOffset(void* ufunc) {
    if (!ufunc) return 0;
    return getUFuncLayoutCached(ufunc).retOffset;
}

// -- Class / enum helpers ---------------------------------------------------

uint32_t modifyClassFlags(void* cls, uint32_t setMask, uint32_t clearMask) {
    if (!cls) return 0;
    if (s_classFlagsOffset < 0) discoverClassFlagsOffset();
    uint32_t* slot = (uint32_t*)((uint8_t*)cls + s_classFlagsOffset);
    uint32_t before = 0;
    if (!safeReadInt32(slot, (int32_t*)&before)) return 0;
    uint32_t after = (before & ~clearMask) | setMask;
    *slot = after;
    return after;
}

int readEnumNames(void* uenum, std::vector<std::pair<uint32_t, int64_t>>& out) {
    if (!uenum) return 0;
    if (s_enumNamesOffset < 0) discoverEnumNamesOffset();

    void* data = nullptr;
    if (!safeReadPtr((uint8_t*)uenum + s_enumNamesOffset, &data) || !data) return 0;

    int32_t num = 0;
    if (!safeReadInt32((uint8_t*)uenum + s_enumNamesOffset + 8, &num)) return 0;
    if (num <= 0 || num > 10000) return 0;

    out.reserve(num);
    for (int32_t i = 0; i < num; i++) {
        uint8_t* entry = (uint8_t*)data + (size_t)i * 16;
        uint32_t idx = 0;
        int64_t val = 0;
        if (!safeReadInt32(entry, (int32_t*)&idx)) break;
        // int64 via two 32-bit reads
        int32_t lo = 0, hi = 0;
        if (!safeReadInt32(entry + 8, &lo)) break;
        if (!safeReadInt32(entry + 12, &hi)) break;
        val = ((int64_t)hi << 32) | (uint32_t)lo;
        out.emplace_back(idx, val);
    }
    return (int)out.size();
}

} // namespace Hydro::Engine
