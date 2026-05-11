#include "RawFunctions.h"
#include "EngineAPI.h"
#include "HydroCore.h"
#include "SEH.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

#include <Zydis/Zydis.h>

/*
 * RawFunctions implementation.
 *
 * Three strategies share three primitives:
 *   - findWideString: linear scan for a UTF-16 literal in the module
 *   - findLeaRef:     scan for a LEA instruction whose disp32 lands on `target`
 *   - walkToFuncStart: walk backward from a code address until CC padding
 *
 * These mirror the helpers in EngineAPI.cpp; reimplementing them here keeps
 * RawFunctions self-contained (no internal-header dependency) and the code
 * footprint is < 30 lines.
 *
 * Cache file format (hydro_raw_funcs.bin):
 *   header:
 *     u32 magic (CACHE_MAGIC)
 *     u64 moduleSize
 *     u32 entryCount
 *   entries[entryCount]:
 *     u16  nameLen
 *     char name[nameLen]
 *     i64  offsetFromModuleBase   (-1 sentinel = discovery failed; cached so
 *                                  we don't re-attempt on every launch)
 *
 * Invalidation: header magic + moduleSize mismatch wipes the cache. Manual
 * invalidate(name) drops the single entry without touching the rest.
 */

namespace Hydro::Engine::RawFn {

namespace {

constexpr uint32_t CACHE_MAGIC = 0x52415746;  // 'RAWF'

// In-memory state
std::mutex                                       s_mutex;
std::unordered_map<std::string, Descriptor>      s_descriptors;
std::unordered_map<std::string, void*>           s_resolved;
std::unordered_map<std::string, ptrdiff_t>       s_offsetCache;  // populated from disk on first use
bool                                             s_cacheLoaded = false;

// Resolved offsets coming from the on-disk cache use a sentinel so a
// "we already tried, it failed" outcome isn't re-attempted every launch.
constexpr ptrdiff_t FAILED_SENTINEL = -1;

// -- Module helpers -------------------------------------------------------

uint8_t* findWideStringIn(uint8_t* base, size_t size, const wchar_t* str) {
    size_t strBytes = wcslen(str) * 2;
    if (strBytes == 0 || strBytes > size) return nullptr;
    for (size_t i = 0; i + strBytes <= size; i++) {
        if (memcmp(base + i, str, strBytes) == 0) return base + i;
    }
    return nullptr;
}

uint8_t* findLeaRefIn(uint8_t* base, size_t size, uint8_t* target) {
    if (size < 8) return nullptr;
    for (size_t i = 0; i + 7 < size; i++) {
        if ((base[i] == 0x48 || base[i] == 0x4C) && base[i + 1] == 0x8D) {
            uint8_t modrm = base[i + 2];
            // ModR/M with mod=00 reg=*** rm=101 -> rip-relative disp32.
            // Accept both lo (rax/rcx/...) and hi (r8..r15 - REX.R bit) regs.
            if ((modrm & 0xC7) != 0x05 && (modrm & 0xC7) != 0x0D &&
                (modrm & 0xC7) != 0x15 && (modrm & 0xC7) != 0x1D &&
                (modrm & 0xC7) != 0x25 && (modrm & 0xC7) != 0x2D &&
                (modrm & 0xC7) != 0x35 && (modrm & 0xC7) != 0x3D) continue;
            int32_t disp = *(int32_t*)(base + i + 3);
            if (base + i + 7 + disp == target) return base + i;
        }
    }
    return nullptr;
}

uint8_t* walkToFuncStartFrom(uint8_t* addr, int maxBack = 16384) {
    uint8_t* p = addr;
    for (int i = 0; i < maxBack; i++) {
        if (p[-1] == 0xCC) return p;
        p--;
    }
    return addr;
}

// Follow chained jmp trampolines. Mirrors HydroEvents.cpp::resolveJmpTrampoline
// - kept private here so RawFunctions doesn't pull in HydroEvents' headers.
void* resolveTrampoline(void* addr, int depth = 0) {
    if (!addr || depth >= 8) return addr;
    const uint8_t* p = (const uint8_t*)addr;
    if (p[0] == 0xE9) {
        int32_t rel = *(const int32_t*)(p + 1);
        return resolveTrampoline((uint8_t*)addr + 5 + rel, depth + 1);
    }
    if (p[0] == 0xFF && p[1] == 0x25) {
        int32_t disp = *(const int32_t*)(p + 2);
        void** slot = (void**)((uint8_t*)addr + 6 + disp);
        void* target = nullptr;
        if (Engine::readPtr(slot, &target) && target) {
            return resolveTrampoline(target, depth + 1);
        }
    }
    return addr;
}

// -- Cache file ----------------------------------------------------------

std::string cachePath() {
    char path[MAX_PATH];
    HMODULE hm = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&cachePath, &hm);
    GetModuleFileNameA(hm, path, MAX_PATH);
    std::string dir(path);
    auto pos = dir.find_last_of("\\/");
    if (pos != std::string::npos) dir = dir.substr(0, pos);
    return dir + "\\hydro_raw_funcs.bin";
}

void loadCache() {
    if (s_cacheLoaded) return;
    s_cacheLoaded = true;

    size_t modSize = Engine::getGameModuleSize();
    if (modSize == 0) return;  // engine not up yet - load deferred to next call

    FILE* f = fopen(cachePath().c_str(), "rb");
    if (!f) return;

    auto closer = [&]() { fclose(f); };

    uint32_t magic = 0; uint64_t cachedSize = 0; uint32_t count = 0;
    if (fread(&magic, 4, 1, f) != 1 || magic != CACHE_MAGIC) { closer(); return; }
    if (fread(&cachedSize, 8, 1, f) != 1 || cachedSize != modSize) {
        // Module size mismatch = game patched. Wipe and start clean.
        closer();
        std::error_code ec;
        std::filesystem::remove(cachePath(), ec);
        Hydro::logInfo("RawFunctions: cache invalidated (module size %llu -> %zu)",
            (unsigned long long)cachedSize, modSize);
        return;
    }
    if (fread(&count, 4, 1, f) != 1) { closer(); return; }
    if (count > 4096) { closer(); return; }  // sanity - we'd never register that many

    for (uint32_t i = 0; i < count; i++) {
        uint16_t nameLen = 0;
        if (fread(&nameLen, 2, 1, f) != 1 || nameLen > 256) { closer(); return; }
        std::string name(nameLen, '\0');
        if (nameLen && fread(name.data(), 1, nameLen, f) != nameLen) { closer(); return; }
        int64_t off = 0;
        if (fread(&off, 8, 1, f) != 1) { closer(); return; }
        s_offsetCache[name] = (ptrdiff_t)off;
    }
    closer();
    Hydro::logInfo("RawFunctions: loaded %zu cached offsets", s_offsetCache.size());
}

void saveCache() {
    size_t modSize = Engine::getGameModuleSize();
    if (modSize == 0) return;
    FILE* f = fopen(cachePath().c_str(), "wb");
    if (!f) return;
    uint32_t magic = CACHE_MAGIC;
    uint64_t sz = modSize;
    uint32_t count = (uint32_t)s_offsetCache.size();
    fwrite(&magic, 4, 1, f);
    fwrite(&sz, 8, 1, f);
    fwrite(&count, 4, 1, f);
    for (auto& kv : s_offsetCache) {
        uint16_t nameLen = (uint16_t)std::min<size_t>(kv.first.size(), 0xFFFF);
        fwrite(&nameLen, 2, 1, f);
        fwrite(kv.first.data(), 1, nameLen, f);
        int64_t off = kv.second;
        fwrite(&off, 8, 1, f);
    }
    fclose(f);
}

// -- Discovery strategies ------------------------------------------------

void* discoverUFuncImpl(const Descriptor& d) {
    void* ufunc = Engine::findObject(d.anchor.c_str());
    if (!ufunc) return nullptr;
    void* funcPtr = nullptr;
    if (!Engine::readPtr((uint8_t*)ufunc + Engine::UFUNC_FUNC, &funcPtr) || !funcPtr)
        return nullptr;
    return resolveTrampoline(funcPtr);
}

// Bounded Zydis walk: decode instructions starting at `body`, return the
// address called by the Nth CALL (rel32). Stops at the first RET or after
// a generous instruction budget - most function bodies that matter for raw
// discovery are well under that. n is 1-based.
void* findNthCallIn(void* body, int n) {
    if (!body || n <= 0) return nullptr;

    ZydisDecoder decoder;
    if (ZYAN_FAILED(ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64,
                                     ZYDIS_STACK_WIDTH_64))) return nullptr;

    const uint8_t* p = (const uint8_t*)body;
    constexpr size_t kWindow = 4096;  // generous - covers most non-reflected impls
    int callsSeen = 0;

    for (size_t i = 0; i < kWindow; ) {
        ZydisDecodedInstruction inst;
        ZydisDecodedOperand     ops[ZYDIS_MAX_OPERAND_COUNT];
        if (ZYAN_FAILED(ZydisDecoderDecodeFull(&decoder, p + i, kWindow - i,
                                               &inst, ops))) break;

        if (inst.mnemonic == ZYDIS_MNEMONIC_CALL && inst.operand_count >= 1 &&
            ops[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE &&
            ops[0].imm.is_relative) {
            if (++callsSeen == n) {
                int64_t rel = ops[0].imm.value.s;
                void* target = (uint8_t*)body + i + inst.length + rel;
                return resolveTrampoline(target);
            }
        }
        // RET ends the function; INT3 padding ends a basic-block region but
        // we keep going since some functions get padded mid-body by /Gy.
        if (inst.mnemonic == ZYDIS_MNEMONIC_RET) break;
        i += inst.length;
    }
    return nullptr;
}

void* discoverNthCall(const Descriptor& d) {
    void* ufunc = Engine::findObject(d.anchor.c_str());
    if (!ufunc) return nullptr;
    void* funcPtr = nullptr;
    if (!Engine::readPtr((uint8_t*)ufunc + Engine::UFUNC_FUNC, &funcPtr) || !funcPtr)
        return nullptr;
    void* body = resolveTrampoline(funcPtr);
    return findNthCallIn(body, d.nthCall);
}

void* discoverStringRefAnchor(const Descriptor& d) {
    uint8_t* base = Engine::getGameModuleBase();
    size_t   size = Engine::getGameModuleSize();
    if (!base || size == 0) return nullptr;

    uint8_t* strAddr = findWideStringIn(base, size, d.anchor.c_str());
    if (!strAddr) return nullptr;

    uint8_t* leaInstr = findLeaRefIn(base, size, strAddr);
    if (!leaInstr) return nullptr;

    // Walk backward to function start (CC padding). walkToFuncStartFrom is
    // generous - string refs often sit deep in a function body.
    return walkToFuncStartFrom(leaInstr);
}

// Inner dispatch - must NOT contain any locals with non-trivial destructors
// (HYDRO_SEH_TRY / __try is incompatible with C++ unwinding under MSVC).
// All std::wstring usage stays inside the dispatched routines, which return
// raw pointers; only primitive types cross the SEH boundary here.
bool dispatchSEH(const Descriptor* d, void** out) {
#ifdef _WIN32
    __try {
        switch (d->strategy) {
            case Strategy::UFuncImpl:       *out = discoverUFuncImpl(*d); break;
            case Strategy::NthCallInUFunc:  *out = discoverNthCall(*d); break;
            case Strategy::StringRefAnchor: *out = discoverStringRefAnchor(*d); break;
        }
        return true;
    } __except (1) {
        *out = nullptr;
        return false;
    }
#else
    switch (d->strategy) {
        case Strategy::UFuncImpl:       *out = discoverUFuncImpl(*d); break;
        case Strategy::NthCallInUFunc:  *out = discoverNthCall(*d); break;
        case Strategy::StringRefAnchor: *out = discoverStringRefAnchor(*d); break;
    }
    return true;
#endif
}

// Dispatch to the appropriate strategy. Wrapped in SEH so a misbehaving
// descriptor can't take down the host - discovery returning nullptr is the
// expected failure path. Caller passes a Descriptor by const-ref; we hand
// it through a pointer so the SEH-wrapped frame holds no destructors.
void* dispatch(const Descriptor& d) {
    void* result = nullptr;
    if (!dispatchSEH(&d, &result)) {
        Hydro::logWarn("RawFunctions: discovery faulted for strategy %d",
            (int)d.strategy);
    }
    return result;
}

} // anonymous namespace

// -- Public API ---------------------------------------------------------

void registerFn(const std::string& name, const Descriptor& desc) {
    std::lock_guard<std::mutex> lk(s_mutex);
    s_descriptors.emplace(name, desc);  // first-wins; idempotent
}

void* resolve(const std::string& name) {
    std::lock_guard<std::mutex> lk(s_mutex);

    // Fast path: already resolved this launch.
    auto live = s_resolved.find(name);
    if (live != s_resolved.end()) return live->second;

    loadCache();

    auto descIt = s_descriptors.find(name);
    if (descIt == s_descriptors.end()) {
        Hydro::logWarn("RawFunctions: resolve(\"%s\") - not registered", name.c_str());
        return nullptr;
    }

    // Try cache first (already filtered by module size on load).
    uint8_t* base = Engine::getGameModuleBase();
    auto cachedIt = s_offsetCache.find(name);
    if (cachedIt != s_offsetCache.end() && base) {
        if (cachedIt->second == FAILED_SENTINEL) {
            // We tried and failed last launch under the same module size -
            // don't burn cycles re-attempting. Game patch will reset this.
            s_resolved[name] = nullptr;
            return nullptr;
        }
        void* addr = base + cachedIt->second;
        s_resolved[name] = addr;
        return addr;
    }

    // Cache miss - actually walk the descriptor.
    void* addr = dispatch(descIt->second);
    s_resolved[name] = addr;

    if (addr && base) {
        s_offsetCache[name] = (uint8_t*)addr - base;
        Hydro::logInfo("RawFunctions: resolved \"%s\" -> exe+0x%zX",
            name.c_str(), (size_t)((uint8_t*)addr - base));
    } else {
        s_offsetCache[name] = FAILED_SENTINEL;
        Hydro::logWarn("RawFunctions: resolve(\"%s\") failed (strategy %d)",
            name.c_str(), (int)descIt->second.strategy);
    }
    saveCache();
    return addr;
}

void resolveAllRegistered() {
    // Snapshot under lock to avoid recursive lock when each resolve() takes
    // s_mutex itself.
    std::vector<std::string> names;
    {
        std::lock_guard<std::mutex> lk(s_mutex);
        names.reserve(s_descriptors.size());
        for (auto& kv : s_descriptors) names.push_back(kv.first);
    }
    for (auto& n : names) {
        (void)resolve(n);
    }
}

void invalidate(const std::string& name) {
    std::lock_guard<std::mutex> lk(s_mutex);
    s_resolved.erase(name);
    s_offsetCache.erase(name);
    saveCache();
}

void clearCache() {
    std::lock_guard<std::mutex> lk(s_mutex);
    s_resolved.clear();
    s_offsetCache.clear();
    s_cacheLoaded = false;
    std::error_code ec;
    std::filesystem::remove(cachePath(), ec);
    Hydro::logInfo("RawFunctions: cache cleared");
}

} // namespace Hydro::Engine::RawFn
