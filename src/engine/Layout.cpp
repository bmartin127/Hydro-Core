#include "Layout.h"
#include "../EngineAPI.h"
#include "../HydroCore.h"
#include "Internal.h"
#include "FName.h"
#include "GUObjectArray.h"

#include <cstdint>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace Hydro::Engine {

PropertyLayoutCache s_layout;
int s_superOffset      = -1;
int s_classFlagsOffset = -1;
int s_enumNamesOffset  = -1;
int s_outerOffset      = -1;

// Walk a candidate property chain with raw probe offsets. Used only by
// discoverPropertyLayout. Returns the count of FName indices in the chain
// that match anything in expectedNames. SEH-guarded so a wrong triple
// producing garbage just scores 0.
static int probePropertyChainScore(void* uclass, int chOff, int nextOff, int nameOff,
                                   const uint32_t* expectedNames, int expectedCount,
                                   int maxWalk = 200) {
    if (!uclass) return 0;
    int hits = 0;
    void* prop = nullptr;
    if (!safeReadPtr((uint8_t*)uclass + chOff, &prop) || !prop) return 0;
    if ((uint8_t*)prop >= s_gm.base && (uint8_t*)prop < s_gm.base + s_gm.size) return 0;
    if ((uintptr_t)prop < 0x1000) return 0;

    int count = 0;
    while (prop && count < maxWalk) {
        uint32_t nameIdx = 0;
        if (!safeReadInt32((uint8_t*)prop + nameOff, (int32_t*)&nameIdx)) break;
        if (nameIdx == 0 || nameIdx > 0x7FFFFFFF) break;
        for (int i = 0; i < expectedCount; i++) {
            if (nameIdx == expectedNames[i]) { hits++; break; }
        }
        void* next = nullptr;
        if (!safeReadPtr((uint8_t*)prop + nextOff, &next)) break;
        if (next == prop) break;
        prop = next;
        count++;
    }
    return hits;
}

// Scan GUObjectArray directly for AActor's UClass. Matches by:
//   obj's class name FName == "Class"  AND  obj's own name FName == "Actor"
// More reliable than StaticFindObject on hosts where /Script/ path resolution
// is unreliable.
static void* findActorClassViaScan() {
    if (!s_guObjectArray) {
        Hydro::logWarn("EngineAPI: findActorClassViaScan: GUObjectArray not ready");
        return nullptr;
    }

    FName8 classFn = {}, actorFn = {};
    if (!safeConstructFName(&classFn, L"Class") || !safeConstructFName(&actorFn, L"Actor")) {
        Hydro::logWarn("EngineAPI: findActorClassViaScan: FName construction failed");
        return nullptr;
    }
    uint32_t classIdx = classFn.comparisonIndex;
    uint32_t actorIdx = actorFn.comparisonIndex;
    Hydro::logInfo("EngineAPI: findActorClassViaScan: class='Class'(%u) target='Actor'(%u)",
                   classIdx, actorIdx);
    if (!classIdx || !actorIdx) return nullptr;

    int32_t count = 0;
    void** chunkTable = nullptr;
    int classMatches = 0;
    int nameMatches  = 0;
    int faults = 0;
    void* result = nullptr;

    if (!safeReadPtr((uint8_t*)s_guObjectArray + FARRAY_OBJECTS, (void**)&chunkTable)) {
        Hydro::logWarn("EngineAPI: findActorClassViaScan: chunk table unreadable");
        return nullptr;
    }
    if (!safeReadInt32((uint8_t*)s_guObjectArray + FARRAY_NUM_ELEMS, &count) || count <= 0) {
        Hydro::logWarn("EngineAPI: findActorClassViaScan: count unreadable");
        return nullptr;
    }
    if (!chunkTable) return nullptr;

    for (int32_t i = 0; i < count; i++) {
        int chunkIdx    = i / CHUNK_SIZE;
        int withinChunk = i % CHUNK_SIZE;

        void* chunk = nullptr;
        if (!safeReadPtr((uint8_t*)&chunkTable[chunkIdx], &chunk) || !chunk) {
            faults++;
            continue;
        }
        uint8_t* item = (uint8_t*)chunk + (withinChunk * FUOBJ_SIZE);
        void* obj = nullptr;
        if (!safeReadPtr(item + FUOBJ_OBJECT, &obj) || !obj) continue;

        void* cls = nullptr;
        if (!safeReadPtr((uint8_t*)obj + UOBJ_CLASS, &cls) || !cls) continue;

        uint32_t selfNameIdx = 0, clsNameIdx = 0;
        if (!safeReadInt32((uint8_t*)obj + UOBJ_NAME, (int32_t*)&selfNameIdx)) continue;
        if (!safeReadInt32((uint8_t*)cls + UOBJ_NAME, (int32_t*)&clsNameIdx)) continue;

        if (clsNameIdx == classIdx) classMatches++;
        if (selfNameIdx == actorIdx) {
            nameMatches++;
            if (nameMatches <= 8) {
                Hydro::logInfo("  Actor-named obj at %p, class=%p clsName=%u",
                               obj, cls, clsNameIdx);
            }
        }

        if (clsNameIdx == classIdx && selfNameIdx == actorIdx) {
            result = obj;
            break;
        }
    }

    Hydro::logInfo("EngineAPI: findActorClassViaScan: scanned=%d classMatches=%d "
                   "nameMatches=%d chunkFaults=%d result=%p",
                   count, classMatches, nameMatches, faults, result);
    return result;
}

// Sweep candidate offsets [start, end) by step; return the first where
// predicate returns true, or -1. Templated to avoid std::function overhead
// on a startup-only path.
template <typename Pred>
static int32_t probeOffset(int start, int end, int step, Pred predicate) {
    for (int off = start; off < end; off += step) {
        if (predicate(off)) return off;
    }
    return -1;
}

bool discoverPropertyLayout() {
    if (s_layout.initialized) return s_layout.succeeded;
    s_layout.initialized = true;

    // Resolve AActor's UClass by direct GUObjectArray scan rather than
    // StaticFindObject - the latter can return the wrong object on some
    // stripped UE 5.1-derived hosts.
    void* actorClass = findActorClassViaScan();
    if (!actorClass) {
        actorClass = findObject(L"/Script/Engine.Actor");
    }
    if (!actorClass) {
        Hydro::logWarn("EngineAPI: Layer 2 probe deferred - AActor UClass not yet in GUObjectArray");
        s_layout.initialized = false;
        return false;
    }

    // -- Stage A: pre-resolve known AActor property name FName indices --
    static const wchar_t* kKnownActorProps[] = {
        L"PrimaryActorTick",
        L"RootComponent",
        L"Tags",
        L"Owner",
        L"Instigator",
        L"Layer",
        L"NetUpdateFrequency",
        L"ReplicatedMovement",
        L"InstanceComponents",
        L"BlueprintCreatedComponents",
        L"AttachmentReplication",
    };
    constexpr int kKnownCount = sizeof(kKnownActorProps) / sizeof(kKnownActorProps[0]);
    uint32_t expectedNames[kKnownCount] = {};
    int expectedReady = 0;
    for (int i = 0; i < kKnownCount; i++) {
        FName8 fn = {};
        if (safeConstructFName(&fn, kKnownActorProps[i]) && fn.comparisonIndex != 0) {
            expectedNames[expectedReady++] = fn.comparisonIndex;
        }
    }
    if (expectedReady < 4) {
        Hydro::logWarn("EngineAPI: Layer 2 probe failed - couldn't resolve AActor property name FNames");
        return false;
    }
    Hydro::logInfo("EngineAPI: Layer 2 expected names (%d resolved):", expectedReady);
    for (int i = 0; i < expectedReady; i++) {
        Hydro::logInfo("  '%ls' = idx %u", kKnownActorProps[i], expectedNames[i]);
    }

    {
        uint32_t selfNameIdx = 0;
        safeReadInt32((uint8_t*)actorClass + UOBJ_NAME, (int32_t*)&selfNameIdx);
        std::string selfName = getNameString(selfNameIdx);
        void* metaCls = nullptr;
        safeReadPtr((uint8_t*)actorClass + UOBJ_CLASS, &metaCls);
        uint32_t metaNameIdx = 0;
        if (metaCls) safeReadInt32((uint8_t*)metaCls + UOBJ_NAME, (int32_t*)&metaNameIdx);
        std::string metaName = metaCls ? getNameString(metaNameIdx) : std::string("<null>");
        Hydro::logInfo("EngineAPI: actorClass identity check: self='%s' (idx=%u) meta=%p '%s' (idx=%u)",
                       selfName.c_str(), selfNameIdx, metaCls, metaName.c_str(), metaNameIdx);
    }

    Hydro::logInfo("EngineAPI: Layer 2 actorClass=%p; full UClass slot dump:", actorClass);
    for (int off = 0x00; off <= 0x180; off += 8) {
        void* p = nullptr;
        if (!safeReadPtr((uint8_t*)actorClass + off, &p)) {
            Hydro::logInfo("  +0x%02X: <unreadable>", off);
            continue;
        }
        uintptr_t v = (uintptr_t)p;
        bool inMod = ((uint8_t*)p >= s_gm.base && (uint8_t*)p < s_gm.base + s_gm.size);
        uint32_t lo = (uint32_t)(v & 0xFFFFFFFFULL);
        uint32_t hi = (uint32_t)(v >> 32);
        bool heapLike = !inMod && hi != 0 && lo != 0 && hi < 0x10000U;
        const char* tag = inMod ? " <module>" : (heapLike ? " <heap>" : "");
        Hydro::logInfo("  +0x%02X: %p%s", off, p, tag);
    }

    // -- Stage B: probe (USTRUCT_CHILD_PROPS, FFIELD_NEXT, FFIELD_NAME) --
    // Walk AActor's child chain with each candidate triple; the combo with
    // the most matches against expectedNames wins. Stock UE 5.5 lands on
    // (0x50, 0x18, 0x20). UE 5.1-derived forks shift these by +8 bytes.
    // Other forks may differ - extend candidates as new hosts surface.
    static const int kChildProps[] = { 0x50, 0x58, 0x48, 0x60, 0x40 };
    static const int kFieldNext[]  = { 0x18, 0x20, 0x28, 0x10, 0x30 };
    static const int kFieldName[]  = { 0x20, 0x28, 0x30, 0x18, 0x38 };

    int bestScore = 0;
    int bestCh = -1, bestNext = -1, bestName = -1;
    for (int ch : kChildProps) {
        void* firstProp = nullptr;
        bool readOk = safeReadPtr((uint8_t*)actorClass + ch, &firstProp);
        if (!readOk || !firstProp) continue;
        if ((uint8_t*)firstProp >= s_gm.base && (uint8_t*)firstProp < s_gm.base + s_gm.size) continue;
        if ((uintptr_t)firstProp < 0x1000) continue;
        Hydro::logInfo("EngineAPI: Layer 2 trying CH=0x%X firstProp=%p", ch, firstProp);

        for (int nm : kFieldName) {
            uint32_t firstIdx = 0;
            if (safeReadInt32((uint8_t*)firstProp + nm, (int32_t*)&firstIdx) && firstIdx != 0) {
                std::string fname = (firstIdx <= 0x7FFFFFFF) ? getNameString(firstIdx) : std::string("<garbage>");
                Hydro::logInfo("  CH=0x%X NAME=0x%X -> firstIdx=%u ('%s')", ch, nm, firstIdx, fname.c_str());
            }
        }

        for (int nx : kFieldNext) {
            for (int nm : kFieldName) {
                if (nx == nm) continue;
                int score = probePropertyChainScore(actorClass, ch, nx, nm,
                                                    expectedNames, expectedReady);
                if (score > bestScore) {
                    bestScore = score;
                    bestCh = ch; bestNext = nx; bestName = nm;
                }
            }
        }
    }

    if (bestScore < 3) {
        Hydro::logWarn("EngineAPI: Layer 2 chain-walk probe FAILED - best score %d "
                       "(needed >= 3). FField/UStruct layout may have shifted "
                       "in an unexpected way; extend candidate lists.", bestScore);
        return false;
    }

    s_layout.childPropsOffset = bestCh;
    s_layout.fieldNextOffset  = bestNext;
    s_layout.fieldNameOffset  = bestName;

    Hydro::logInfo("EngineAPI: Layer 2 chain-walk DISCOVERED - "
                   "CHILD_PROPS=0x%X FF_NEXT=0x%X FF_NAME=0x%X (score=%d/%d)",
                   bestCh, bestNext, bestName, bestScore, expectedReady);
    if (bestCh != USTRUCT_CHILD_PROPS || bestNext != FFIELD_NEXT || bestName != FFIELD_NAME)
        Hydro::logInfo("EngineAPI: chain-walk offsets DIFFER from stock UE 5.5 "
                       "(stock: CH=0x%X NEXT=0x%X NAME=0x%X)",
                       USTRUCT_CHILD_PROPS, FFIELD_NEXT, FFIELD_NAME);

    // -- Stage C: with chain offsets in hand, find anchor properties --
    void* tickProp = findProperty(actorClass, L"PrimaryActorTick");
    void* rootProp = findProperty(actorClass, L"RootComponent");
    void* tagsProp = findProperty(actorClass, L"Tags");
    if (!tickProp) {
        Hydro::logWarn("EngineAPI: Layer 2 stage C failed - PrimaryActorTick not findable "
                       "even with discovered chain-walk offsets");
        return false;
    }

    // -- Stage D: probe FPROP_OFFSET_INTERNAL --
    // Each prop's offset must be 4-byte aligned, < 8KB, > 32 (real AActor
    // property offsets always exceed the UObjectBase + AActor header), and
    // distinct between PrimaryActorTick / RootComponent / Tags. The > 32
    // floor separates this from FPROP_ELEMENT_SIZE - element sizes like 8
    // (TObjectPtr) and 16 (TArray) share the same int32 shape.
    int32_t bestOI = probeOffset(0x30, 0x80, 4, [&](int off) -> bool {
        auto sane = [](int32_t v) { return v > 32 && v < 8192 && (v & 3) == 0; };
        int32_t vTick = 0, vRoot = 0, vTags = 0;
        if (!safeReadInt32((uint8_t*)tickProp + off, &vTick) || !sane(vTick)) return false;
        if (rootProp && (!safeReadInt32((uint8_t*)rootProp + off, &vRoot) || !sane(vRoot) || vRoot == vTick)) return false;
        if (tagsProp && (!safeReadInt32((uint8_t*)tagsProp + off, &vTags) || !sane(vTags) || vTags == vTick)) return false;
        return true;
    });

    // -- Stage E: probe FPROP_ELEMENT_SIZE --
    // RootComponent is TObjectPtr (8 bytes), Tags is TArray<FName> (16 bytes).
    int32_t bestES = probeOffset(0x30, 0x80, 4, [&](int off) -> bool {
        int32_t vTick = 0;
        if (!safeReadInt32((uint8_t*)tickProp + off, &vTick)) return false;
        if (vTick <= 0 || vTick > 1024) return false;
        if (rootProp) {
            int32_t v = 0;
            if (!safeReadInt32((uint8_t*)rootProp + off, &v) || v != 8) return false;
        }
        if (tagsProp) {
            int32_t v = 0;
            if (!safeReadInt32((uint8_t*)tagsProp + off, &v) || v != 16) return false;
        }
        return true;
    });

    // -- Stage F: probe FPROP_FLAGS (uint64) --
    // Real PropertyFlags exhibit a rich, diverse bit pattern in the low 16
    // (multiple CPF bits per property) and differ between anchor properties.
    // Wrong offsets read pointers or adjacent fields that share a sparse,
    // uniform pattern across anchors.
    constexpr uint64_t CPF_Parm_local = 0x0000000000000080ULL;
    auto popcount16 = [](uint64_t v) {
        int n = 0;
        for (int b = 0; b < 16; b++) if (v & (1ULL << b)) n++;
        return n;
    };
    auto validFlags = [](uint64_t v) {
        if (v == 0 || v == ~0ULL) return false;
        if ((v >> 56) != 0) return false;
        if (v & CPF_Parm_local) return false;
        return true;
    };

    Hydro::logInfo("EngineAPI: FLAGS candidate dump:");
    for (int off = 0x30; off < 0x80; off += 4) {
        uint64_t vTick = 0, vRoot = 0, vTags = 0;
        bool tickOk = safeReadU64((uint8_t*)tickProp + off, &vTick);
        bool rootOk = rootProp && safeReadU64((uint8_t*)rootProp + off, &vRoot);
        bool tagsOk = tagsProp && safeReadU64((uint8_t*)tagsProp + off, &vTags);
        Hydro::logInfo("  +0x%02X: Tick=%s%llX Root=%s%llX Tags=%s%llX",
            off,
            tickOk ? "0x" : "<r:", (unsigned long long)vTick,
            rootOk ? "0x" : "<r:", (unsigned long long)vRoot,
            tagsOk ? "0x" : "<r:", (unsigned long long)vTags);
    }

    // PrimaryActorTick and Tags are the most reliable anchors. RootComponent
    // can carry custom CPF bits on game-specific subclasses (bit 56+ set on
    // Root's flags u64 observed on UE 5.6 shipping), so it stays an optional
    // validator rather than a strict gate.
    constexpr uint64_t CPF_Edit_local = 0x0000000000000001ULL;
    int32_t bestFL = probeOffset(0x30, 0x80, 4, [&](int off) -> bool {
        uint64_t vTick = 0, vRoot = 0, vTags = 0;
        if (!safeReadU64((uint8_t*)tickProp + off, &vTick) || !validFlags(vTick)) return false;
        if (!tagsProp) return false;
        if (!safeReadU64((uint8_t*)tagsProp + off, &vTags) || !validFlags(vTags)) return false;
        if (vTick == vTags) return false;
        // CPF_Edit (bit 0) is set on every UPROPERTY-exposed member.
        // This separates the real PropertyFlags offset from a u64 read
        // straddling ElementSize + flag-low bytes (ElementSize 8/16/48
        // doesn't have bit 0 set).
        if (!(vTick & CPF_Edit_local) || !(vTags & CPF_Edit_local)) return false;
        bool rootPass = rootProp && safeReadU64((uint8_t*)rootProp + off, &vRoot)
                        && validFlags(vRoot);
        if (rootPass && (vRoot == vTick || vRoot == vTags)) return false;
        int richness = popcount16(vTick) + popcount16(vTags);
        if (rootPass) richness += popcount16(vRoot);
        if (richness < 4) return false;
        return true;
    });
    if (bestFL >= 0) {
        uint64_t vTick = 0, vRoot = 0, vTags = 0;
        safeReadU64((uint8_t*)tickProp + bestFL, &vTick);
        if (rootProp) safeReadU64((uint8_t*)rootProp + bestFL, &vRoot);
        if (tagsProp) safeReadU64((uint8_t*)tagsProp + bestFL, &vTags);
        int richness = popcount16(vTick) + popcount16(vRoot) + popcount16(vTags);
        Hydro::logInfo("EngineAPI:   FLAGS=0x%X validated (richness=%d) via Tick=0x%llX Root=0x%llX Tags=0x%llX",
                       bestFL, richness,
                       (unsigned long long)vTick, (unsigned long long)vRoot, (unsigned long long)vTags);
    }

    s_layout.offsetInternal = bestOI;
    s_layout.elementSize    = bestES;
    s_layout.flags          = bestFL;
    s_layout.succeeded      = (bestOI >= 0 && bestES >= 0 && bestFL >= 0);

    Hydro::logInfo("EngineAPI: Layer 2 FProperty layout %s - "
                   "OFFSET_INTERNAL=0x%X ELEMENT_SIZE=0x%X FLAGS=0x%X",
                   s_layout.succeeded ? "DISCOVERED" : "PARTIAL",
                   bestOI, bestES, bestFL);
    if (bestOI != FPROP_OFFSET_INTERNAL || bestES != FPROP_ELEMENT_SIZE || bestFL != FPROP_FLAGS)
        Hydro::logInfo("EngineAPI: FProperty offsets DIFFER from stock UE 5.5 "
                       "(stock: OI=0x%X ES=0x%X FL=0x%X)",
                       FPROP_OFFSET_INTERNAL, FPROP_ELEMENT_SIZE, FPROP_FLAGS);

    // -- Stage G: discover FArrayProperty::Inner via Tags (TArray<FName>) --
    // FArrayProperty extends FProperty with a pointer to the element-type
    // FProperty. Sweep all 8-byte-aligned offsets in [0x40, 0x100); the
    // validator accepts only a pointer that dereferences to a FProperty
    // whose TypeName is "NameProperty" (Tags is TArray<FName>).
    // Failure is non-fatal; readToLua's array branch falls back to an empty table.
    if (tagsProp) {
        const auto& types = getPropertyTypeNames();
        int32_t bestAI = probeOffset(0x40, 0x100, 8, [&](int off) -> bool {
            void* inner = nullptr;
            if (!safeReadPtr((uint8_t*)tagsProp + off, &inner) || !inner) return false;
            return getPropertyTypeNameIndex(inner) == types.nameProperty;
        });
        s_layout.arrayInner = bestAI;
        if (bestAI >= 0) {
            Hydro::logInfo("EngineAPI:   ARRAY_INNER=0x%X validated via Tags->NameProperty",
                           bestAI);
        } else {
            Hydro::logWarn("EngineAPI: Stage G (FArrayProperty::Inner) FAILED - "
                           "TArray reads will return empty tables");
        }
    }

    // -- Stage H: discover FStructProperty::Struct via PrimaryActorTick --
    // FStructProperty extends FProperty with a UScriptStruct* describing
    // the struct's shape. Validator: at each 8-byte-aligned offset in
    // [0x40,0x100), dereference the pointer as a UObject and compare its
    // NamePrivate FName index to the index makeFName produces for known
    // struct names. UE convention can strip the F prefix for the
    // UScriptStruct's exposed FName, so we try both forms.
    // Failure is non-fatal - marshal falls back to FVector size heuristic.
    {
        const uint32_t stageHNames[] = {
            makeFName(L"ActorTickFunction"),
            makeFName(L"FActorTickFunction"),
            makeFName(L"TickFunction"),
            makeFName(L"FTickFunction"),
        };
        int32_t bestSS = probeOffset(0x40, 0x100, 8, [&](int off) -> bool {
            void* sstruct = nullptr;
            if (!safeReadPtr((uint8_t*)tickProp + off, &sstruct) || !sstruct) return false;
            uint32_t nameIdx = 0;
            if (!safeReadInt32((uint8_t*)sstruct + UOBJ_NAME, (int32_t*)&nameIdx)) return false;
            if (nameIdx == 0) return false;
            for (uint32_t exp : stageHNames) {
                if (exp != 0 && nameIdx == exp) return true;
            }
            return false;
        });
        s_layout.structStruct = bestSS;
        if (bestSS >= 0) {
            Hydro::logInfo("EngineAPI:   STRUCT_STRUCT=0x%X validated via "
                           "PrimaryActorTick->TickFunction-family (FName-index match)",
                           bestSS);
        } else {
            Hydro::logWarn("EngineAPI: Stage H (FStructProperty::Struct) FAILED - "
                           "expected FName indices: ATF=%u FATF=%u TF=%u FTF=%u",
                           stageHNames[0], stageHNames[1], stageHNames[2], stageHNames[3]);
            for (int off = 0x40; off < 0x100; off += 8) {
                void* sstruct = nullptr;
                bool readOk = safeReadPtr((uint8_t*)tickProp + off, &sstruct);
                uint32_t nameIdx = 0;
                if (readOk && sstruct) {
                    safeReadInt32((uint8_t*)sstruct + UOBJ_NAME, (int32_t*)&nameIdx);
                }
                Hydro::logWarn("  +0x%02X: ptr=%p nameIdx=%u",
                               off, sstruct, nameIdx);
            }
            Hydro::logWarn("EngineAPI: struct reads fall back to size heuristic");
        }
    }

    // -- Derive object/enum/byte property inner-type offsets from structStruct --
    // Every "FProperty subclass that adds a single UObject* as its first added
    // member" lands at the same offset within FProperty:
    //
    //   FStructProperty   : FProperty       { UScriptStruct* Struct; }
    //   FObjectProperty   : FObjectPropertyBase : FProperty { UClass* PropertyClass; }
    //   FByteProperty     : FNumericProperty : FProperty    { UEnum* Enum; }
    //   FEnumProperty     : FProperty       { FNumericProperty* UnderlyingProp;
    //                                         UEnum* Enum; }
    //
    // So objectPropClass and bytePropEnum are at structStruct; enumPropEnum
    // is at structStruct+8 (UnderlyingProp comes first). Validated by reading
    // AActor::Owner (FObjectProperty) -> PropertyClass and confirming its
    // NamePrivate FName matches "Actor".
    if (s_layout.structStruct >= 0) {
        const int32_t derived = s_layout.structStruct;
        s_layout.objectPropClass = derived;
        s_layout.bytePropEnum    = derived;
        s_layout.enumPropEnum    = derived + 8;

        void* ownerProp = findProperty(actorClass, L"Owner");
        if (ownerProp) {
            void* ownerClass = nullptr;
            const uint32_t expectedActorName = makeFName(L"Actor");
            bool validated = false;
            if (safeReadPtr((uint8_t*)ownerProp + derived, &ownerClass) && ownerClass) {
                uint32_t nameIdx = 0;
                if (safeReadInt32((uint8_t*)ownerClass + UOBJ_NAME, (int32_t*)&nameIdx)
                    && nameIdx == expectedActorName) {
                    validated = true;
                }
            }
            if (validated) {
                Hydro::logInfo("EngineAPI:   OBJ_CLASS=0x%X / BYTE_ENUM=0x%X / "
                               "ENUM_ENUM=0x%X (derived from STRUCT_STRUCT, "
                               "validated via Actor::Owner->Actor)",
                               s_layout.objectPropClass, s_layout.bytePropEnum,
                               s_layout.enumPropEnum);
            } else {
                Hydro::logWarn("EngineAPI: derived inner-type offsets did NOT "
                               "validate via Actor::Owner. Keeping them anyway "
                               "(0x%X/+8); dumper will null-check at use sites",
                               derived);
            }
        }
    } else {
        Hydro::logWarn("EngineAPI: STRUCT_STRUCT not discovered - leaving "
                       "OBJ_CLASS/BYTE_ENUM/ENUM_ENUM at -1 (dumper will not "
                       "emit inner type names)");
    }

    return s_layout.succeeded;
}

// FField name offset: discovered once at first call by using findProperty
// with a known property name and checking which offset produces the
// matching FName index.
static int s_ffieldNameOffset = -1;

void discoverFieldNameOffset() {
    void* actorClass = findObject(L"/Script/Engine.Actor");
    if (!actorClass) return;

    uint32_t expectedIdx = makeFName(L"PrimaryActorTick");
    if (expectedIdx == 0) return;

    void* prop = findProperty(actorClass, L"PrimaryActorTick");
    if (!prop) return;

    static const int kOffsets[] = { 0x20, 0x28, 0x18, 0x30, 0x38 };
    for (int off : kOffsets) {
        uint32_t idx = 0;
        safeReadInt32((uint8_t*)prop + off, (int32_t*)&idx);
        if (idx == expectedIdx) {
            s_ffieldNameOffset = off;
            Hydro::logInfo("EngineAPI: FField name offset = 0x%X (validated via PrimaryActorTick, idx=%u)",
                           off, idx);
            return;
        }
    }

    Hydro::logWarn("EngineAPI: FField name offset not found (PrimaryActorTick probe failed)");
    s_ffieldNameOffset = FFIELD_NAME;
}

std::string getFieldName(void* field) {
    if (!field) return {};
    if (s_ffieldNameOffset < 0) discoverFieldNameOffset();
    uint32_t idx = 0;
    safeReadInt32((uint8_t*)field + s_ffieldNameOffset, (int32_t*)&idx);
    return getNameString(idx);
}

// UStruct::SuperStruct offset discovery. AActor's SuperStruct is the UClass
// for "Object" in most UE5 builds; probe a few known layout offsets until
// one dereferences to a struct named "Object".
void discoverSuperOffset() {
    void* actorClass = findObject(L"/Script/Engine.Actor");
    if (!actorClass) { s_superOffset = 0x40; return; }

    uint32_t objectIdx = makeFName(L"Object");
    if (objectIdx == 0) { s_superOffset = 0x40; return; }

    static const int kOffsets[] = { 0x40, 0x48, 0x38, 0x50, 0x30, 0x58 };
    for (int off : kOffsets) {
        void* candidate = nullptr;
        if (!readPtr((uint8_t*)actorClass + off, &candidate) || !candidate) continue;
        uint32_t idx = 0;
        if (!safeReadInt32((uint8_t*)candidate + UOBJ_NAME, (int32_t*)&idx)) continue;
        if (idx == objectIdx) {
            s_superOffset = off;
            Hydro::logInfo("EngineAPI: UStruct::SuperStruct offset = 0x%X (validated via Actor->Object)", off);
            return;
        }
    }

    Hydro::logWarn("EngineAPI: SuperStruct offset not found, defaulting to 0x40");
    s_superOffset = 0x40;
}

void* getSuper(void* ustruct) {
    if (!ustruct) return nullptr;
    if (s_superOffset < 0) discoverSuperOffset();
    void* super = nullptr;
    readPtr((uint8_t*)ustruct + s_superOffset, &super);
    return super;
}

// UClass::ClassFlags offset: /Script/CoreUObject.Object always has
// CLASS_Native (0x00000001) set. Probe offsets until we find one where
// CLASS_Native is set and the value is plausibly small (ClassFlags fit in
// 28 bits).
void discoverClassFlagsOffset() {
    void* uobjectClass = findObject(L"/Script/CoreUObject.Object");
    if (!uobjectClass) { s_classFlagsOffset = 0x1C0; return; }

    static const int kOffsets[] = {
        0x1C0, 0x1B0, 0x1C8, 0x1D0, 0x1B8, 0x1A8, 0x1E0, 0x1E8, 0x1F0, 0x1F8, 0x200
    };
    for (int off : kOffsets) {
        uint32_t flags = 0;
        if (!safeReadInt32((uint8_t*)uobjectClass + off, (int32_t*)&flags)) continue;
        if ((flags & 0x1) == 0) continue;
        if (flags == 0xFFFFFFFF || flags == 0) continue;
        if (flags > 0x10000000) continue;
        s_classFlagsOffset = off;
        Hydro::logInfo("EngineAPI: UClass::ClassFlags offset = 0x%X (validated via Object, flags=0x%X)", off, flags);
        return;
    }

    Hydro::logWarn("EngineAPI: UClass::ClassFlags offset not found, defaulting to 0x1C0");
    s_classFlagsOffset = 0x1C0;
}

uint32_t getClassFlags(void* cls) {
    if (!cls) return 0;
    if (s_classFlagsOffset < 0) discoverClassFlagsOffset();
    uint32_t val = 0;
    safeReadInt32((uint8_t*)cls + s_classFlagsOffset, (int32_t*)&val);
    return val;
}

// UEnum::Names offset: discovered on first call by probing candidate
// offsets for a TArray<TPair<FName, int64>> that decodes sanely.
//
// TArray layout: T* Data (8) + int32 Num (4) + int32 Max (4) = 16B
// Each TPair<FName, int64>: FName (8) + int64 (8) = 16B
void discoverEnumNamesOffset() {
    void* enumProbe = findObject(L"/Script/Engine.ESpawnActorCollisionHandlingMethod");
    if (!enumProbe) enumProbe = findObject(L"/Script/CoreUObject.EObjectFlags");
    if (!enumProbe) {
        void* enumMetaclass = findObject(L"/Script/CoreUObject.Enum");
        if (enumMetaclass) {
            int32_t total = getObjectCount();
            for (int32_t i = 0; i < total && !enumProbe; i++) {
                void* obj = getObjectAt(i);
                if (obj && getObjClass(obj) == enumMetaclass) enumProbe = obj;
            }
        }
    }

    if (!enumProbe) {
        Hydro::logWarn("EngineAPI: no UEnum found for offset probe, defaulting to 0x40");
        s_enumNamesOffset = 0x40;
        return;
    }

    static const int kOffsets[] = { 0x40, 0x48, 0x50, 0x58, 0x38, 0x60, 0x68 };
    for (int off : kOffsets) {
        void* data = nullptr;
        if (!safeReadPtr((uint8_t*)enumProbe + off, &data) || !data) continue;
        if ((uint8_t*)data >= s_gm.base && (uint8_t*)data < s_gm.base + s_gm.size) continue;

        int32_t num = 0, max = 0;
        if (!safeReadInt32((uint8_t*)enumProbe + off + 8, &num)) continue;
        if (!safeReadInt32((uint8_t*)enumProbe + off + 12, &max)) continue;
        if (num < 1 || num > 10000) continue;
        if (max < num) continue;

        uint32_t firstComparisonIdx = 0;
        if (!safeReadInt32(data, (int32_t*)&firstComparisonIdx)) continue;
        if (firstComparisonIdx == 0) continue;
        std::string firstName = getNameString(firstComparisonIdx);
        if (firstName.empty() || firstName[0] == '<') continue;

        s_enumNamesOffset = off;
        Hydro::logInfo("EngineAPI: UEnum::Names offset = 0x%X (validated via %p, first='%s', num=%d)",
                       off, enumProbe, firstName.c_str(), num);
        return;
    }

    Hydro::logWarn("EngineAPI: UEnum::Names offset not found, defaulting to 0x40");
    s_enumNamesOffset = 0x40;
}

// OuterPrivate offset: AActor's outer is the /Script/Engine package.
// Probe offsets and validate via name lookup.
void discoverOuterOffset() {
    void* actorClass = findObject(L"/Script/Engine.Actor");
    if (!actorClass) {
        Hydro::logWarn("EngineAPI: OuterPrivate probe: AActor not found, defaulting to 0x%X", UOBJ_OUTER);
        s_outerOffset = UOBJ_OUTER;
        return;
    }

    static const int kOffsets[] = { 0x20, 0x28, 0x30, 0x18, 0x38, 0x40, 0x48 };
    Hydro::logInfo("EngineAPI: OuterPrivate probe on AActor at %p:", actorClass);
    for (int off : kOffsets) {
        void* candidate = nullptr;
        if (!readPtr((uint8_t*)actorClass + off, &candidate) || !candidate) {
            Hydro::logInfo("  +0x%02X: null", off);
            continue;
        }
        uint32_t idx = 0;
        safeReadInt32((uint8_t*)candidate + UOBJ_NAME, (int32_t*)&idx);
        std::string name = getNameString(idx);
        Hydro::logInfo("  +0x%02X: %p nameIdx=%u name='%s'", off, candidate, idx, name.c_str());
    }

    void* outer = nullptr;
    readPtr((uint8_t*)actorClass + UOBJ_OUTER, &outer);
    if (outer) {
        std::string outerName = getObjectName(outer);
        Hydro::logInfo("EngineAPI: OuterPrivate offset = 0x%X, outer name = '%s'", UOBJ_OUTER, outerName.c_str());
    }
    s_outerOffset = UOBJ_OUTER;
}

void* getOuter(void* obj) {
    if (!obj) return nullptr;
    if (s_outerOffset < 0) discoverOuterOffset();
    void* outer = nullptr;
    readPtr((uint8_t*)obj + s_outerOffset, &outer);
    return outer;
}

} // namespace Hydro::Engine
