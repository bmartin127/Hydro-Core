#pragma once

#include <cstdint>
#include <cstddef>

// FArchiveLoader - minimal FArchive subclass for runtime AR.bin merging via
// IAssetRegistry::Serialize(FArchive&) vtable dispatch.
//
// Layout MUST mirror UE 5.6 `FArchive : private FArchiveState` exactly:
//   - Field declaration order matches `Engine/Source/Runtime/Core/Public/Serialization/Archive.h`
//     so engine code that reads bitfields directly (e.g. `IsLoading()` is an
//     inline accessor for `ArIsLoading`) hits the right offset.
//   - Virtual method declaration order matches Archive.h so the compiler-emitted
//     vtable's slot[N] equals UE's vtable slot[N] for the same method.
//
// Target build config: WITH_EDITOR=0 (shipping), USE_STABLE_LOCALIZATION_KEYS=0,
// WITH_VERSE_VM=0 (DMG / SN2 don't use Verse), UE_WITH_REMOTE_OBJECT_HANDLE=0.
// Gated fields/virtuals (ArDebugSerializationFlags, LocalizationNamespacePtr,
// VCell operator<<, ArIsMigratingRemoteObjects) are skipped accordingly.
//
// 5.6 deltas vs 5.5 (verified against C:/ue6/UE_5.6/Engine/Source/Runtime/Core/Public/Serialization/Archive.h):
//   - Adds bitfield ArPreserveArrayElements (always present)
//   - Adds virtual GetMigrationContext (always present in vtable)
//   - Adds virtual SetDebugSerializationFlags (always present in vtable;
//     body is WITH_EDITOR-gated but the slot exists either way)
//
// Most virtuals are stubbed - they bump per-instance counters so the vtable-slot
// probe can fingerprint each candidate. Real implementations target the small
// subset that `IAssetRegistry::Serialize` actually invokes during AR.bin
// deserialization: Tell/Seek/Serialize/TotalSize/AtEnd/SetIsLoading/
// SetIsPersistent/GetArchiveName.

namespace Hydro {

// FString shape used by GetArchiveName return values. Trivial - owns nothing
// because Num=0 tells UE there's no allocation to free.
struct FStringShim {
    wchar_t* Data = nullptr;
    int32_t  ArrayNum = 0;
    int32_t  ArrayMax = 0;
};

// FPackageFileVersion = { int32 FileVersionUE4; int32 FileVersionUE5; }
struct FPackageFileVersionShim {
    int32_t FileVersionUE4 = 0;
    int32_t FileVersionUE5 = 0;
};

// FEngineVersionBase = { uint16 Major; uint16 Minor; uint16 Patch; uint32 Changelist; }
struct FEngineVersionBaseShim {
    uint16_t Major = 0;
    uint16_t Minor = 0;
    uint16_t Patch = 0;
    uint32_t Changelist = 0;
};

class FArchiveLoader {
public:
    FArchiveLoader(const uint8_t* bytes, size_t len);
    virtual ~FArchiveLoader();

    // -- FArchiveState virtuals (in Archive.h declaration order) ------------
    virtual FArchiveLoader& GetInnermostState();
    virtual void CountBytes(size_t InNum, size_t InMax);
    virtual FStringShim GetArchiveName() const;
    virtual void* GetLinker();
    virtual int64_t Tell();
    virtual int64_t TotalSize();
    virtual bool AtEnd();
    virtual void* GetArchetypeFromLoader(const void* Obj);
    virtual uint32_t EngineNetVer() const;
    virtual uint32_t GameNetVer() const;
    virtual const void* GetMigrationContext() const;          // 5.6 NEW (slot 11)
    virtual const void* GetCustomVersions() const;
    virtual void SetCustomVersions(const void* InCustomVersionContainer);
    virtual void ResetCustomVersions();
    virtual void SetDebugSerializationFlags(uint32_t InFlags);// 5.6 NEW (slot 15)
    virtual void SetFilterEditorOnly(bool InFilterEditorOnly);
    virtual bool UseToResolveEnumerators() const;
    virtual bool ShouldSkipProperty(const void* InProperty) const;
    virtual void SetSerializedProperty(void* InProperty);
    virtual void SetSerializedPropertyChain(const void* InChain, void* InOverride);
    virtual bool IsEditorOnlyPropertyOnTheStack() const;
    virtual void SetSerializeContext(void* InLoadContext);
    virtual void* GetSerializeContext();
    virtual void SetLocalizationNamespace(const FStringShim& InNamespace);
    virtual FStringShim GetLocalizationNamespace() const;
    virtual void Reset();
    virtual void SetIsLoading(bool bInIsLoading);
    virtual void SetIsLoadingFromCookedPackage(bool b);
    virtual void SetIsSaving(bool b);
    virtual void SetIsTransacting(bool b);
    virtual void SetIsTextFormat(bool b);
    virtual void SetWantBinaryPropertySerialization(bool b);
    virtual void SetUseUnversionedPropertySerialization(bool b);
    virtual void SetForceUnicode(bool b);
    virtual void SetIsPersistent(bool b);
    virtual void SetUEVer(FPackageFileVersionShim InVer);
    virtual void SetLicenseeUEVer(int32_t InVer);
    virtual void SetEngineVer(const FEngineVersionBaseShim& InVer);
    virtual void SetEngineNetVer(uint32_t InEngineNetVer);
    virtual void SetGameNetVer(uint32_t InGameNetVer);

    // -- FArchive virtuals (continue Archive.h declaration order) -----------
    virtual FArchiveLoader& operator<<(class FNameDummy& Value);
    virtual FArchiveLoader& operator<<(struct FTextDummy& Value);
    virtual FArchiveLoader& operator<<(class UObjectDummy*& Value);
    virtual FArchiveLoader& operator<<(class FFieldDummy*& Value);
    virtual FArchiveLoader& operator<<(struct FLazyObjectPtrDummy& Value);
    virtual FArchiveLoader& operator<<(struct FObjectPtrDummy& Value);
    virtual FArchiveLoader& operator<<(struct FSoftObjectPtrDummy& Value);
    virtual FArchiveLoader& operator<<(struct FSoftObjectPathDummy& Value);
    virtual FArchiveLoader& operator<<(struct FWeakObjectPtrDummy& Value);
    virtual void ForceBlueprintFinalization();
    virtual void SerializeBool(bool& D);
    virtual void Serialize(void* V, int64_t Length);
    virtual void SerializeBits(void* V, int64_t LengthBits);
    virtual void SerializeInt(uint32_t& Value, uint32_t Max);
    virtual void SerializeIntPacked(uint32_t& Value);
    virtual void SerializeIntPacked64(uint64_t& Value);
    virtual void Preload(void* Object);
    virtual void Seek(int64_t InPos);
    virtual void AttachBulkData_A(void* Owner, void* BulkData);
    virtual void AttachBulkData_B(void* BulkData);
    virtual void DetachBulkData_A(void* BulkData, bool bEnsure);
    virtual void DetachBulkData_B(void* BulkData, bool bEnsure);
    virtual bool SerializeBulkData(void* BulkData, const void* Params);
    virtual bool IsProxyOf(FArchiveLoader* InOther) const;
    virtual bool Precache(int64_t PrecacheOffset, int64_t PrecacheSize);
    virtual void FlushCache();
    virtual bool SetCompressionMap(void* CompressedChunks, uint32_t CompressionFlags);
    virtual void Flush();
    virtual bool Close();
    virtual void MarkScriptSerializationStart(const void* Obj);
    virtual void MarkScriptSerializationEnd(const void* Obj);
    virtual void MarkSearchableName(const void* TypeObject, const void* ValueName) const;
    virtual void UsingCustomVersion(const void* Guid);
    virtual FArchiveLoader* GetCacheableArchive();
    virtual void PushSerializedProperty(void* InProperty, const bool bIsEditorOnly);
    virtual void PopSerializedProperty(void* InProperty, const bool bIsEditorOnly);
    virtual bool AttachExternalReadDependency(void* ReadCallback);
    virtual void PushDebugDataString(const void* DebugData);
    virtual void PopDebugDataString();
    virtual void PushFileRegionType(uint8_t Type);
    virtual void PopFileRegionType();

    // -- Probe instrumentation accessors ------------------------------------
    /// Reset offset, error state, and all counters between probe attempts.
    void rewind();
    /// Whether Serialize ever ran out of bytes (offset+count > size).
    bool errored() const { return m_isError != 0; }
    /// Bytes read so far (= Tell()). Probe heuristic: real AR.Serialize only
    /// advances offset when the archive has bytes; with empty bytes the call
    /// trips ArIsError on the first read attempt.
    int64_t getBytesRead() const { return m_offset; }

    /// Per-instance call counts. The right vtable slot is fingerprinted by
    /// these - IAssetRegistry::Serialize calls Serialize() at least once and
    /// typically TotalSize/Tell to validate the archive. Slots that don't
    /// take FArchive& won't touch any of these.
    int getSerializeCalls()  const { return m_serializeCalls; }
    int getTotalSizeCalls()  const { return m_totalSizeCalls; }
    int getAtEndCalls()      const { return m_atEndCalls; }
    int getSeekCalls()       const { return m_seekCalls; }
    int getTellCalls()       const { return m_tellCalls; }
    int getStubHits()        const { return m_stubHits; }
    int getOtherCalls()      const { return m_otherCalls; }

private:
    // -- DEVIRTUALIZE_FLinkerLoad_Serialize fields (Archive.h L760-770)
    //
    // CRITICAL: in UE 5.6 shipping (WITH_EDITORONLY_DATA=0), the macro
    // DEVIRTUALIZE_FLinkerLoad_Serialize = !WITH_EDITORONLY_DATA = 1 is
    // active, which inserts these 32 bytes of fast-path-load-buffer state
    // INTO FArchiveState BEFORE the bitfields. If we don't mirror this,
    // engine code that reads `Ar.IsLoading()` (an inline bitfield read at
    // a fixed offset from the FArchive base) lands ~32 bytes off and gets
    // garbage, causing FAssetRegistryState::Load to early-return false
    // without touching the archive at all. Verified empirically 2026-05-09
    // on DMG@5.6: shim hit count dropped to 0 across all counters after
    // Load returned false.
    void* m_ActiveFPLB                  = nullptr;  // FFastPathLoadBuffer*
    void* m_InlineFPLB_StartPtr         = nullptr;  // FFastPathLoadBuffer.StartFastPathLoadBuffer
    void* m_InlineFPLB_EndPtr           = nullptr;  // .EndFastPathLoadBuffer
    void* m_InlineFPLB_OriginalPtr      = nullptr;  // .OriginalFastPathLoadBuffer

    // -- FArchiveState fields (Archive.h L809-1086) - exact declaration order
    //
    // First 33 single-bit flags. UE compiles these as a sequence of `uint8 X : 1;`
    // on MSVC each `uint8 ... : 1` opens a fresh storage unit by default UNLESS
    // followed by another bitfield of the same underlying type - then they pack.
    // UE keeps them all `uint8 : 1` so MSVC packs the whole run into 5 bytes
    // (8 bits per byte * 5 = 40, easily fits 33).
    uint8_t  ArIsLoading                            : 1;  // L781
    uint8_t  ArIsLoadingFromCookedPackage           : 1;  // L784
    uint8_t  ArIsSaving                             : 1;  // L787
    uint8_t  ArIsTransacting                        : 1;  // L790
    uint8_t  ArIsTextFormat                         : 1;  // L793
    uint8_t  ArWantBinaryPropertySerialization      : 1;  // L796
    uint8_t  ArUseUnversionedPropertySerialization  : 1;  // L799
    uint8_t  ArForceUnicode                         : 1;  // L802
    uint8_t  ArIsPersistent                         : 1;  // L805
    uint8_t  ArIsError                              : 1;  // L809
    uint8_t  ArIsCriticalError                      : 1;  // L812
    uint8_t  ArShouldSkipCompilingAssets            : 1;  // L815
    uint8_t  ArShouldSkipUpdateCustomVersion        : 1;  // L820
    uint8_t  ArContainsCode                         : 1;  // L824
    uint8_t  ArContainsMap                          : 1;  // L827
    uint8_t  ArRequiresLocalizationGather           : 1;  // L830
    uint8_t  ArForceByteSwapping                    : 1;  // L833
    uint8_t  ArIgnoreArchetypeRef                   : 1;  // L836
    uint8_t  ArNoDelta                              : 1;  // L839
    uint8_t  ArNoIntraPropertyDelta                 : 1;  // L842
    uint8_t  ArIgnoreOuterRef                       : 1;  // L845
    uint8_t  ArIgnoreClassGeneratedByRef            : 1;  // L848
    uint8_t  ArIgnoreClassRef                       : 1;  // L851
    uint8_t  ArAllowLazyLoading                     : 1;  // L854
    uint8_t  ArIsObjectReferenceCollector           : 1;  // L857
    uint8_t  ArIsModifyingWeakAndStrongReferences   : 1;  // L860
    uint8_t  ArIsCountingMemory                     : 1;  // L863
    uint8_t  ArShouldSkipBulkData                   : 1;  // L866
    uint8_t  ArIsFilterEditorOnly                   : 1;  // L869
    uint8_t  ArIsSaveGame                           : 1;  // L872
    uint8_t  ArIsNetArchive                         : 1;  // L875
    uint8_t  ArUseCustomPropertyList                : 1;  // L912
    uint8_t  ArMergeOverrides                       : 1;  // L915
    uint8_t  ArPreserveArrayElements                : 1;  // L918 (5.6 NEW)

    int32_t  ArSerializingDefaults    = 0;  // L884
    uint32_t ArPortFlags              = 0;  // L887
    int64_t  ArMaxSerializeSize       = 0;  // L890

    FPackageFileVersionShim ArUEVer;            // L988
    int32_t                 ArLicenseeUEVer = 0;// L991
    FEngineVersionBaseShim  ArEngineVer;        // L994

    void*       CustomVersionContainer = nullptr;   // L1001 (FCustomVersionContainer*)
    const void* ArCustomPropertyList   = nullptr;   // L1005 (FCustomPropertyListNode*)

    // #if WITH_EDITOR ArDebugSerializationFlags - OFF in shipping, omitted

    void* SavePackageData          = nullptr;       // L1018 (FArchiveSavePackageData*)
    void* SerializedProperty       = nullptr;       // L1021 (FProperty*)
    void* SerializedPropertyChain  = nullptr;       // L1024 (FArchiveSerializedPropertyChain*)

    // #if USE_STABLE_LOCALIZATION_KEYS LocalizationNamespacePtr - OFF in shipping, omitted

    bool  bCustomVersionsAreReset  = false;         // L1045
    void* NextProxy                = nullptr;       // L1049 (FArchiveState*)

    // Stub FCustomVersionContainer: empty TArray<FCustomVersion>{Data=null,
    // Num=0, Max=0}. The engine's `Ar.SetCustomVersion` (called from
    // SerializeVersion) reads CustomVersionContainer and iterates its
    // `Versions` TArray. With nullptr the iteration helper AVs immediately
    // at `mov rax, [rcx]`. Pointing CustomVersionContainer here gives a
    // valid empty array → cmp cur,end equal → loop exits no-op.
    // Size = 16 bytes (TArray header) + a few extra for forward-compat.
    alignas(8) uint8_t m_customVersionsStub[64] = {};

    // (FArchive : private FArchiveState in 5.5 declares no additional non-virtual
    // data members beyond what FArchiveState has - only more virtuals.)

    // -- Our subclass state (after the FArchiveState/FArchive layout) --
    const uint8_t* m_data;
    int64_t        m_size;
    int64_t        m_offset;
    int            m_isError;  // separate from ArIsError so probe can inspect

    // -- Probe instrumentation (per-instance, reset by rewind()) ---------
    int m_serializeCalls = 0;
    int m_totalSizeCalls = 0;
    int m_atEndCalls     = 0;
    int m_seekCalls      = 0;
    int m_tellCalls      = 0;
    int m_stubHits       = 0;  // every other virtual increments this
    int m_otherCalls     = 0;  // SerializeBits/Bool/Int/IntPacked/Bulk paths

    // Helper: bump m_stubHits without log spam during probe. The probe sweeps
    // ~80 slots; logging every stub hit drowns the diagnostic. Probe-mode is
    // controlled by the caller via setQuietProbe(true).
    static bool s_quietProbe;
public:
    static void setQuietProbe(bool quiet) { s_quietProbe = quiet; }
    static bool isQuietProbe() { return s_quietProbe; }
};

} // namespace Hydro
