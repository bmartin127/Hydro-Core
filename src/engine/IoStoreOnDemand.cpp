#include "IoStoreOnDemand.h"
#include "../EngineAPI.h"
#include "../HydroCore.h"
#include "Internal.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include <algorithm>
#include <cstring>
#include <vector>

namespace Hydro::Engine {

void*    s_ioStoreOnDemandMount           = nullptr;
uint8_t* s_ioStoreOnDemandSingletonGlobal = nullptr;

// On IoStore-shipped UE 5.4+ cooks, FPakPlatformFile::Mount is decoupled from
// FIoDispatcher's FPackageStore - files mount but assets stay invisible to
// StaticLoadObject / AssetRegistry. UE::IoStore::OnDemand::Mount is UE's own
// runtime injection path that feeds FPackageStore.
//
// Anchor: L"Mount requests pending for MountId" - uniquely tied to Mount's
// args-validation path. .pdata-resolve gives the function start without the
// prologue-walk fragility that breaks on SSE-save prologues.
//
// On hosts that don't link IoStoreOnDemandCore (pre-UE 5.4 or bUseIoStore=False
// cooks), the anchor string isn't present and discovery returns false; callers
// fall back to the legacy PakLoader path.
bool findIoStoreOnDemandMount() {
    if (!s_gm.base) return false;

    // UE_LOG TEXT(...) macros emit UTF-16LE wide-string literals.
    const wchar_t* needle = L"Mount requests pending for MountId";
    uint8_t* strAddr = findWideString(s_gm.base, s_gm.size, needle);
    if (!strAddr) {
        Hydro::logInfo("EngineAPI: IoStoreOnDemand::Mount anchor not found "
            "(host doesn't link IoStoreOnDemandCore - pre-5.4 or non-IoStore cook)");
        return false;
    }
    Hydro::logInfo("EngineAPI: IoStoreOnDemand Mount anchor at exe+0x%zX",
        (size_t)(strAddr - s_gm.base));

    uint8_t* leaAddr = findLeaRef(s_gm.base, s_gm.size, strAddr);
    if (!leaAddr) {
        Hydro::logWarn("EngineAPI: No LEA ref to IoStoreOnDemand Mount anchor");
        return false;
    }
    Hydro::logInfo("EngineAPI: IoStoreOnDemand Mount LEA at exe+0x%zX",
        (size_t)(leaAddr - s_gm.base));

    uint8_t* funcStart = funcStartViaPdata(leaAddr);
    if (!funcStart) {
        Hydro::logWarn("EngineAPI: IoStoreOnDemand Mount LEA has no containing .pdata function");
        return false;
    }

    s_ioStoreOnDemandMount = funcStart;
    uint32_t beginRva = (uint32_t)(funcStart - s_gm.base);

    // Prologue dump logged in 64-byte chunks (ue4ss logger truncates long lines).
    // Call shape (member fn vs free fn) and FOnDemandMountArgs field offsets are
    // read off this output.
    const size_t dumpLen = 512;
    for (size_t chunk = 0; chunk < dumpLen; chunk += 64) {
        char hexLine[3 * 64 + 1] = {};
        size_t off = 0;
        for (size_t i = 0; i < 64 && chunk + i < dumpLen && chunk + i < s_gm.size - beginRva; i++) {
            off += snprintf(hexLine + off, sizeof(hexLine) - off, "%02X ", funcStart[chunk + i]);
        }
        Hydro::logInfo("EngineAPI: IoStoreOnDemand Mount at exe+0x%X +0x%02zX: %s",
            beginRva, chunk, hexLine);
    }

    return true;
}

// Mount is dispatched only via vtable (no direct call sites in shipping cooks).
// Strategy:
//   1. Scan module QWORDs for Mount's address to find vtable slots.
//   2. Walk back over consecutive .text-pointing QWORDs to find each vtable start.
//   3. Cache the first vtable start in s_ioStoreOnDemandSingletonGlobal as the
//      match key for getIoStoreOnDemandSingleton's runtime instance scan.
bool findIoStoreOnDemandVtable() {
    if (!s_gm.base || !s_ioStoreOnDemandMount) return false;

    uint64_t mountAddr = (uint64_t)s_ioStoreOnDemandMount;
    Hydro::logInfo("EngineAPI: IoStoreOnDemand vtable scan - searching for Mount @ %p as QWORD",
        s_ioStoreOnDemandMount);

    std::vector<uint8_t*> slotMatches;
    for (size_t i = 0; i + 8 <= s_gm.size; i += 8) {
        uint64_t qw;
        memcpy(&qw, s_gm.base + i, 8);
        if (qw == mountAddr) slotMatches.push_back(s_gm.base + i);
    }

    if (slotMatches.empty()) {
        Hydro::logWarn("EngineAPI: IoStoreOnDemand vtable scan: no QWORD matches for Mount address "
            "(unexpected - Mount must be in some vtable to be callable virtually)");
        return false;
    }
    Hydro::logInfo("EngineAPI: IoStoreOnDemand vtable scan: %zu QWORD matches", slotMatches.size());

    auto isTextPtr = [&](uint64_t p) {
        return p >= (uint64_t)s_gm.base && p < (uint64_t)(s_gm.base + s_gm.size);
    };

    std::vector<uint8_t*> vtableStarts;
    for (uint8_t* slot : slotMatches) {
        uint8_t* p = slot;
        while (p > s_gm.base + 8) {
            uint64_t prev;
            memcpy(&prev, p - 8, 8);
            if (!isTextPtr(prev)) break;
            p -= 8;
        }
        vtableStarts.push_back(p);
    }
    std::sort(vtableStarts.begin(), vtableStarts.end());
    vtableStarts.erase(std::unique(vtableStarts.begin(), vtableStarts.end()), vtableStarts.end());

    Hydro::logInfo("EngineAPI: IoStoreOnDemand vtable scan: %zu unique vtable start(s)",
        vtableStarts.size());

    for (size_t i = 0; i < vtableStarts.size() && i < 5; i++) {
        uint8_t* vt = vtableStarts[i];
        size_t slotsForward = 0;
        for (uint8_t* p = vt; p + 8 <= s_gm.base + s_gm.size; p += 8, slotsForward++) {
            uint64_t qw;
            memcpy(&qw, p, 8);
            if (!isTextPtr(qw)) break;
        }
        Hydro::logInfo("  vtable[%zu] at exe+0x%zX (~%zu slots)",
            i, (size_t)(vt - s_gm.base), slotsForward);
    }

    // Pick the first (lowest-address) vtable. Derived-class vtables copy the
    // inherited Mount slot, so multiple matches are expected; the base
    // IOnDemandIoStore vtable is typically first in memory order.
    s_ioStoreOnDemandSingletonGlobal = vtableStarts[0];
    Hydro::logInfo("EngineAPI: IOnDemandIoStore vtable cached at exe+0x%zX",
        (size_t)(s_ioStoreOnDemandSingletonGlobal - s_gm.base));
    return true;
}

void* getIoStoreOnDemandMountFn() { return s_ioStoreOnDemandMount; }

void* getIoStoreOnDemandSingleton() {
    static void* s_cached = nullptr;
    if (s_cached) return s_cached;
    if (!s_ioStoreOnDemandSingletonGlobal || !s_gm.base) return nullptr;

    // Throttle: ~50 ms cost on a 150 MB exe. Once every 2 seconds catches
    // subsystem binding without stalling the game thread on every tick.
    static uint64_t s_lastScan = 0;
    uint64_t now = GetTickCount64();
    if (now - s_lastScan < 2000) return nullptr;
    s_lastScan = now;

    // s_ioStoreOnDemandSingletonGlobal holds the IOnDemandIoStore vtable
    // address. Scan module-data QWORDs for heap pointers whose first 8 bytes
    // equal that vtable - UE keeps the owning global in the exe's .data
    // section even when the instance lives on the heap.
    uint64_t vtableAddr = (uint64_t)s_ioStoreOnDemandSingletonGlobal;
    int derefAttempts = 0;
    for (size_t i = 0; i + 8 <= s_gm.size; i += 8) {
        uint64_t maybePtr;
        memcpy(&maybePtr, s_gm.base + i, 8);
        if (maybePtr < 0x10000) continue;
        if (maybePtr >= (uint64_t)s_gm.base && maybePtr < (uint64_t)(s_gm.base + s_gm.size)) continue;

        uint64_t destVtable;
        if (!safeReadPtr((void*)maybePtr, (void**)&destVtable)) continue;
        derefAttempts++;
        if (destVtable == vtableAddr) {
            s_cached = (void*)maybePtr;
            Hydro::logInfo("EngineAPI: IOnDemandIoStore singleton found at %p "
                "(via global at exe+0x%zX, %d derefs)",
                s_cached, i, derefAttempts);
            return s_cached;
        }
    }
    Hydro::logInfo("EngineAPI: IOnDemandIoStore singleton not yet bound "
        "(scanned %zu module-data QWORDs, %d valid derefs, none vtable-matching)",
        s_gm.size / 8, derefAttempts);
    return nullptr;
}

} // namespace Hydro::Engine
