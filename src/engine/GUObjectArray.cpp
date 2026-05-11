#include "GUObjectArray.h"
#include "../EngineAPI.h"
#include "../HydroCore.h"
#include "Internal.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace Hydro::Engine {

void* s_guObjectArray = nullptr;

bool findGUObjectArray() {
    uint8_t* strAddr = findWideString(s_gm.base, s_gm.size,
        L"Unable to add more objects to disregard for GC pool");

    if (!strAddr) {
        Hydro::logWarn("EngineAPI: GC pool string not found - trying alternate");
        strAddr = findWideString(s_gm.base, s_gm.size,
            L"MaxObjectsNotConsideredByGC");
        if (!strAddr) {
            Hydro::logError("EngineAPI: GUObjectArray strings not found");
            return false;
        }
    }

    Hydro::logInfo("EngineAPI: GC string at exe+0x%zX", (size_t)(strAddr - s_gm.base));

    uint8_t* leaAddr = findLeaRef(s_gm.base, s_gm.size, strAddr);
    if (!leaAddr) {
        Hydro::logError("EngineAPI: No LEA ref to GC string");
        return false;
    }

    uint8_t* funcStart = walkToFuncStart(leaAddr);
    Hydro::logInfo("EngineAPI: GC function at exe+0x%zX", (size_t)(funcStart - s_gm.base));

    // AllocateObjectPool is a member function - 'this' (rcx) is set by the
    // caller, not loaded internally. Scan callers for 'lea rcx, [rip+disp]'
    // to recover the GUObjectArray global address.
    Hydro::logInfo("EngineAPI: Searching for callers of AllocateObjectPool...");

    // Two global layouts seen across UE 5.x builds:
    //   (a) TUObjectArray-direct: global IS the FChunkedFixedUObjectArray
    //       (NumElements @ +0x14, Objects @ +0x00). Seen on UE 5.1.
    //   (b) FUObjectArray-wrapper: global is FUObjectArray with inner
    //       TUObjectArray ObjObjects at +0x10. Seen on UE 5.5+.
    // Probe both; accept the first with a plausible NumElements.
    constexpr int FARRAY_OBJOBJECTS_OFFSET = 0x10;  // FUObjectArray::ObjObjects
    auto tryAcceptCandidate = [&](void* resolved) -> bool {
        if ((uint8_t*)resolved < s_gm.base || (uint8_t*)resolved >= s_gm.base + s_gm.size)
            return false;
        int32_t numElems = 0;
        if (safeReadInt32((uint8_t*)resolved + FARRAY_NUM_ELEMS, &numElems) &&
            numElems > 1000 && numElems < 2000000) {
            s_guObjectArray = resolved;
            Hydro::logInfo("EngineAPI: GUObjectArray at %p (%d objects, TUObjectArray-direct shape)",
                resolved, numElems);
            return true;
        }
        uint8_t* inner = (uint8_t*)resolved + FARRAY_OBJOBJECTS_OFFSET;
        if (safeReadInt32(inner + FARRAY_NUM_ELEMS, &numElems) &&
            numElems > 1000 && numElems < 2000000) {
            s_guObjectArray = inner;
            Hydro::logInfo("EngineAPI: GUObjectArray at %p (%d objects, FUObjectArray-wrapper, inner TUObjectArray @ %p)",
                resolved, numElems, inner);
            return true;
        }
        return false;
    };

    for (size_t i = 0; i + 5 < s_gm.size; i++) {
        if (s_gm.base[i] != 0xE8) continue; // CALL rel32
        int32_t rel = *(int32_t*)(s_gm.base + i + 1);
        uint8_t* target = s_gm.base + i + 5 + rel;
        if (target != funcStart) continue;

        // Search backwards for 'lea rcx, [rip+disp32]' (48 8D 0D xx xx xx xx)
        for (int back = 5; back < 64; back++) {
            uint8_t* p = s_gm.base + i - back;
            if (p < s_gm.base) break;
            if (p[0] == 0x48 && p[1] == 0x8D && p[2] == 0x0D) {
                int32_t disp = *(int32_t*)(p + 3);
                void* resolved = p + 7 + disp;
                if (tryAcceptCandidate(resolved)) return true;
            }
        }
    }

    Hydro::logError("EngineAPI: GUObjectArray caller not found - trying data scan");

    // Fallback: scan writable PE sections for a plausible FChunkedFixedUObjectArray.
    // Layout probed:
    //   +0x00: void** Objects (chunk table - heap address)
    //   +0x08: void*  PreAllocated (may be null)
    //   +0x10: int32  MaxElements
    //   +0x14: int32  NumElements
    // Accept the candidate with the highest NumElements.

    auto* dosHeader = (IMAGE_DOS_HEADER*)s_gm.base;
    auto* ntHeaders = (IMAGE_NT_HEADERS*)(s_gm.base + dosHeader->e_lfanew);
    auto* section = IMAGE_FIRST_SECTION(ntHeaders);
    int numSections = ntHeaders->FileHeader.NumberOfSections;

    void* bestCandidate = nullptr;
    int32_t bestCount = 0;
    const char* bestShape = nullptr;

    uint8_t* moduleStart = s_gm.base;
    uint8_t* moduleEnd   = s_gm.base + s_gm.size;
    auto isHeapPtr = [&](void* p) {
        if (!p) return false;
        uint8_t* pb = (uint8_t*)p;
        if (pb >= moduleStart && pb < moduleEnd) return false;
        if ((uintptr_t)p < 0x10000) return false;
        if (((uintptr_t)p & 0x7) != 0) return false;
        return true;
    };

    // Returns NumElements if addr looks like a TUObjectArray: chunk table
    // dereferences, first 5 entries contain heap-resident UObjects with
    // heap-resident class pointers. Threshold of 1000 is intentionally low
    // to catch small games; structural validation filters random .data ints.
    auto probeShape = [&](uint8_t* addr) -> int32_t {
        int32_t numElems = *(int32_t*)(addr + FARRAY_NUM_ELEMS);
        if (numElems < 1000 || numElems > 2000000) return 0;
        int32_t maxElems = *(int32_t*)(addr + FARRAY_MAX_ELEMS);
        if (maxElems < numElems) return 0;
        void* objects = *(void**)addr;
        if (!isHeapPtr(objects)) return 0;
        void* chunk0 = nullptr;
        if (!safeReadPtr(objects, &chunk0) || !chunk0) return 0;

        int validEntries = 0;
        int probedEntries = 0;
        for (int e = 0; e < 5; e++) {
            void* obj = nullptr;
            if (!safeReadPtr((uint8_t*)chunk0 + (e * FUOBJ_SIZE) + FUOBJ_OBJECT, &obj)) continue;
            if (!obj) continue;
            probedEntries++;
            if (!isHeapPtr(obj)) break;
            void* cls = nullptr;
            if (!safeReadPtr((uint8_t*)obj + UOBJ_CLASS, &cls)) break;
            if (!cls) break;
            if (!isHeapPtr(cls)) break;
            validEntries++;
        }
        if (validEntries < 3) return 0;
        return numElems;
    };

    for (int sec = 0; sec < numSections; sec++, section++) {
        if (!(section->Characteristics & IMAGE_SCN_MEM_WRITE)) continue;

        uint8_t* secStart = s_gm.base + section->VirtualAddress;
        size_t secSize = section->Misc.VirtualSize;
        if (secSize < 0x30) continue;

        // Scan at 8-byte alignment. Probe both global layouts at each address:
        //   (a) TUObjectArray-direct - UE 5.1
        //   (b) FUObjectArray-wrapper, inner TUObjectArray @ +0x10 - UE 5.5+
        for (size_t off = 0; off + 0x30 <= secSize; off += 8) {
            uint8_t* addr = secStart + off;

            int32_t n = probeShape(addr);
            if (n > 0) {
                Hydro::logInfo("EngineAPI: GUObjectArray candidate at %p (%d objects, exe+0x%zX, TUObjectArray-direct)",
                    addr, n, (size_t)(addr - s_gm.base));
                if (n > bestCount) {
                    bestCandidate = addr;
                    bestCount = n;
                    bestShape = "TUObjectArray-direct";
                }
                continue;
            }

            uint8_t* inner = addr + 0x10;
            n = probeShape(inner);
            if (n > 0) {
                Hydro::logInfo("EngineAPI: GUObjectArray candidate at %p (FUObjectArray, inner @ %p, %d objects, exe+0x%zX)",
                    addr, inner, n, (size_t)(addr - s_gm.base));
                if (n > bestCount) {
                    // Store inner pointer - iteration code uses TUObjectArray-direct offsets unchanged.
                    bestCandidate = inner;
                    bestCount = n;
                    bestShape = "FUObjectArray-wrapper";
                }
            }
        }
    }

    if (bestCandidate) {
        s_guObjectArray = bestCandidate;
        Hydro::logInfo("EngineAPI: GUObjectArray at %p (%d objects, %s) - found via data section scan",
            bestCandidate, bestCount, bestShape ? bestShape : "?");
        return true;
    }

    Hydro::logError("EngineAPI: GUObjectArray not found by any method");
    return false;
}

void* getObjectAt(int32_t index) {
    if (!s_guObjectArray || index < 0) return nullptr;

    void** chunkTable = nullptr;
    if (!safeReadPtr((uint8_t*)s_guObjectArray + FARRAY_OBJECTS, (void**)&chunkTable) || !chunkTable)
        return nullptr;

    int32_t chunkIndex = index / CHUNK_SIZE;
    int32_t withinChunk = index % CHUNK_SIZE;

    void* chunk = nullptr;
    if (!safeReadPtr(&chunkTable[chunkIndex], &chunk) || !chunk)
        return nullptr;

    // Each FUObjectItem is FUOBJ_SIZE bytes. Object pointer is at offset 0.
    uint8_t* item = (uint8_t*)chunk + (withinChunk * FUOBJ_SIZE);
    void* obj = nullptr;
    if (!safeReadPtr(item + FUOBJ_OBJECT, &obj))
        return nullptr;

    return obj;
}

// Return the FUObjectItem* at the given index (a pointer into the chunk
// table), NOT the UObject* it wraps. Callers can read the item's flags or
// re-check its Object pointer for stale-handle detection.
void* getObjectItemAt(int32_t index) {
    if (!s_guObjectArray || index < 0) return nullptr;
    void** chunkTable = nullptr;
    if (!safeReadPtr((uint8_t*)s_guObjectArray + FARRAY_OBJECTS, (void**)&chunkTable) || !chunkTable)
        return nullptr;
    int32_t chunkIndex = index / CHUNK_SIZE;
    int32_t withinChunk = index % CHUNK_SIZE;
    void* chunk = nullptr;
    if (!safeReadPtr(&chunkTable[chunkIndex], &chunk) || !chunk)
        return nullptr;
    return (uint8_t*)chunk + (withinChunk * FUOBJ_SIZE);
}

int32_t getObjectCount() {
    if (!s_guObjectArray) return 0;
    int32_t count = 0;
    safeReadInt32((uint8_t*)s_guObjectArray + FARRAY_NUM_ELEMS, &count);
    return count;
}

uint32_t getNameIndex(void* obj) {
    if (!obj) return 0;
    uint32_t idx = 0;
#ifdef _WIN32
    __try { idx = *(uint32_t*)((uint8_t*)obj + UOBJ_NAME); }
    __except(1) { return 0; }
#else
    idx = *(uint32_t*)((uint8_t*)obj + UOBJ_NAME);
#endif
    return idx;
}

// Read ClassPrivate from a UObject (offset 0x10).
// Early internal version used by discovery code before the public API section.
// The public getClass() at the bottom of EngineAPI.cpp is a different function
// that performs UClass-resolved type checks.
void* getObjClass(void* obj) {
    if (!obj) return nullptr;
    void* cls = nullptr;
    safeReadPtr((uint8_t*)obj + UOBJ_CLASS, &cls);
    return cls;
}

} // namespace Hydro::Engine
