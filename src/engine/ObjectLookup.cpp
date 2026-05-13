#include "ObjectLookup.h"
#include "../EngineAPI.h"
#include "../HydroCore.h"
#include "../SEH.h"
#include "../RawFunctions.h"
#include "Internal.h"
#include "FName.h"
#include "GUObjectArray.h"
#include "Layout.h"
#include "Reflection.h"
#include "ProcessEvent.h"
#include <string>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace Hydro::Engine {

// Cross-module externs - defined in EngineAPI.cpp, will migrate to AssetRegistry module later.
extern void* s_assetRegImpl;
extern void* s_getAssetFunc;
extern void* s_assetRegHelpersCDO;
extern void* s_getByPathFunc;

// Forward decl - defined in EngineAPI.cpp (non-static), will move to AssetRegistry module.
bool discoverAssetRegistry();

// Storage for function pointer globals declared in ObjectLookup.h.
void* s_staticLoadObject       = nullptr;
void* s_realStaticFindObject   = nullptr;
void* s_loadPackage            = nullptr;
void* s_staticFindObject       = nullptr;
void* s_staticFindObjectFast   = nullptr;

// -- StaticLoadObject discovery --------------------------------------------

using StaticLoadObjectFn_t = void*(__fastcall*)(
    void* Class, void* InOuter, const wchar_t* Name, const wchar_t* Filename,
    uint32_t LoadFlags, void* Sandbox, bool bAllowReconciliation, void* InstancingContext);

void* safeCallLoadObject(void* funcAddr, const wchar_t* path, uint32_t flags) {
#ifdef _WIN32
    __try {
        auto fn = (StaticLoadObjectFn_t)funcAddr;
        return fn(nullptr, nullptr, path, nullptr, flags, nullptr, true, nullptr);
    } __except(1) { return nullptr; }
#else
    auto fn = (StaticLoadObjectFn_t)funcAddr;
    return fn(nullptr, nullptr, path, nullptr, flags, nullptr, true, nullptr);
#endif
}

using StaticFindObjectFn_t = void*(__fastcall*)(void*, void*, const wchar_t*, bool);
void* safeCallFindObject(void* funcAddr, const wchar_t* path) {
#ifdef _WIN32
    __try {
        auto fn = (StaticFindObjectFn_t)funcAddr;
        return fn(nullptr, nullptr, path, false);
    } __except(1) { return nullptr; }
#else
    auto fn = (StaticFindObjectFn_t)funcAddr;
    return fn(nullptr, nullptr, path, false);
#endif
}

// Forward-declared here; defined further down after findObjectViaScan is in scope.
static void* findObjectViaScan(const wchar_t* path);

static void* getObjectClassAnchor() {
    static void* s_objectClass = nullptr;
    if (s_objectClass) return s_objectClass;
    if (!s_guObjectArray || !s_fnameConstructor) return nullptr;
    s_objectClass = findObjectViaScan(L"/Script/CoreUObject.Object");
    if (s_objectClass)
        Hydro::logInfo("EngineAPI: Object UClass anchor at %p", s_objectClass);
    return s_objectClass;
}

bool validateStaticFindObjectCandidate(void* fn, std::string* outName) {
    if (outName) *outName = "<null>";
    if (!fn) return false;
    void* a = safeCallFindObject(fn, L"/Script/CoreUObject.Object");
    if (!a) return false;

    void* anchor = getObjectClassAnchor();
    if (anchor) {
        if (outName) *outName = (a == anchor) ? "<MATCH>" : "<wrong-ptr>";
        return a == anchor;
    }

    // Diversity fallback: two different paths must yield two different non-null heap pointers.
    void* b = safeCallFindObject(fn, L"/Script/Engine.Actor");
    if (!b) {
        if (outName) *outName = "<no-2nd>";
        return false;
    }
    bool ok = (a != b);
    auto inMod = [](void* p) {
        return (uint8_t*)p >= s_gm.base && (uint8_t*)p < s_gm.base + s_gm.size;
    };
    if (inMod(a) || inMod(b)) ok = false;
    if (outName) *outName = ok ? "<diversity-pass>" : "<same-or-mod>";
    return ok;
}

bool validateStaticLoadObjectResult(void* anyObj, std::string* outName) {
    if (outName) *outName = "<null>";
    if (!anyObj) return false;
    void* anchor = getObjectClassAnchor();
    if (!anchor) return false;
    if (outName) *outName = "<got-ptr>";
    return anyObj == anchor;
}

bool validateStaticLoadObjectCandidate(void* fn) {
    if (!fn) return false;
    void* ret = safeCallLoadObject(fn, L"/Script/CoreUObject.Object", 0);
    return validateStaticLoadObjectResult(ret, nullptr);
}

bool findStaticLoadObject() {
    // "Failed to find object" sits inside StaticFindObject (in-memory lookup only).
    uint8_t* findStr = findWideString(s_gm.base, s_gm.size, L"Failed to find object");
    if (!findStr) {
        Hydro::logWarn("EngineAPI: 'Failed to find object' string not found");
        return false;
    }

    Hydro::logInfo("EngineAPI: 'Failed to find object' at exe+0x%zX", (size_t)(findStr - s_gm.base));

    for (size_t i = 0; i + 7 < s_gm.size; i++) {
        uint8_t* p = s_gm.base + i;
        if ((p[0] != 0x48 && p[0] != 0x4C) || p[1] != 0x8D) continue;
        uint8_t modrm = p[2];
        if ((modrm & 0xC7) != 0x05 && (modrm & 0xC7) != 0x0D &&
            (modrm & 0xC7) != 0x15 && (modrm & 0xC7) != 0x35) continue;
        int32_t disp = *(int32_t*)(p + 3);
        if (p + 7 + disp != findStr) continue;

        Hydro::logInfo("EngineAPI: LEA ref at exe+0x%zX", i);

        // .pdata lookup is authoritative: the prior CC-padding walkback picked
        // up mid-function bytes as prologues on some UE 5.6 builds and crashed.
        uint8_t* funcStart = funcStartViaPdata(p);
        if (funcStart) {
            s_staticLoadObject = funcStart;
            Hydro::logInfo("EngineAPI: StaticFindObject at exe+0x%zX (.pdata fn start)",
                (size_t)(funcStart - s_gm.base));
        } else {
            Hydro::logWarn("EngineAPI: LEA at exe+0x%zX has no .pdata containing function", i);
        }
    }

    // What we found above is StaticFindObject (find-only).
    if (s_staticLoadObject) {
        s_staticFindObject = s_staticLoadObject;
        Hydro::logInfo("EngineAPI: StaticFindObject confirmed at %p", s_staticFindObject);
    }

    // Now locate the actual StaticLoadObject which calls LoadPackage.
    const wchar_t* loadStrings[] = {
        L"Attempting to find",
        L"LoadPackageInternal",
        L"Can't find file for package",
        L"CreateLinker",
    };

    for (auto* searchStr : loadStrings) {
        uint8_t* loadStr = findWideString(s_gm.base, s_gm.size, searchStr);
        if (!loadStr) continue;

        Hydro::logInfo("EngineAPI: Found loading string at exe+0x%zX", (size_t)(loadStr - s_gm.base));

        uint8_t* leaAddr = findLeaRef(s_gm.base, s_gm.size, loadStr);
        if (!leaAddr) continue;

        uint8_t* funcStart = funcStartViaPdata(leaAddr);
        if (funcStart) {
            s_staticLoadObject = funcStart;
            Hydro::logInfo("EngineAPI: LoadPackage/LoadObject at exe+0x%zX (.pdata fn start)",
                (size_t)(funcStart - s_gm.base));
            return true;
        }
        Hydro::logWarn("EngineAPI: '%ls' LEA at exe+0x%zX has no .pdata containing function",
            searchStr, (size_t)(leaAddr - s_gm.base));
    }

    Hydro::logError("EngineAPI: No loading function found");
    return s_staticFindObject != nullptr;
}

// -- findObjectViaScan (GUObjectArray walk) --------------------------------
//
// Robust alternative to pattern-scanned StaticFindObject: walks GUObjectArray
// directly, matching on FName index + outer chain. UE4SS uses the same approach
// for early-init lookups (StaticFindObject_InternalNoToStringFromNames).
//
// Path syntax: split on '.' and ':' into ordered name parts, e.g.
//   /Script/Engine.Actor              → ["/Script/Engine", "Actor"]
//   /Script/Pkg.Class:Member          → ["/Script/Pkg", "Class", "Member"]
//   /Game/Mods/M/Sphere.Sphere_C      → ["/Game/Mods/M/Sphere", "Sphere_C"]
//
// For each candidate whose own FName matches the last part, walk its outer
// chain comparing each step's FName against the parts in reverse.
// Strict match (exact ordered, no leftover) wins. Fallback: outermost package
// name compared as a decoded string (resilient to per-fork FName index variation).

static std::unordered_map<std::wstring, void*> s_findObjectCache;

static void* findObjectViaScan(const wchar_t* path) {
    if (!s_guObjectArray || !path || !s_fnameConstructor) return nullptr;

    std::wstring p(path);
    std::vector<std::wstring> parts;
    size_t start = 0;
    for (size_t i = 0; i <= p.size(); i++) {
        if (i == p.size() || p[i] == L'.' || p[i] == L':') {
            if (i > start) parts.emplace_back(p.substr(start, i - start));
            start = i + 1;
        }
    }
    if (parts.empty()) return nullptr;

    std::vector<uint32_t> partIdx;
    partIdx.reserve(parts.size());
    for (auto& s : parts) {
        FName8 fn = {};
        if (!safeConstructFName(&fn, s.c_str()) || !fn.comparisonIndex) return nullptr;
        partIdx.push_back(fn.comparisonIndex);
    }
    uint32_t targetNameIdx = partIdx.back();

    void** chunkTable = nullptr;
    int32_t count = 0;
    if (!safeReadPtr((uint8_t*)s_guObjectArray + FARRAY_OBJECTS, (void**)&chunkTable)) return nullptr;
    if (!safeReadInt32((uint8_t*)s_guObjectArray + FARRAY_NUM_ELEMS, &count)) return nullptr;
    if (!chunkTable || count <= 0) return nullptr;

    void* strictMatch   = nullptr;
    void* fallbackMatch = nullptr;

    for (int32_t i = 0; i < count; i++) {
        int chunkIdx    = i / CHUNK_SIZE;
        int withinChunk = i % CHUNK_SIZE;
        void* chunk = nullptr;
        if (!safeReadPtr((uint8_t*)&chunkTable[chunkIdx], &chunk) || !chunk) continue;
        uint8_t* item = (uint8_t*)chunk + (withinChunk * FUOBJ_SIZE);
        void* obj = nullptr;
        if (!safeReadPtr(item + FUOBJ_OBJECT, &obj) || !obj) continue;

        uint32_t selfIdx = 0;
        if (!safeReadInt32((uint8_t*)obj + UOBJ_NAME, (int32_t*)&selfIdx)) continue;
        if (selfIdx != targetNameIdx) continue;

        // Tier 1: strict outer-chain match - every part in exact order.
        void* cur = obj;
        bool strict = true;
        for (int k = (int)partIdx.size() - 1; k >= 0; k--) {
            if (!cur) { strict = false; break; }
            uint32_t curName = 0;
            if (!safeReadInt32((uint8_t*)cur + UOBJ_NAME, (int32_t*)&curName)) { strict = false; break; }
            if (curName != partIdx[k]) { strict = false; break; }
            void* outer = nullptr;
            if (!safeReadPtr((uint8_t*)cur + UOBJ_OUTER, &outer)) { strict = false; break; }
            cur = outer;
        }
        if (strict && cur != nullptr) strict = false;
        if (strict) {
            strictMatch = obj;
            break;
        }

        // Tier 2: forgiving "Pkg.Leaf" form. Walk to outermost, compare the
        // package name as a decoded string (case-insensitive). FName index
        // parity for package names isn't guaranteed across forked engines -
        // the same string may yield different ComparisonIndex values from
        // FNAME_Add vs. the actual stored index on the live UPackage.
        if (!fallbackMatch && partIdx.size() >= 2) {
            void* outerCur  = obj;
            void* outermost = obj;
            for (int hops = 0; hops < 32; hops++) {
                void* next = nullptr;
                if (!safeReadPtr((uint8_t*)outerCur + UOBJ_OUTER, &next) || !next) break;
                outermost = next;
                outerCur  = next;
            }
            std::string outermostStr = getObjectName(outermost);
            std::string firstPartStr;
            firstPartStr.reserve(parts.front().size());
            for (wchar_t c : parts.front()) firstPartStr.push_back((char)(c & 0x7F));
            bool match = outermostStr.size() == firstPartStr.size();
            for (size_t k = 0; match && k < outermostStr.size(); k++) {
                unsigned char a = (unsigned char)outermostStr[k];
                unsigned char b = (unsigned char)firstPartStr[k];
                if (a >= 'A' && a <= 'Z') a += 32;
                if (b >= 'A' && b <= 'Z') b += 32;
                if (a != b) match = false;
            }
            if (match) {
                fallbackMatch = obj;
                // Keep scanning - a strict match may appear later.
            }
        }
    }

    if (strictMatch)   return strictMatch;
    if (fallbackMatch) return fallbackMatch;

    // -- Tier-3 diagnostics - fired only on miss from tiers 1+2. --
    //
    // For each object whose leaf string name matches case-insensitively, dump
    // its outer chain with FName indices and decoded strings. Highlights any
    // mismatch between our computed partIdx[k] and the actual stored index
    // at chain position k. One-shot per unique path to avoid log spam.
    static std::unordered_map<std::wstring, bool> s_diagDumped;
    if (s_diagDumped.find(p) == s_diagDumped.end()) {
        s_diagDumped[p] = true;

        std::string narrowPath;
        for (wchar_t c : p) narrowPath.push_back((char)(c & 0x7F));
        std::string narrowLeaf;
        for (wchar_t c : parts.back()) narrowLeaf.push_back((char)(c & 0x7F));

        Hydro::logInfo("findObject DIAG: tiers 1+2 missed for '%s'", narrowPath.c_str());
        Hydro::logInfo("findObject DIAG: parts=%zu, our computed FName indices:", parts.size());
        for (size_t k = 0; k < parts.size(); k++) {
            std::string narrowPart;
            for (wchar_t c : parts[k]) narrowPart.push_back((char)(c & 0x7F));
            Hydro::logInfo("  [%zu] '%s' -> our FNAME_Add idx=%u",
                (size_t)k, narrowPart.c_str(), partIdx[k]);
        }

        auto iequalNarrow0 = [](const std::string& a, const std::string& b) {
            if (a.size() != b.size()) return false;
            for (size_t i = 0; i < a.size(); i++) {
                unsigned char ca = (unsigned char)a[i];
                unsigned char cb = (unsigned char)b[i];
                if (ca >= 'A' && ca <= 'Z') ca += 32;
                if (cb >= 'A' && cb <= 'Z') cb += 32;
                if (ca != cb) return false;
            }
            return true;
        };

        int diagFound = 0;
        for (int32_t i2 = 0; i2 < count && diagFound < 6; i2++) {
            int chunkIdx2    = i2 / CHUNK_SIZE;
            int withinChunk2 = i2 % CHUNK_SIZE;
            void* chunk2 = nullptr;
            if (!safeReadPtr((uint8_t*)&chunkTable[chunkIdx2], &chunk2) || !chunk2) continue;
            uint8_t* item2 = (uint8_t*)chunk2 + (withinChunk2 * FUOBJ_SIZE);
            void* obj2 = nullptr;
            if (!safeReadPtr(item2 + FUOBJ_OBJECT, &obj2) || !obj2) continue;

            std::string leafName = getObjectName(obj2);
            if (!iequalNarrow0(leafName, narrowLeaf)) continue;

            diagFound++;
            Hydro::logInfo("  candidate obj=%p '%s'", obj2, leafName.c_str());
            void* cur2 = obj2;
            for (int hops = 0; hops < (int)parts.size() + 2 && cur2; hops++) {
                uint32_t curIdx = 0;
                safeReadInt32((uint8_t*)cur2 + UOBJ_NAME, (int32_t*)&curIdx);
                std::string curName = getObjectName(cur2);
                Hydro::logInfo("    chain[%d] obj=%p name='%s' idx=%u",
                    hops, cur2, curName.c_str(), curIdx);
                void* next2 = nullptr;
                if (!safeReadPtr((uint8_t*)cur2 + UOBJ_OUTER, &next2) || !next2) break;
                cur2 = next2;
            }
        }
        if (diagFound == 0) {
            static bool s_groundTruthDumped = false;
            if (!s_groundTruthDumped) {
                s_groundTruthDumped = true;
                Hydro::logInfo("findObject DIAG: ground-truth dump of first 10 GUObjectArray entries:");
                Hydro::logInfo("  guObjectArray=%p  count=%d  CHUNK_SIZE=%d  FUOBJ_SIZE=%d",
                    s_guObjectArray, count, (int)CHUNK_SIZE, (int)FUOBJ_SIZE);
                for (int32_t gi = 0; gi < 10 && gi < count; gi++) {
                    int chunkIdx3    = gi / CHUNK_SIZE;
                    int withinChunk3 = gi % CHUNK_SIZE;
                    void* chunk3 = nullptr;
                    if (!safeReadPtr((uint8_t*)&chunkTable[chunkIdx3], &chunk3) || !chunk3) {
                        Hydro::logInfo("  [%d] chunk read failed", gi);
                        continue;
                    }
                    uint8_t* item3 = (uint8_t*)chunk3 + (withinChunk3 * FUOBJ_SIZE);
                    void* obj3 = nullptr;
                    if (!safeReadPtr(item3 + FUOBJ_OBJECT, &obj3) || !obj3) {
                        Hydro::logInfo("  [%d] obj=null", gi);
                        continue;
                    }
                    uint32_t selfIdx3 = 0;
                    safeReadInt32((uint8_t*)obj3 + UOBJ_NAME, (int32_t*)&selfIdx3);
                    std::string selfStr = getObjectName(obj3);
                    void* clsPtr = nullptr;
                    safeReadPtr((uint8_t*)obj3 + UOBJ_CLASS, &clsPtr);
                    std::string clsStr = clsPtr ? getObjectName(clsPtr) : std::string();
                    void* outerPtr = nullptr;
                    safeReadPtr((uint8_t*)obj3 + UOBJ_OUTER, &outerPtr);
                    std::string outerStr = outerPtr ? getObjectName(outerPtr) : std::string();
                    Hydro::logInfo("  [%d] obj=%p name='%s' idx=%u class=%p('%s') outer=%p('%s')",
                        gi, obj3, selfStr.c_str(), selfIdx3,
                        clsPtr, clsStr.c_str(), outerPtr, outerStr.c_str());
                }
            }
            Hydro::logInfo("findObject DIAG: NO objects whose leaf name matches '%s' (case-insensitive).",
                narrowLeaf.c_str());
        }
    }

    // Tier 3 - UE4SS-style full-path string compare.
    // Mirrors StaticFindObject_InternalSlow (UObjectGlobals.cpp:111): walk
    // every UObject whose self-name matches the leaf, build its full outer-chain
    // path string, and compare case-insensitively. Handles forks where the CDO's
    // outer chain shape differs from stock UE.
    auto iequalNarrow = [](const std::string& a, const std::string& b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); i++) {
            unsigned char ca = (unsigned char)a[i];
            unsigned char cb = (unsigned char)b[i];
            if (ca >= 'A' && ca <= 'Z') ca += 32;
            if (cb >= 'A' && cb <= 'Z') cb += 32;
            if (ca != cb) return false;
        }
        return true;
    };
    auto wideToNarrow = [](const std::wstring& w) {
        std::string out;
        out.reserve(w.size());
        for (wchar_t c : w) out.push_back((char)(c & 0x7F));
        return out;
    };
    std::string targetNarrow = wideToNarrow(p);
    std::string leafNarrow   = wideToNarrow(parts.back());

    for (int32_t i = 0; i < count; i++) {
        int chunkIdx    = i / CHUNK_SIZE;
        int withinChunk = i % CHUNK_SIZE;
        void* chunk = nullptr;
        if (!safeReadPtr((uint8_t*)&chunkTable[chunkIdx], &chunk) || !chunk) continue;
        uint8_t* item = (uint8_t*)chunk + (withinChunk * FUOBJ_SIZE);
        void* obj = nullptr;
        if (!safeReadPtr(item + FUOBJ_OBJECT, &obj) || !obj) continue;

        std::string objLeaf = getObjectName(obj);
        if (objLeaf.empty()) continue;
        if (!iequalNarrow(objLeaf, leafNarrow)) continue;

        std::string built = getObjectPath(obj);
        if (built.empty()) continue;
        if (iequalNarrow(built, targetNarrow)) return obj;
    }

    return nullptr;
}

// -- findObject ------------------------------------------------------------

void* findObject(const wchar_t* path) {
    if (!path) return nullptr;

    // Validate cached entry with one read so a GC'd object doesn't stick.
    // /Script/ classes are root-held so rare in practice, but /Game/ assets
    // can be collected.
    std::wstring key(path);
    auto it = s_findObjectCache.find(key);
    if (it != s_findObjectCache.end() && it->second) {
        void* cls = nullptr;
        if (safeReadPtr((uint8_t*)it->second + UOBJ_CLASS, &cls) && cls)
            return it->second;
        s_findObjectCache.erase(it);
    }

    // Primary: GUObjectArray walk. Depends only on UOBJ_NAME/UOBJ_OUTER/GUObjectArray
    // layout - all stable and validated at bootstrap.
    if (void* result = findObjectViaScan(path)) {
        s_findObjectCache[key] = result;
        return result;
    }

    // Fallback: pattern-scanned StaticFindObject, only used on hosts where it's
    // been behaviorally validated to return correct objects.
    if (s_realStaticFindObject) {
        if (void* result = safeCallFindObject(s_realStaticFindObject, path)) {
            s_findObjectCache[key] = result;
            return result;
        }
    }

    return nullptr;
}

// -- loadAsset -------------------------------------------------------------
//
// Two-tier dispatch:
//   Tier 0 (UE 5.6+ / IoStore): StaticLoadObject → FPackageStore hash lookup.
//     AR is not consulted on this path; pakchunk auto-mount registers our
//     PackageIds in FPackageStore.MountedContainers directly.
//   Tier 1 (legacy fallback): AssetRegistry::GetAsset via ProcessEvent.
//     Same dispatch path BPModLoaderMod uses. Required on older engines where
//     the pak-mount→AR broadcast wiring is present.

// Forward decl - body follows below.
void* loadObject(const wchar_t* path);

void* loadAsset(const wchar_t* assetPath) {
    if (!assetPath) return nullptr;

    // Tier 0: try StaticLoadObject first. On UE 5.6 IoStore builds this is the
    // only path that works - AR has no entry for newly-mounted content until
    // explicitly scanned, and GetAsset returns null even for loadable packages.
    {
        void* obj = loadObject(assetPath);
        if (obj) return obj;
    }

    if (!s_fnameConstructor || !s_processEvent) return nullptr;
    if (!s_assetRegHelpersCDO || !s_getAssetFunc) {
        // AR CDOs aren't in GUObjectArray during HydroCore bootstrap. Retry once.
        // This costs ~1s on a miss (full GUObjectArray walk) but happens at most
        // once - after the first success all state is cached.
        discoverAssetRegistry();
        if (!s_assetRegHelpersCDO || !s_getAssetFunc) {
            Hydro::logError("EngineAPI: AssetRegistry not initialized");
            return nullptr;
        }
        Hydro::logInfo("EngineAPI: AssetRegistry deferred-init succeeded on loadAsset");
    }

    std::wstring fullPath(assetPath);
    std::wstring packageName = fullPath;
    std::wstring assetName;
    auto dotPos = fullPath.rfind(L'.');
    if (dotPos != std::wstring::npos) {
        packageName = fullPath.substr(0, dotPos);
        assetName   = fullPath.substr(dotPos + 1);
    }

    FName8 pkgFName = {}, assetFName = {};
    if (!safeConstructFName(&pkgFName, packageName.c_str())) {
        Hydro::logError("EngineAPI: Failed to construct FName for package");
        return nullptr;
    }
    if (!assetName.empty())
        safeConstructFName(&assetFName, assetName.c_str());

    Hydro::logInfo("EngineAPI: Loading '%ls' (pkg={%u,%u} asset={%u,%u})",
        assetPath, pkgFName.comparisonIndex, pkgFName.number,
        assetFName.comparisonIndex, assetFName.number);

    uint16_t gaParmsSize = getUFunctionParmsSize(s_getAssetFunc);
    uint16_t gaRetOffset = getUFunctionRetOffset(s_getAssetFunc);

    // Resolve field offsets from the FAssetData UScriptStruct at runtime rather
    // than hardcoding them.
    int32_t pkgNameOffset = -1, assetNameOffset = -1;
    {
        void* firstProp = nullptr;
        safeReadPtr((uint8_t*)s_getAssetFunc + UFUNC_CHILD_PROPS, &firstProp);
        if (firstProp) {
            // Use findObject (GUObjectArray walk) - the legacy safeCallLoadObject
            // path crashes on builds where SFO cached pointer is stale.
            void* assetDataStruct = findObject(L"/Script/CoreUObject.AssetData");
            if (!assetDataStruct)
                assetDataStruct = findObject(L"/Script/CoreUObject.ScriptStruct'/Script/CoreUObject.AssetData'");

            if (assetDataStruct) {
                Hydro::logInfo("EngineAPI: FAssetData UScriptStruct at %p", assetDataStruct);
                void* prop = getChildProperties(assetDataStruct);
                int fieldIdx = 0;
                while (prop && fieldIdx < 15) {
                    int32_t offset  = getPropertyOffset(prop);
                    int32_t size    = getPropertyElementSize(prop);
                    uint32_t nameIdx = 0;
                    int nmOff = (s_layout.fieldNameOffset >= 0) ? s_layout.fieldNameOffset : FFIELD_NAME;
                    safeReadInt32((uint8_t*)prop + nmOff, (int32_t*)&nameIdx);
                    Hydro::logInfo("EngineAPI:   Field[%d]: '%s' offset=0x%X size=%d",
                        fieldIdx, getNameString(nameIdx).c_str(), offset, size);
                    fieldIdx++;
                    prop = getNextProperty(prop);
                }
            } else {
                Hydro::logWarn("EngineAPI: Could not find FAssetData UScriptStruct");
            }
        }
    }

    if (pkgNameOffset  < 0) pkgNameOffset  = 0x00;
    if (assetNameOffset < 0) assetNameOffset = 0x10;

    // PackagePath = directory part of PackageName (/Game/Mods/X/Y → /Game/Mods/X).
    std::wstring pkgPathStr = packageName;
    auto lastSlash = pkgPathStr.find_last_of(L'/');
    if (lastSlash != std::wstring::npos) pkgPathStr = pkgPathStr.substr(0, lastSlash);
    FName8 pkgPathFName = {};
    safeConstructFName(&pkgPathFName, pkgPathStr.c_str());

    // FAssetData layout (UE 5.x):
    //   +0x00 FName PackageName
    //   +0x08 FName PackagePath    ← required by GetAsset's load path
    //   +0x10 FName AssetName
    //   +0x18 FName AssetClass     ← left zero; optional
    uint8_t params[1024] = {};
    memcpy(params + pkgNameOffset,  &pkgFName,     8);
    memcpy(params + 0x08,           &pkgPathFName, 8);
    memcpy(params + assetNameOffset, &assetFName,  8);

    if (!callProcessEvent(s_assetRegHelpersCDO, s_getAssetFunc, params)) {
        Hydro::logError("EngineAPI: GetAsset ProcessEvent failed");
        return nullptr;
    }

    Hydro::logInfo("EngineAPI: GetAsset post-call (ParmsSize=%u, RetOff=%u):", gaParmsSize, gaRetOffset);
    for (int row = 0; row < 48; row += 16) {
        uint32_t* pp = (uint32_t*)(params + row);
        Hydro::logInfo("  +0x%02X: %08X %08X %08X %08X", row, pp[0], pp[1], pp[2], pp[3]);
    }

    void* loadedAsset = *(void**)(params + gaRetOffset);
    if (loadedAsset) {
        Hydro::logInfo("EngineAPI: Asset loaded at %p!", loadedAsset);
        return loadedAsset;
    }

    // GetAsset returned null. Trigger an explicit AR scan of the package
    // directory and retry once.
    Hydro::logInfo("EngineAPI: GetAsset null - triggering AssetRegistry scan + retry");
    {
        std::wstring scanPath = packageName;
        auto slash = scanPath.find_last_of(L'/');
        if (slash != std::wstring::npos) scanPath = scanPath.substr(0, slash);
        if (scanPath.empty()) scanPath = L"/Game";
        scanAssetRegistryPaths(scanPath.c_str(), true);

        memset(params, 0, sizeof(params));
        memcpy(params + pkgNameOffset,  &pkgFName,     8);
        memcpy(params + 0x08,           &pkgPathFName, 8);
        memcpy(params + assetNameOffset, &assetFName,  8);
        if (callProcessEvent(s_assetRegHelpersCDO, s_getAssetFunc, params)) {
            loadedAsset = *(void**)(params + gaRetOffset);
            if (loadedAsset) {
                Hydro::logInfo("EngineAPI: Asset loaded at %p after scan!", loadedAsset);
                return loadedAsset;
            }
        }
        Hydro::logInfo("EngineAPI: GetAsset still null after scan. Package '%ls' in memory: %p",
                       packageName.c_str(), findObject(packageName.c_str()));

        // ScanFilesSynchronous fallback: try filesystem paths in the engine's
        // virtual-root format. Useful when ScanPathsSynchronous can't see the
        // pak because the mount-point broadcast never fired.
        {
            std::wstring pkgRel = packageName;
            const wchar_t kGamePrefix[] = L"/Game/";
            if (pkgRel.compare(0, 6, kGamePrefix) == 0) pkgRel = pkgRel.substr(6);

            // Try the host project's content path and a generic fallback form.
            const wchar_t* projectForms[] = { L"Game", L"Content" };
            for (const wchar_t* projForm : projectForms) {
                std::wstring fsPath = std::wstring(L"../../../") + projForm +
                                       L"/Content/" + pkgRel + L".uasset";
                Hydro::logInfo("EngineAPI: trying ScanFilesSynchronous with '%ls'", fsPath.c_str());
                bool scanOk = scanAssetRegistryFiles(fsPath.c_str(), true);
                if (!scanOk) {
                    Hydro::logInfo("EngineAPI:   ScanFilesSynchronous dispatch failed for that candidate");
                    continue;
                }

                memset(params, 0, sizeof(params));
                memcpy(params + pkgNameOffset,  &pkgFName,     8);
                memcpy(params + 0x08,           &pkgPathFName, 8);
                memcpy(params + assetNameOffset, &assetFName,  8);
                if (callProcessEvent(s_assetRegHelpersCDO, s_getAssetFunc, params)) {
                    loadedAsset = *(void**)(params + gaRetOffset);
                    if (loadedAsset) {
                        Hydro::logInfo("EngineAPI: Asset loaded at %p after ScanFilesSynchronous!", loadedAsset);
                        return loadedAsset;
                    }
                }
                Hydro::logInfo("EngineAPI:   GetAsset still null after ScanFilesSynchronous('%ls')", projForm);
            }
            Hydro::logInfo("EngineAPI: All ScanFilesSynchronous candidates exhausted");
        }

        // AR-state diagnostic: walk GUObjectArray for any object under the
        // same mod directory prefix. Disambiguates whether the pak content
        // made it into memory at all vs. AR having no metadata for it.
        static bool s_arDiagDumped = false;
        if (!s_arDiagDumped) {
            s_arDiagDumped = true;

            std::wstring prefix = packageName;
            auto ls2 = prefix.find_last_of(L'/');
            if (ls2 != std::wstring::npos) prefix = prefix.substr(0, ls2);
            std::string prefixNarrow;
            for (wchar_t c : prefix) {
                char ch = (char)(c & 0x7F);
                if (ch >= 'A' && ch <= 'Z') ch += 32;
                prefixNarrow.push_back(ch);
            }
            Hydro::logInfo("EngineAPI: AR-state diagnostic - searching for '%s' in GUObjectArray",
                           prefixNarrow.c_str());

            void** ct = nullptr;
            int32_t ct_count = 0;
            safeReadPtr((uint8_t*)s_guObjectArray + FARRAY_OBJECTS, (void**)&ct);
            safeReadInt32((uint8_t*)s_guObjectArray + FARRAY_NUM_ELEMS, &ct_count);

            int matches = 0;
            const int kMaxDump = 30;
            if (ct && ct_count > 0) {
                for (int32_t i = 0; i < ct_count && matches < kMaxDump; i++) {
                    int ci  = i / CHUNK_SIZE;
                    int wi  = i % CHUNK_SIZE;
                    void* ch2 = nullptr;
                    if (!safeReadPtr((uint8_t*)&ct[ci], &ch2) || !ch2) continue;
                    uint8_t* it2 = (uint8_t*)ch2 + (wi * FUOBJ_SIZE);
                    void* obj2 = nullptr;
                    if (!safeReadPtr(it2 + FUOBJ_OBJECT, &obj2) || !obj2) continue;
                    std::string op = getObjectPath(obj2);
                    if (op.empty()) continue;
                    std::string opLower;
                    opLower.reserve(op.size());
                    for (char ch3 : op) {
                        if (ch3 >= 'A' && ch3 <= 'Z') ch3 += 32;
                        opLower.push_back(ch3);
                    }
                    if (opLower.find(prefixNarrow) == std::string::npos) continue;
                    matches++;
                    void* cls2 = getClass(obj2);
                    std::string clsName2 = cls2 ? getObjectName(cls2) : std::string("?");
                    Hydro::logInfo("  match[%d] %p class='%s' path='%s'",
                                   matches, obj2, clsName2.c_str(), op.c_str());
                }
            }
            if (matches == 0)
                Hydro::logInfo("  RESULT: NO objects under '%s' - pak content was not loaded.", prefixNarrow.c_str());
            else
                Hydro::logInfo("  RESULT: %d object(s) found - pak content IS in memory.", matches);

            // UPackage census: total + /Game/Mods vs. /Script vs. other.
            int totalPkg = 0, gameModsPkg = 0, scriptPkg = 0, gamePkg = 0;
            int firstPkgsLogged = 0;
            if (ct && ct_count > 0) {
                for (int32_t i = 0; i < ct_count; i++) {
                    int ci = i / CHUNK_SIZE;
                    int wi = i % CHUNK_SIZE;
                    void* ch3 = nullptr;
                    if (!safeReadPtr((uint8_t*)&ct[ci], &ch3) || !ch3) continue;
                    uint8_t* it3 = (uint8_t*)ch3 + (wi * FUOBJ_SIZE);
                    void* obj3 = nullptr;
                    if (!safeReadPtr(it3 + FUOBJ_OBJECT, &obj3) || !obj3) continue;
                    void* cls3 = getClass(obj3);
                    if (!cls3) continue;
                    if (getObjectName(cls3) != "Package") continue;
                    totalPkg++;
                    std::string p3 = getObjectPath(obj3);
                    if (p3.find("/Game/Mods") != std::string::npos) gameModsPkg++;
                    else if (p3.find("/Script") != std::string::npos) scriptPkg++;
                    else if (p3.find("/Game") != std::string::npos) gamePkg++;
                    if (firstPkgsLogged < 10) {
                        Hydro::logInfo("  UPackage[%d] %s", firstPkgsLogged, p3.c_str());
                        firstPkgsLogged++;
                    }
                }
            }
            Hydro::logInfo("  UPackage census: total=%d, /Game/Mods=%d, /Script=%d, other /Game=%d",
                           totalPkg, gameModsPkg, scriptPkg, gamePkg);
            Hydro::logInfo("EngineAPI: AR-state diagnostic done");
        }
    }

    return nullptr;
}

// -- loadObject ------------------------------------------------------------
//
// Three-tier cascade:
//   1. discovered StaticLoadObject (works on stock UE; may be absent on forks)
//   2. LoadPackage(packagePath) + findObject(fullPath) - independent anchor
//      string ("Attempted to LoadPackage"), findObject is GUObjectArray-based
//   3. StaticFindObject (in-memory only - last-ditch for already-loaded objects)

using LoadPackageFn3 = void*(__fastcall*)(void* InOuter, const wchar_t* PackageName, uint32_t LoadFlags);

static void* safeCallLoadPackage3(void* fn, const wchar_t* pkg, uint32_t flags) {
#ifdef _WIN32
    __try {
        auto f = (LoadPackageFn3)fn;
        return f(nullptr, pkg, flags);
    } __except(1) { return nullptr; }
#else
    auto f = (LoadPackageFn3)fn;
    return f(nullptr, pkg, flags);
#endif
}

void* loadObject(const wchar_t* path) {
    if (!path) return nullptr;

    // Tier 1: StaticLoadObject with multiple flag combinations.
    if (s_staticLoadObject) {
        uint32_t flagSets[] = { 0x0A, 0x00, 0x02, 0x2000 };
        for (auto flags : flagSets) {
            void* result = safeCallLoadObject(s_staticLoadObject, path, flags);
            if (result) {
                if (flags != 0) Hydro::logInfo("EngineAPI: Loaded via StaticLoadObject with flags 0x%X", flags);
                return result;
            }
        }
    }

    // Tier 2: LoadPackage(packagePath) + findObject(fullPath).
    // Split at the last '.' - before is the package path, full string is the object path.
    if (s_loadPackage) {
        std::wstring full(path);
        auto dot = full.rfind(L'.');
        std::wstring pkg = (dot == std::wstring::npos) ? full : full.substr(0, dot);
        uint32_t flagSets[] = { 0x00, 0x0A, 0x2000 };
        for (auto flags : flagSets) {
            void* loaded = safeCallLoadPackage3(s_loadPackage, pkg.c_str(), flags);
            (void)loaded; // return value unreliable; check via findObject
            void* obj = findObject(path);
            if (obj) {
                Hydro::logInfo("EngineAPI: Loaded via LoadPackage('%ls', flags=0x%X) + findObject",
                               pkg.c_str(), flags);
                return obj;
            }
        }
    }

    // Tier 3: StaticFindObject (in-memory only).
    if (s_staticFindObject) {
        void* result = safeCallLoadObject(s_staticFindObject, path, 0);
        if (result) return result;
    }

    return nullptr;
}

// -- seedAndResolveRawFunctions --------------------------------------------
//
// Registers known-good engine functions with the RawFn registry. Resolves
// them once per launch; results are cached in hydro_raw_funcs.bin next to
// the DLL and keyed by host module size.
//
// ProcessInternal: Tier 1 via ExecuteUbergraph Func@0xD8 - identical address
//   that HydroEvents.cpp's discoverProcessInternal returns.
// StaticLoadObject: Tier 3 via L"Failed to find object" LEA walk - same anchor
//   findStaticLoadObject uses internally. Note: L"StaticLoadObject" on UE 5.5
//   appears only in a debug-only callsite outside the SLO body, so we avoid
//   it as an anchor.

void seedAndResolveRawFunctions() {
    using namespace ::Hydro::Engine::RawFn;
    registerFn("ProcessInternal", { Strategy::UFuncImpl,
        L"/Script/CoreUObject.Object:ExecuteUbergraph", 0 });
    registerFn("StaticLoadObject", { Strategy::StringRefAnchor,
        L"Failed to find object", 0 });
    resolveAllRegistered();
}

// -- fastScan --------------------------------------------------------------
//
// Core GUObjectArray scanner for findFirstOf/findAllOf.
//
// Performance notes:
//   Layer 1 - resolve className → UClass* once, then compare obj->class
//             to that pointer instead of reading each obj->class->name.
//   Layer 2 - raw reads in the hot loop under one SEH block, not per-read.
//   Layer 3 - cache last found instance; warm call validates with one read.
// Combined: cold ~10ms, warm ~nanoseconds vs. ~1s previously.

struct FindCacheEntry {
    uint32_t targetNameIdx = 0;
    void* uclass       = nullptr;
    void* lastInstance = nullptr;
};
static std::unordered_map<std::wstring, FindCacheEntry> s_findCache;

static bool validateCachedInstance(void* instance, void* expectedClass) {
    if (!instance || !expectedClass) return false;
    void* cls = nullptr;
    if (!safeReadPtr((uint8_t*)instance + UOBJ_CLASS, &cls)) return false;
    return cls == expectedClass;
}

static int fastScan(uint32_t targetNameIdx, void* targetClass,
                    void** outFirst, void** outAll, int maxResults,
                    void** outResolvedClass) {
    if (!s_guObjectArray) return 0;
    int found = 0;
#ifdef _WIN32
    __try {
#endif
        void** chunkTable = *(void***)((uint8_t*)s_guObjectArray + FARRAY_OBJECTS);
        int32_t count = *(int32_t*)((uint8_t*)s_guObjectArray + FARRAY_NUM_ELEMS);
        if (!chunkTable || count <= 0) return 0;

        for (int32_t i = 0; i < count; i++) {
            int chunkIdx    = i / CHUNK_SIZE;
            int withinChunk = i % CHUNK_SIZE;
            void* chunk = chunkTable[chunkIdx];
            if (!chunk) continue;

            uint8_t* item = (uint8_t*)chunk + (withinChunk * FUOBJ_SIZE);
            void* obj = *(void**)(item + FUOBJ_OBJECT);
            if (!obj) continue;

            void* cls = *(void**)((uint8_t*)obj + UOBJ_CLASS);
            if (!cls) continue;

            bool match;
            if (targetClass) {
                match = (cls == targetClass);
            } else {
                uint32_t classNameIdx = *(uint32_t*)((uint8_t*)cls + UOBJ_NAME);
                match = (classNameIdx == targetNameIdx);
            }
            if (!match) continue;

            if (found == 0 && outResolvedClass && !*outResolvedClass)
                *outResolvedClass = cls;

            if (outFirst && found == 0) {
                *outFirst = obj;
                if (!outAll) return 1;  // findFirstOf - short-circuit
            }
            if (outAll && found < maxResults) outAll[found] = obj;
            found++;
            if (outAll && found >= maxResults) break;
        }
#ifdef _WIN32
    } __except (1) {
        // Fault mid-scan (rare: chunk torn by concurrent GC). Return partial results.
    }
#endif
    return found;
}

// -- Actor subclass helpers ------------------------------------------------

static void* s_actorClassPtr            = nullptr;
static void* s_playerControllerClassPtr = nullptr;

static bool isPlayerControllerSubclass(void* uclass) {  // TU-local; not in header
    if (!uclass) return false;
    if (!s_playerControllerClassPtr) {
        s_playerControllerClassPtr = findObject(L"/Script/Engine.PlayerController");
        if (!s_playerControllerClassPtr) return false;
    }
    int superOff = (s_superOffset > 0) ? s_superOffset : 0x40;
    void* cur = uclass;
    for (int depth = 0; depth < 64 && cur; depth++) {
        if (cur == s_playerControllerClassPtr) return true;
        void* super = nullptr;
        if (!safeReadPtr((uint8_t*)cur + superOff, &super)) return false;
        if (super == cur) return false;
        cur = super;
    }
    return false;
}

// Find the first non-CDO APlayerController. PlayerController is the strongest
// generic gameplay-world anchor: always created by GameMode when a player
// connects, class chain reflection-resolved (no string anchors in shipping),
// never in a transient or debug context.
void* findPlayerController() {
    if (!s_guObjectArray) return nullptr;
    int32_t count = getObjectCount();
    for (int32_t i = 0; i < count; i++) {
        void* obj = getObjectAt(i);
        if (!obj) continue;
        uint32_t flags = 0;
        if (!safeReadInt32((uint8_t*)obj + UOBJ_FLAGS, (int32_t*)&flags)) continue;
        if (flags & 0x10) continue;  // RF_ClassDefaultObject
        void* cls = getObjClass(obj);
        if (!cls) continue;
        if (!isPlayerControllerSubclass(cls)) continue;
        return obj;
    }
    return nullptr;
}

bool isActorSubclass(void* uclass) {
    if (!uclass) return false;
    if (!s_actorClassPtr) {
        s_actorClassPtr = findObject(L"/Script/Engine.Actor");
        if (!s_actorClassPtr) return false;
    }
    int superOff = (s_superOffset > 0) ? s_superOffset : 0x40;
    void* cur = uclass;
    for (int depth = 0; depth < 64 && cur; depth++) {
        if (cur == s_actorClassPtr) return true;
        void* super = nullptr;
        if (!safeReadPtr((uint8_t*)cur + superOff, &super)) return false;
        if (super == cur) return false;
        cur = super;
    }
    return false;
}

// Walk GUObjectArray for a non-CDO actor instance suitable as a UFunction
// WorldContext. UE's GetWorldFromContextObject expects an object that's in
// a world; UWorld* itself fails on some UE 5.5 builds, and CDOs always fail.
void* findInWorldActor() {
    if (!s_guObjectArray) return nullptr;
    int32_t count = getObjectCount();
    for (int32_t i = 0; i < count; i++) {
        void* obj = getObjectAt(i);
        if (!obj) continue;
        uint32_t flags = 0;
        if (!safeReadInt32((uint8_t*)obj + UOBJ_FLAGS, (int32_t*)&flags)) continue;
        if (flags & 0x10) continue;  // RF_ClassDefaultObject
        void* cls = getObjClass(obj);
        if (!cls || !isActorSubclass(cls)) continue;
        return obj;
    }
    return nullptr;
}

// -- findFirstOf / findAllOf -----------------------------------------------

void* findFirstOf(const wchar_t* className) {
    if (!s_guObjectArray || !s_fnameConstructor || !className) return nullptr;

    std::wstring key(className);
    auto& entry = s_findCache[key];

    // Warm path: validate cached instance with one read.
    if (entry.lastInstance &&
        validateCachedInstance(entry.lastInstance, entry.uclass))
        return entry.lastInstance;

    if (entry.targetNameIdx == 0) {
        FName8 targetName = {};
        if (!safeConstructFName(&targetName, className)) return nullptr;
        entry.targetNameIdx = targetName.comparisonIndex;
    }

    // Actor fast path: once UClass* is resolved and it descends from AActor,
    // use UGameplayStatics::GetAllActorsOfClass - O(instances-in-world) via
    // UE's own hash-backed iteration rather than O(all UObjects).
    if (entry.uclass && isActorSubclass(entry.uclass)) {
        void* buf[1] = {};
        if (getAllActorsOfClass(entry.uclass, buf, 1) > 0) {
            entry.lastInstance = buf[0];
            return buf[0];
        }
        return nullptr;
    }

    void* first   = nullptr;
    void* resolved = entry.uclass;
    fastScan(entry.targetNameIdx, entry.uclass, &first, nullptr, 0, &resolved);
    if (resolved && !entry.uclass) entry.uclass = resolved;
    entry.lastInstance = first;
    return first;
}

int findAllOf(const wchar_t* className, void** outArray, int maxResults) {
    if (!s_guObjectArray || !s_fnameConstructor || !className || !outArray || maxResults <= 0)
        return 0;

    std::wstring key(className);
    auto& entry = s_findCache[key];

    if (entry.targetNameIdx == 0) {
        FName8 targetName = {};
        if (!safeConstructFName(&targetName, className)) return 0;
        entry.targetNameIdx = targetName.comparisonIndex;
    }

    // Actor fast path - see findFirstOf rationale.
    if (entry.uclass && isActorSubclass(entry.uclass)) {
        int found = getAllActorsOfClass(entry.uclass, outArray, maxResults);
        if (found > 0) entry.lastInstance = outArray[0];
        return found;
    }

    void* resolved = entry.uclass;
    int found = fastScan(entry.targetNameIdx, entry.uclass,
                         nullptr, outArray, maxResults, &resolved);
    if (resolved && !entry.uclass) entry.uclass = resolved;
    if (found > 0) entry.lastInstance = outArray[0];
    return found;
}

// -- readFromPool ----------------------------------------------------------

std::string readFromPool(uint32_t comparisonIndex) {
    if (!s_poolReady || !s_fnamePool) return {};

    char buf[2048];
    bool isWide = false;
    int len = readPoolEntryRaw(comparisonIndex, buf, sizeof(buf) / 2, &isWide);
    if (len <= 0) return {};

    if (isWide) {
        std::string result;
        result.reserve(len);
        const wchar_t* wstr = (const wchar_t*)buf;
        for (int i = 0; i < len; i++) result += (char)(wstr[i] & 0x7F);
        return result;
    }
    return std::string(buf, len);
}

} // namespace Hydro::Engine
