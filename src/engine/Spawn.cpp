#include "Spawn.h"
#include "../EngineAPI.h"
#include "../HydroCore.h"
#include "Internal.h"
#include "GUObjectArray.h"
#include "Layout.h"
#include "ObjectLookup.h"
#include "ProcessEvent.h"
#include "Reflection.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace Hydro::Engine {

// Globals owned here (also declared extern in Spawn.h). discoverEngineObjects()
// in EngineAPI.cpp writes these during initialize().
void* s_world             = nullptr;   // UWorld*
void* s_gameplayStaticsCDO = nullptr;  // Default__GameplayStatics
void* s_spawnFunc         = nullptr;   // BeginDeferredActorSpawnFromClass UFunction*
void* s_finishSpawnFunc   = nullptr;   // FinishSpawningActor UFunction*
// Public API: spawnActor

// BeginDeferredActorSpawnFromClass params (UE 5.5, As502Plus)
#pragma pack(push, 1)
struct alignas(16) SpawnParams {
    void*    worldContext;       // 0x00
    void*    actorClass;        // 0x08
    // FTransform (96 bytes, 16-byte aligned)
    double   rotX, rotY, rotZ, rotW;  // 0x10 - FQuat
    double   locX, locY, locZ;        // 0x30 - FVector translation
    double   pad1;                     // 0x48
    double   scaleX, scaleY, scaleZ;  // 0x50 - FVector scale
    double   pad2;                     // 0x68
    uint8_t  collisionMethod;   // 0x70
    uint8_t  pad3[7];           // 0x71
    void*    owner;             // 0x78
    uint8_t  scaleMethod;       // 0x80
    uint8_t  pad4[7];           // 0x81
    void*    returnValue;       // 0x88 - OUT
};
#pragma pack(pop)

// --- AActor::DispatchBeginPlay discovery --------------------------------
//
// Background: DispatchBeginPlay is the non-virtual engine funnel that every
// actor's BeginPlay passes through. Hooking it catches every actor's begin-
// play exactly once, regardless of subclass vtable overrides. Hooking the
// virtual AActor::BeginPlay directly misses calls dispatched through
// subclass vtables, which is why our earlier attempt produced zero fires.
//
// We find DispatchBeginPlay by scanning for its distinctive last-instruction
// shape: `call qword ptr [reg + BeginPlay_vtable_offset]`, where the offset
// is whatever vtable slot AActor::BeginPlay occupies in the current engine
// version (0x3A0 in UE 5.5). That instruction is 6 or 7 bytes of the form
// `FF 9X XX XX XX XX` (ModRM byte 0x90..0x97 selects the register; no SIB).
//
// A literal byte scan finds MANY matches - any function that does a virtual
// call at that offset. To narrow to DispatchBeginPlay we also require:
//   - exactly ONE such virtual call in the enclosing function
//   - function size is small (DispatchBeginPlay is ~40-200 bytes)
//   - the function is called by OTHER code in the module (non-leaf caller)
//
// Candidates that pass are scored. The single best candidate wins. All
// candidates are logged so a future engine version shift is debuggable.

// Walk backwards from `at` looking for the function start. Heuristic: the
// previous function ends with a RET (0xC3/0xC2) followed by alignment padding
// (0xCC INT3 bytes). When we see that padding run, the byte after it is the
// next function's entry. We cap the walk at 4KB to keep cost bounded.
static uint8_t* findFunctionStartBackwards(uint8_t* at, uint8_t* modBase) {
    uint8_t* p = at;
    uint8_t* limit = (p > modBase + 4096) ? p - 4096 : modBase;
    for (; p > limit; p--) {
        // Look for the pattern: RET (0xC3 or 0xC2 xx xx) followed by 0xCC padding.
        // The candidate function starts right after the padding run.
        if (p[0] == 0xCC) {
            // Walk forward past the padding run to the first non-CC byte.
            uint8_t* q = p;
            while (q < at && *q == 0xCC) q++;
            // Preceded by a RET or near-RET instruction? Check a few bytes back.
            // C3 = RET, C2 xx xx = RET imm16, E9 xx xx xx xx = JMP rel32 (tail call)
            uint8_t prev = (p > modBase) ? *(p - 1) : 0;
            if (prev == 0xC3 || prev == 0xCC) {
                return q;
            }
            // Also accept "JMP rel32" as function boundary (tail-call ending)
            if (p > modBase + 5 && *(p - 5) == 0xE9) {
                return q;
            }
        }
    }
    return nullptr;
}

struct DbpCandidate {
    uint8_t* funcStart;
    size_t approxSize;
    int virtualCalls;    // count of call [reg+offset] matching the target offset
    int flagWrites;      // count of mov byte ptr [reg+N], imm8 (actor flag sets)
    int callers;         // count of direct E8 calls targeting this function
    int score;
};

// Count direct CALL instructions (E8 rel32) targeting `target` across a
// subset of the module. DispatchBeginPlay has multiple callers (at least
// UWorld::InitializeActorsForPlay and AActor::PostActorConstruction), so
// a callers count of 0 rules out many false positives. We cap the search
// region so this doesn't blow the frame budget on a module that might be
// hundreds of MB.
static int countDirectCallers(uint8_t* target, uint8_t* modBase, size_t modSize) {
    int count = 0;
    const size_t kMaxScan = 64 * 1024 * 1024;  // 64MB - plenty for .text
    size_t limit = modSize < kMaxScan ? modSize : kMaxScan;
    for (size_t i = 0; i + 5 < limit; i++) {
        if (modBase[i] != 0xE8) continue;
        int32_t rel = *(int32_t*)(modBase + i + 1);
        uint8_t* dst = modBase + i + 5 + rel;
        if (dst == target && ++count >= 4) return count;  // 4+ is plenty
    }
    return count;
}

void* findDispatchBeginPlay(int beginPlayVtableOffset) {
    if (!s_gm.base || !s_gm.size) {
        logError("EngineAPI: findDispatchBeginPlay - game module not found");
        return nullptr;
    }

    uint8_t* mod = s_gm.base;
    size_t modSize = s_gm.size;
    int32_t targetOffset = (int32_t)beginPlayVtableOffset;

    // Pass 1: find every `FF /2 m32` (near call to memory with disp32) whose
    // displacement matches the BeginPlay vtable offset. ModRM byte is
    // 0x90..0x97 (mod=10, reg=010 for CALL, rm picks the register - we don't
    // care which). SIB-indexed forms (rm=4, 0x94) are possible but rare for
    // virtual dispatch and we skip them to avoid SIB parsing.
    std::vector<uint8_t*> hits;
    for (size_t i = 0; i + 6 < modSize; i++) {
        if (mod[i] != 0xFF) continue;
        uint8_t modrm = mod[i + 1];
        // mod=10 (disp32), reg=010 (CALL), rm=0..7 except 4 (SIB)
        if ((modrm & 0xF8) != 0x90) continue;
        if ((modrm & 0x07) == 4) continue;
        int32_t disp = *(int32_t*)(mod + i + 2);
        if (disp == targetOffset) {
            hits.push_back(mod + i);
        }
    }
    logInfo("EngineAPI: DispatchBeginPlay scan - %zu virtual-call hits at offset 0x%X",
            hits.size(), targetOffset);
    if (hits.empty()) return nullptr;

    // Pass 2: group hits by enclosing function. Build candidate set.
    std::unordered_map<uint8_t*, DbpCandidate> candidates;
    for (uint8_t* hit : hits) {
        uint8_t* start = findFunctionStartBackwards(hit, mod);
        if (!start) continue;
        auto& c = candidates[start];
        c.funcStart = start;
        c.virtualCalls++;
    }

    // Pass 3a: cheap structural scoring for every unique candidate (local
    // only - no full-module scans). This trims the field before we spend
    // effort counting direct callers, which is quadratic otherwise.
    std::vector<DbpCandidate*> scored;
    for (auto& [start, c] : candidates) {
        // Approximate function size: scan forward until first RET + padding.
        size_t maxScan = 512;
        size_t fnLen = 0;
        for (size_t j = 0; j < maxScan && (start + j) < (mod + modSize); j++) {
            if (start[j] == 0xC3 || start[j] == 0xC2) {
                fnLen = j + 1;
                break;
            }
        }
        if (fnLen == 0) fnLen = 64;  // unknown; assume reasonable
        c.approxSize = fnLen;

        // Flag writes: count `mov byte ptr [reg+N], imm8` patterns (C6 4X NN II)
        // inside the function. DispatchBeginPlay writes the HasActorBegunPlay
        // bitfield before calling BeginPlay.
        for (size_t j = 0; j + 4 < fnLen; j++) {
            if (start[j] == 0xC6 && (start[j + 1] & 0xF0) == 0x40) {
                c.flagWrites++;
            }
        }
        c.callers = 0;  // computed later for the top-N only
        scored.push_back(&c);
    }

    // Cheap pre-rank: virtualCalls==1 + reasonable size wins first.
    std::sort(scored.begin(), scored.end(),
              [](const DbpCandidate* a, const DbpCandidate* b) {
                  if ((a->virtualCalls == 1) != (b->virtualCalls == 1))
                      return a->virtualCalls == 1;
                  bool aSize = a->approxSize >= 32 && a->approxSize <= 256;
                  bool bSize = b->approxSize >= 32 && b->approxSize <= 256;
                  if (aSize != bSize) return aSize;
                  return a->approxSize < b->approxSize;
              });

    // Pass 3b: count direct callers only for the top 10 - keeps the scan
    // bounded regardless of total candidate count. 10 is plenty because
    // the cheap pre-rank puts the right candidate near the top.
    size_t deepScan = scored.size() < 10 ? scored.size() : (size_t)10;
    for (size_t i = 0; i < deepScan; i++) {
        scored[i]->callers = countDirectCallers(scored[i]->funcStart, mod, modSize);
    }

    // Score all candidates. Rubric:
    //   +1000 if exactly 1 virtual call at the offset (DispatchBeginPlay
    //         invokes BeginPlay exactly once - functions with many such
    //         calls are almost certainly something else)
    //   +200 per caller (non-leaf -> more likely a dispatch funnel)
    //   +100 if function is reasonably sized (32..256 bytes)
    //   +50 per flag write (up to 3)
    //   -200 if function is very large (>400 bytes; DispatchBeginPlay is tight)
    for (auto* cp : scored) {
        DbpCandidate& c = *cp;
        c.score = 0;
        if (c.virtualCalls == 1) c.score += 1000;
        c.score += c.callers * 200;
        if (c.approxSize >= 32 && c.approxSize <= 256) c.score += 100;
        c.score += (c.flagWrites < 3 ? c.flagWrites : 3) * 50;
        if (c.approxSize > 400) c.score -= 200;
    }

    // Sort by score descending, log top candidates for debuggability.
    std::sort(scored.begin(), scored.end(),
              [](const DbpCandidate* a, const DbpCandidate* b) { return a->score > b->score; });

    size_t topN = scored.size() < 5 ? scored.size() : (size_t)5;
    logInfo("EngineAPI: DispatchBeginPlay - %zu unique candidates, top %zu:",
            scored.size(), topN);
    for (size_t i = 0; i < topN; i++) {
        const DbpCandidate* c = scored[i];
        logInfo("  [%zu] score=%d func=exe+0x%zX size=%zu vcalls=%d flagWr=%d callers=%d",
                i, c->score, (size_t)(c->funcStart - mod),
                c->approxSize, c->virtualCalls, c->flagWrites, c->callers);
    }

    if (!scored.empty() && scored[0]->score > 500) {
        uint8_t* bestFunc = scored[0]->funcStart;
        logInfo("EngineAPI: DispatchBeginPlay chosen at %p (exe+0x%zX, score=%d)",
                bestFunc, (size_t)(bestFunc - mod), scored[0]->score);
        return bestFunc;
    }

    logError("EngineAPI: no strong DispatchBeginPlay candidate (best score=%d)",
             scored.empty() ? 0 : scored[0]->score);
    return nullptr;
}

// Cached param offsets for BeginDeferredActorSpawnFromClass. Looked up once
// per session via the UFunction's property chain, so the layout is correct
// for whichever UE 5.x build the host uses (UE 5.1 / 5.5 / 5.6 may shift
// alignment or insert/remove flags). Using static + sentinel; -1 means
// "not yet resolved" so we re-attempt on first spawn after engine init.
struct SpawnParamLayout {
    bool resolved = false;
    int32_t worldOff = -1;
    int32_t classOff = -1;
    int32_t transformOff = -1;
    int32_t collisionOff = -1;
    int32_t ownerOff = -1;
    int32_t scaleMethodOff = -1;
    int32_t retOff = -1;
    int32_t totalSize = -1;
};
static SpawnParamLayout s_spawnLayout;

static void resolveSpawnLayout() {
    if (s_spawnLayout.resolved) return;
    if (!s_spawnFunc) return;
    // BeginDeferredActorSpawnFromClass param names (must match UE source):
    auto off = [](const wchar_t* n) -> int32_t {
        void* prop = findProperty(s_spawnFunc, n);
        return prop ? getPropertyOffset(prop) : -1;
    };
    s_spawnLayout.worldOff       = off(L"WorldContextObject");
    s_spawnLayout.classOff       = off(L"ActorClass");
    s_spawnLayout.transformOff   = off(L"SpawnTransform");
    s_spawnLayout.collisionOff   = off(L"CollisionHandlingOverride");
    s_spawnLayout.ownerOff       = off(L"Owner");
    s_spawnLayout.scaleMethodOff = off(L"TransformScaleMethod");
    s_spawnLayout.retOff         = (int32_t)getUFunctionRetOffset(s_spawnFunc);
    s_spawnLayout.totalSize      = (int32_t)getUFunctionParmsSize(s_spawnFunc);
    s_spawnLayout.resolved = (s_spawnLayout.worldOff >= 0 && s_spawnLayout.classOff >= 0
        && s_spawnLayout.transformOff >= 0 && s_spawnLayout.totalSize > 0);
    Hydro::logInfo("EngineAPI: SpawnParams layout (reflection-derived): "
        "World=%d Class=%d Transform=%d Collision=%d Owner=%d ScaleMethod=%d Ret=%d Total=%d resolved=%d",
        s_spawnLayout.worldOff, s_spawnLayout.classOff, s_spawnLayout.transformOff,
        s_spawnLayout.collisionOff, s_spawnLayout.ownerOff, s_spawnLayout.scaleMethodOff,
        s_spawnLayout.retOff, s_spawnLayout.totalSize, s_spawnLayout.resolved ? 1 : 0);
    // DIAG: confirm we resolved the right UFunction. Log fully-qualified
    // name + owning class so we can tell if findObject landed on a wrong
    // overload on UE 5.6 (e.g. BeginSpawningActorFromClass legacy).
    {
        std::string fnName = getObjectName(s_spawnFunc);
        void* fnOuter = nullptr;
        safeReadPtr((uint8_t*)s_spawnFunc + UOBJ_OUTER, &fnOuter);
        std::string outerName = fnOuter ? getObjectName(fnOuter) : "<null>";
        void* fnClass = getObjClass(s_spawnFunc);
        std::string fnClassName = fnClass ? getObjectName(fnClass) : "<null>";
        Hydro::logInfo("EngineAPI: s_spawnFunc=%p name='%s' outer='%s' class='%s'",
            s_spawnFunc, fnName.c_str(), outerName.c_str(), fnClassName.c_str());
    }
}

void* spawnActor(void* actorClass, double x, double y, double z) {
    if (!s_ready || !s_spawnFunc || !s_gameplayStaticsCDO) {
        Hydro::logError("EngineAPI: Not ready for spawning");
        return nullptr;
    }
    if (!actorClass) {
        Hydro::logError("EngineAPI: actorClass is null");
        return nullptr;
    }

    // Refresh world pointer before each spawn - the world may have changed
    // since initialize() ran (e.g., map transition from menu to gameplay).
    refreshWorld();
    if (!s_world) {
        Hydro::logError("EngineAPI: No valid UWorld found");
        return nullptr;
    }

    Hydro::logInfo("EngineAPI: Spawning actor at (%.0f, %.0f, %.0f) in world %p...", x, y, z, s_world);

    // Resolve param offsets via reflection on first call. If the property
    // chain isn't readable yet (e.g., very early init), fall back to the
    // hardcoded UE 5.5 layout so existing UE 5.5 paths still work.
    resolveSpawnLayout();

    if (s_spawnLayout.resolved && s_spawnLayout.totalSize <= 256) {
        // BeginDeferredActorSpawnFromClass has meta=(WorldContext="WorldContextObject")
        // - UE's GetWorldFromContextObject() calls obj->GetWorld() and expects
        // the object to BE in a world. UWorld::GetWorld() returns nullptr in
        // some UE 5.5 builds (DMG ThirdPersonMap empirically), causing access
        // violation deep in the spawn code. Prefer a player Pawn/Character
        // as WorldContext (canonical BP-callable pattern).
        // WorldContext resolution. UE's GetWorldFromContextObject expects an
        // object that's IN a world (i.e. an actor instance). A UWorld* itself
        // does not satisfy this (DMG UE 5.5 empirically crashes). A CDO
        // also fails (returns null GetWorld). We need a real in-world actor.
        //
        // Strategy: prime the find cache by calling findFirstOf once (which
        // resolves the UClass* via GUObjectArray walk - may return a CDO),
        // then call again - the second call routes through
        // UGameplayStatics::GetAllActorsOfClass, which ONLY returns
        // in-world instances, never CDOs.
        // Helper: detect Class Default Object via UObject flags. RF_ClassDefaultObject
        // = 0x10 - stable across UE versions. CDOs have no world (GetWorld()
        // returns null), so passing one as WorldContext fails the spawn.
        auto isCDO = [](void* obj) {
            if (!obj) return true;
            uint32_t flags = 0;
            if (!safeReadInt32((uint8_t*)obj + UOBJ_FLAGS, (int32_t*)&flags)) return true;
            return (flags & 0x10) != 0;  // RF_ClassDefaultObject
        };

        void* worldContext = nullptr;
        const char* wcSource = nullptr;

        // PREFER APlayerController. Reflection-resolved (no string anchors
        // in shipping; the UClass is loaded into GUObjectArray by every
        // UE game that has player input). PlayerController is created by
        // GameMode when a player connects, ALWAYS lives in the gameplay
        // world (never transient/debug), and exists across every UE 4/5
        // engine version under the same `/Script/Engine.PlayerController`
        // path. This is the strongest generic gameplay-world anchor -
        // beats `getPlayerCharacter` (game-specific Pawn subclasses cause
        // false-misses on hosts whose Character class differs) and beats
        // a generic Actor walk (UE 5.6 spawns ChaosDebugDrawActor early
        // and the first-non-CDO pick lands in a transient world).
        // T7B regression (2026-05-10): even with a valid PlayerController as
        // WorldContext, BeginDeferred returns null on UE 5.6 - diagnosed as
        // GEngine->GetWorldFromContextObject(PC) likely returning null
        // because the PC's outer chain isn't wired to a UWorld at script-run
        // time on UE 5.6 (LogSpawn warning suppressed in shipping). The Pawn
        // a PC controls is always a child of the Level (Level->Actors), so
        // its GetWorld() is reliable. Try Pawn first; PC second; UWorld* last.
        worldContext = getPlayerCharacter(0);
        if (worldContext) wcSource = "getPlayerCharacter UFunction (Pawn - in-Level)";
        if (!worldContext) {
            worldContext = getPlayerPawn(0);
            if (worldContext) wcSource = "getPlayerPawn UFunction";
        }
        if (!worldContext) {
            worldContext = findPlayerController();
            if (worldContext) wcSource = "APlayerController (GUObjectArray + class chain)";
        }
        if (!worldContext && s_guObjectArray) {
            int32_t count = getObjectCount();
            for (int32_t i = 0; i < count && !worldContext; i++) {
                void* obj = getObjectAt(i);
                if (!obj || isCDO(obj)) continue;
                void* cls = getObjClass(obj);
                if (!cls) continue;
                if (!isActorSubclass(cls)) continue;
                worldContext = obj;
                wcSource = "GUObjectArray walk (non-CDO actor - may be debug-world)";
            }
        }
        if (!worldContext) {
            worldContext = s_world;
            wcSource = "UWorld* (last-resort - empirically crashes on UE 5.5 DMG)";
        }
        // Diagnostic: print WorldContext object name + class name so we can
        // see whether we picked a real in-world actor or a CDO/template.
        if (worldContext) {
            uint32_t wcNameIdx = 0;
            safeReadInt32((uint8_t*)worldContext + UOBJ_NAME, (int32_t*)&wcNameIdx);
            std::string wcName = getNameString(wcNameIdx);
            void* wcClass = getObjClass(worldContext);
            uint32_t wcClassNameIdx = 0;
            if (wcClass) safeReadInt32((uint8_t*)wcClass + UOBJ_NAME, (int32_t*)&wcClassNameIdx);
            std::string wcClassName = wcClass ? getNameString(wcClassNameIdx) : "<null>";
            uint32_t wcFlags = 0;
            safeReadInt32((uint8_t*)worldContext + UOBJ_FLAGS, (int32_t*)&wcFlags);
            // Diagnostic on actor class too
            uint32_t acNameIdx = 0;
            safeReadInt32((uint8_t*)actorClass + UOBJ_NAME, (int32_t*)&acNameIdx);
            std::string acName = getNameString(acNameIdx);
            void* acClass = getObjClass(actorClass);
            uint32_t acClassNameIdx = 0;
            if (acClass) safeReadInt32((uint8_t*)acClass + UOBJ_NAME, (int32_t*)&acClassNameIdx);
            std::string acClassName = acClass ? getNameString(acClassNameIdx) : "<null>";
            Hydro::logInfo("EngineAPI: WorldContext = %p name='%s' class='%s' flags=0x%X (%s)",
                worldContext, wcName.c_str(), wcClassName.c_str(), wcFlags, wcSource);
            Hydro::logInfo("EngineAPI: ActorClass = %p name='%s' class='%s'",
                actorClass, acName.c_str(), acClassName.c_str());
        } else {
            Hydro::logInfo("EngineAPI: WorldContext for spawn = %p (%s)",
                worldContext, wcSource);
        }
        // Reflection-derived param block - version-resilient path.
        alignas(16) uint8_t params[256] = {};
        *(void**)(params + s_spawnLayout.worldOff) = worldContext;
        *(void**)(params + s_spawnLayout.classOff) = actorClass;
        // FTransform: FQuat at +0x00 (32 bytes), Translation FVector at +0x20
        // (24+8 pad = 32 bytes), Scale3D FVector at +0x40 (24+8 pad). Total
        // 96 bytes, double-precision (UE 5.x default since 5.0). Layout
        // within FTransform itself is stable across UE 5.x.
        uint8_t* tx = params + s_spawnLayout.transformOff;
        *(double*)(tx + 0x00) = 0.0;  // QuatX
        *(double*)(tx + 0x08) = 0.0;  // QuatY
        *(double*)(tx + 0x10) = 0.0;  // QuatZ
        *(double*)(tx + 0x18) = 1.0;  // QuatW (identity)
        *(double*)(tx + 0x20) = x;    // TranslationX
        *(double*)(tx + 0x28) = y;
        *(double*)(tx + 0x30) = z;
        *(double*)(tx + 0x40) = 1.0;  // ScaleX
        *(double*)(tx + 0x48) = 1.0;
        *(double*)(tx + 0x50) = 1.0;
        if (s_spawnLayout.collisionOff >= 0)
            *(uint8_t*)(params + s_spawnLayout.collisionOff) = 1;  // AlwaysSpawn
        if (s_spawnLayout.ownerOff >= 0)
            *(void**)(params + s_spawnLayout.ownerOff) = nullptr;
        if (s_spawnLayout.scaleMethodOff >= 0)
            *(uint8_t*)(params + s_spawnLayout.scaleMethodOff) = 1;  // MultiplyWithRoot
        // ReturnValue slot is zero already.

        // Pre-spawn class diagnostics. UE 5.6 SpawnActor returns null
        // primarily for: CLASS_Abstract (0x1), CLASS_Deprecated (0x2),
        // !IsChildOf(AActor), or NeedsLoadForServer/Client mismatch on the
        // CDO. Probe these from the live class so we know which path
        // BeginDeferredActorSpawnFromClass is taking when it returns null
        // (LogSpawn warnings get suppressed in shipping cooks).
        {
            bool isActor = isActorSubclass(actorClass);
            // Walk SuperStruct chain, log names so we can see why
            // !IsChildOf(AActor) might trigger (e.g. broken chain).
            int superOff = (s_superOffset > 0) ? s_superOffset : 0x40;
            std::string chain;
            void* cur = actorClass;
            for (int d = 0; d < 8 && cur; d++) {
                if (!chain.empty()) chain += " -> ";
                chain += getObjectName(cur);
                void* super = nullptr;
                if (!safeReadPtr((uint8_t*)cur + superOff, &super)) break;
                if (super == cur) break;
                cur = super;
            }
            // ClassFlags lives somewhere in UClass body. UE 5.x typical
            // offsets: 0xB0..0xD0 region. Probe likely qwords for a value
            // with reasonable EClassFlags bits set. Real EClassFlags values:
            //   CLASS_Abstract        = 0x00000001
            //   CLASS_Deprecated      = 0x00002000
            //   CLASS_Hidden          = 0x00001000
            //   CLASS_HideDropDown    = 0x00004000
            //   CLASS_Native          = 0x00000080
            //   CLASS_Compiled        = 0x00040000  (set on most cooked BPGCs)
            // We log every qword from +0x90 to +0x110 stepping 4 bytes so the
            // operator can eyeball which offset holds the flags.
            Hydro::logInfo("EngineAPI: Pre-spawn class diag for %p:", actorClass);
            Hydro::logInfo("  isActorSubclass = %d", isActor ? 1 : 0);
            Hydro::logInfo("  SuperStruct chain: %s", chain.c_str());

            // Compare ClassFlags-region bytes against AActor's. Bytes that
            // DIFFER between a spawnable class (AActor) and our class are
            // candidates for "the flag we're tripping on." Abstract is the
            // top suspect; if AActor has 0 at +0x0D0 and Sphere_C has 1,
            // that's the flag. (isActorSubclass already cached AActor's
            // UClass via findObject - re-resolve through the same path.)
            void* aactorPtr = findObject(L"/Script/Engine.Actor");
            if (aactorPtr && aactorPtr != actorClass) {
                Hydro::logInfo("  delta vs AActor (offsets where bytes differ):");
                for (int off = 0x80; off <= 0x180; off += 4) {
                    int32_t mine = 0, actor = 0;
                    if (!safeReadInt32((uint8_t*)actorClass + off, &mine)) continue;
                    if (!safeReadInt32((uint8_t*)aactorPtr + off, &actor)) continue;
                    if (mine == actor) continue;
                    // Skip pointer-shaped values (high bits set - likely
                    // SuperStruct / ChildProperties / etc).
                    uint32_t mu = (uint32_t)mine, au = (uint32_t)actor;
                    if (mu >= 0x40000000u || au >= 0x40000000u) continue;
                    Hydro::logInfo("    +0x%03X: actor=0x%08X  ours=0x%08X  delta=0x%08X",
                                   off, au, mu, mu ^ au);
                }
            }
            // Probe ClassFlags region - real EClassFlags fits in a uint32,
            // typically with low-bit flags set. Print every uint32 from
            // +0x90 to +0x110 so we can spot CLASS_Abstract (0x1) etc.
            for (int off = 0x90; off <= 0x110; off += 4) {
                int32_t v = 0;
                if (!safeReadInt32((uint8_t*)actorClass + off, &v)) continue;
                uint32_t u = (uint32_t)v;
                // Suppress all-zero and all-ones noise. Print only values
                // with a plausible mix of class-flag bits.
                if (u == 0 || u == 0xFFFFFFFFu) continue;
                if ((u & 0xF0000000) != 0 && (u & 0x0FFFFFFF) == 0) continue;
                Hydro::logInfo("  +0x%03X: 0x%08X%s%s%s",
                               off, u,
                               (u & 0x00000001) ? " [Abstract?]"   : "",
                               (u & 0x00002000) ? " [Deprecated?]" : "",
                               (u & 0x00040000) ? " [Compiled?]"   : "");
            }
        }

        // CLASS_Abstract fixup. Confirmed empirically on UE 5.6 DMG:
        // BlueprintGeneratedClass'es from runtime-mounted mod paks land
        // with CLASS_Abstract (bit 0) set in UClass::ClassFlags at +0x0D0,
        // which makes UWorld::SpawnActor early-return null at LevelActor.cpp:488
        // ("class %s is abstract"). Comparing against AActor (also at +0x0D0,
        // value 0x0) confirms the offset and the bit. Strip the bit at runtime
        // - it's not load-bearing for any BP we'd spawn, the BP author didn't
        // intend it (it's a cook-side artifact), and UE itself does the same
        // strip in some editor-only flows.
        constexpr int kClassFlagsOff = 0x0D0;
        constexpr uint32_t kClassAbstract = 0x00000001;
        {
            int32_t cf = 0;
            if (safeReadInt32((uint8_t*)actorClass + kClassFlagsOff, &cf) &&
                ((uint32_t)cf & kClassAbstract)) {
                uint32_t fixed = (uint32_t)cf & ~kClassAbstract;
                *(uint32_t*)((uint8_t*)actorClass + kClassFlagsOff) = fixed;
                Hydro::logInfo("EngineAPI: stripped CLASS_Abstract on %p "
                               "(0x%08X -> 0x%08X) for runtime spawn",
                               actorClass, (uint32_t)cf, fixed);
            }
        }

        // DIAG: snapshot the first 160 bytes of the param block immediately
        // before the call. If reflection offsets are correct, we should see
        // worldContext pointer @ worldOff, actorClass pointer @ classOff, the
        // transform doubles @ transformOff. After the call, the ReturnValue
        // slot @ retOff should hold the spawned actor pointer (or stay zero
        // if engine returned null). If NO byte changes between before/after,
        // ProcessEvent didn't actually dispatch - func pointer / Native field
        // / CDO is wrong.
        constexpr int kDiagBytes = 160;
        uint8_t snapBefore[kDiagBytes] = {};
        memcpy(snapBefore, params, kDiagBytes);
        Hydro::logInfo("EngineAPI: param block BEFORE callProcessEvent (first %d bytes):", kDiagBytes);
        for (int row = 0; row < kDiagBytes; row += 16) {
            Hydro::logInfo("  +0x%03X: %02X %02X %02X %02X %02X %02X %02X %02X  %02X %02X %02X %02X %02X %02X %02X %02X",
                row,
                snapBefore[row+0],  snapBefore[row+1],  snapBefore[row+2],  snapBefore[row+3],
                snapBefore[row+4],  snapBefore[row+5],  snapBefore[row+6],  snapBefore[row+7],
                snapBefore[row+8],  snapBefore[row+9],  snapBefore[row+10], snapBefore[row+11],
                snapBefore[row+12], snapBefore[row+13], snapBefore[row+14], snapBefore[row+15]);
        }

        if (!callProcessEvent(s_gameplayStaticsCDO, s_spawnFunc, params)) {
            Hydro::logError("EngineAPI: BeginDeferredActorSpawnFromClass failed (reflection path, ProcessEvent crashed)");
            return nullptr;
        }

        // DIAG: diff the param block. List every offset where a byte changed.
        // If nothing changed -> ProcessEvent didn't dispatch. If changes are
        // confined to slots we expect (retOff and possibly Owner / pad), the
        // call ran cleanly and the engine just returned null. If unexpected
        // slots changed, our layout is wrong.
        bool anyChanged = false;
        for (int i = 0; i < kDiagBytes; i++) {
            if (((uint8_t*)params)[i] != snapBefore[i]) { anyChanged = true; break; }
        }
        Hydro::logInfo("EngineAPI: param block AFTER callProcessEvent (anyChanged=%d):", anyChanged ? 1 : 0);
        for (int row = 0; row < kDiagBytes; row += 16) {
            uint8_t* aft = params + row;
            uint8_t* bef = snapBefore + row;
            bool rowDiff = false;
            for (int j = 0; j < 16; j++) if (aft[j] != bef[j]) { rowDiff = true; break; }
            if (!rowDiff && !anyChanged) continue;  // suppress all-equal rows if NOTHING changed at all
            Hydro::logInfo("  +0x%03X: %02X %02X %02X %02X %02X %02X %02X %02X  %02X %02X %02X %02X %02X %02X %02X %02X%s",
                row,
                aft[0],  aft[1],  aft[2],  aft[3],  aft[4],  aft[5],  aft[6],  aft[7],
                aft[8],  aft[9],  aft[10], aft[11], aft[12], aft[13], aft[14], aft[15],
                rowDiff ? "  (delta)" : "");
        }

        void* result = (s_spawnLayout.retOff >= 0)
            ? *(void**)(params + s_spawnLayout.retOff)
            : nullptr;
        if (!result) {
            Hydro::logError("EngineAPI: BeginDeferredActorSpawnFromClass returned null");
            return nullptr;
        }

        // FinishSpawningActor - derive its param layout reflectively too.
        if (s_finishSpawnFunc) {
            int32_t fsActorOff = -1, fsTxOff = -1, fsScaleOff = -1;
            uint16_t fsTotal = getUFunctionParmsSize(s_finishSpawnFunc);
            if (void* p = findProperty(s_finishSpawnFunc, L"Actor"))
                fsActorOff = getPropertyOffset(p);
            if (void* p = findProperty(s_finishSpawnFunc, L"SpawnTransform"))
                fsTxOff = getPropertyOffset(p);
            if (void* p = findProperty(s_finishSpawnFunc, L"TransformScaleMethod"))
                fsScaleOff = getPropertyOffset(p);
            if (fsActorOff >= 0 && fsTxOff >= 0 && fsTotal <= 512) {
                alignas(16) uint8_t fsParams[512] = {};
                *(void**)(fsParams + fsActorOff) = result;
                uint8_t* fsTx = fsParams + fsTxOff;
                *(double*)(fsTx + 0x00) = 0.0;
                *(double*)(fsTx + 0x08) = 0.0;
                *(double*)(fsTx + 0x10) = 0.0;
                *(double*)(fsTx + 0x18) = 1.0;
                *(double*)(fsTx + 0x20) = x;
                *(double*)(fsTx + 0x28) = y;
                *(double*)(fsTx + 0x30) = z;
                *(double*)(fsTx + 0x40) = 1.0;
                *(double*)(fsTx + 0x48) = 1.0;
                *(double*)(fsTx + 0x50) = 1.0;
                if (fsScaleOff >= 0)
                    *(uint8_t*)(fsParams + fsScaleOff) = 1;
                if (!callProcessEvent(s_gameplayStaticsCDO, s_finishSpawnFunc, fsParams))
                    Hydro::logWarn("EngineAPI: FinishSpawningActor failed (reflection path)");
            } else {
                Hydro::logWarn("EngineAPI: FinishSpawningActor reflection lookup failed; skipping finish step");
            }
        }
        return result;
    }

    // Fallback: hardcoded UE 5.5 layout (legacy path, kept for cache-load
    // case where reflection chain isn't walked yet on this session).
    Hydro::logWarn("EngineAPI: SpawnParams reflection unavailable; using hardcoded UE 5.5 layout");
    SpawnParams sp = {};
    sp.worldContext = s_world;
    sp.actorClass = actorClass;
    sp.rotX = 0.0; sp.rotY = 0.0; sp.rotZ = 0.0; sp.rotW = 1.0; // identity quat
    sp.locX = x; sp.locY = y; sp.locZ = z;
    sp.scaleX = 1.0; sp.scaleY = 1.0; sp.scaleZ = 1.0;
    sp.collisionMethod = 1; // AlwaysSpawn (enum value 1, NOT 3)
    sp.owner = nullptr;
    sp.scaleMethod = 1; // MultiplyWithRoot
    sp.returnValue = nullptr;

    if (!callProcessEvent(s_gameplayStaticsCDO, s_spawnFunc, &sp) || !sp.returnValue) {
        Hydro::logError("EngineAPI: BeginDeferredActorSpawnFromClass failed");
        return nullptr;
    }

    // FinishSpawningActor - completes construction, registers components, dispatches BeginPlay
    if (s_finishSpawnFunc) {
        // Params: Actor(0x00), pad(0x08), FTransform(0x10-0x6F), ScaleMethod(0x70), ReturnValue(0x78)
        alignas(16) uint8_t fsParams[512] = {};
        *(void**)(fsParams + 0x00) = sp.returnValue;
        // FTransform: FQuat(0x10) + FVector Translation(0x30) + FVector Scale3D(0x50)
        *(double*)(fsParams + 0x10) = 0.0;
        *(double*)(fsParams + 0x18) = 0.0;
        *(double*)(fsParams + 0x20) = 0.0;
        *(double*)(fsParams + 0x28) = 1.0; // identity quat W
        *(double*)(fsParams + 0x30) = x;
        *(double*)(fsParams + 0x38) = y;
        *(double*)(fsParams + 0x40) = z;
        *(double*)(fsParams + 0x50) = 1.0; // Scale3D
        *(double*)(fsParams + 0x58) = 1.0;
        *(double*)(fsParams + 0x60) = 1.0;
        *(uint8_t*)(fsParams + 0x70) = 1;  // MultiplyWithRoot

        bool fsOk = callProcessEvent(s_gameplayStaticsCDO, s_finishSpawnFunc, fsParams);
        if (!fsOk)
            Hydro::logWarn("EngineAPI: FinishSpawningActor failed");
    }

    return sp.returnValue;
}

} // namespace Hydro::Engine
