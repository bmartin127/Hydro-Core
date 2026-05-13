#include "FArchiveLoader.h"
#include "HydroCore.h"
#include <cstring>

namespace Hydro {

bool FArchiveLoader::s_quietProbe = false;

// Forward-decl dummy types referenced by operator<< overloads. Engine code
// won't pass real instances here - IAssetRegistry::Serialize doesn't touch
// these - we only need the type names to disambiguate vtable slots.
class FNameDummy {};
struct FTextDummy {};
class UObjectDummy {};
class FFieldDummy {};
struct FLazyObjectPtrDummy {};
struct FObjectPtrDummy {};
struct FSoftObjectPtrDummy {};
struct FSoftObjectPathDummy {};
struct FWeakObjectPtrDummy {};

FArchiveLoader::FArchiveLoader(const uint8_t* bytes, size_t len)
    // FArchiveState bitfields - order matches Archive.h L781-881.
    // ArIsLoading=1, ArIsPersistent=1: this archive reads bytes from
    // persistent storage. UE's `IsLoading()` / `IsPersistent()` are inline
    // accessors that read these bits directly via offset, so getting these
    // right is critical - engine code branches on them before going through
    // any virtual.
    : ArIsLoading(1), ArIsLoadingFromCookedPackage(0), ArIsSaving(0)
    , ArIsTransacting(0), ArIsTextFormat(0)
    , ArWantBinaryPropertySerialization(0), ArUseUnversionedPropertySerialization(0)
    , ArForceUnicode(0), ArIsPersistent(1)
    , ArIsError(0), ArIsCriticalError(0)
    , ArShouldSkipCompilingAssets(0), ArShouldSkipUpdateCustomVersion(0)
    , ArContainsCode(0), ArContainsMap(0), ArRequiresLocalizationGather(0)
    , ArForceByteSwapping(0)
    , ArIgnoreArchetypeRef(0), ArNoDelta(0), ArNoIntraPropertyDelta(0)
    , ArIgnoreOuterRef(0), ArIgnoreClassGeneratedByRef(0), ArIgnoreClassRef(0)
    , ArAllowLazyLoading(0)
    , ArIsObjectReferenceCollector(0), ArIsModifyingWeakAndStrongReferences(0)
    , ArIsCountingMemory(0), ArShouldSkipBulkData(0), ArIsFilterEditorOnly(0)
    , ArIsSaveGame(0), ArIsNetArchive(0)
    , ArUseCustomPropertyList(0), ArMergeOverrides(0), ArPreserveArrayElements(0)
    , m_data(bytes), m_size((int64_t)len), m_offset(0), m_isError(0)
{
    // CRITICAL invariant: UE's inline serialize fast path reads
    // `Ar.ActiveFPLB->{Start,End}FastPathLoadBuffer` UNCONDITIONALLY before
    // checking the cur+N > end test. So `ActiveFPLB` MUST be non-null -
    // standard practice is to point it at the FArchive's own InlineFPLB.
    // With the inline-fpl pointers all null (Start=End=Original=nullptr),
    // the fast-path check `cur+N > end` evaluates as `N > 0` → true → slow
    // path → virtual `Ar.Serialize()` → hits our override.
    //
    // Empirically validated 2026-05-09: with m_ActiveFPLB=nullptr, the
    // inline fast path AV'd at exe+0x12900C9 (`mov rcx, [r8]; lea rax,
    // [rcx+4]; cmp rax, [r8+8]`). Setting ActiveFPLB to point at our
    // InlineFPLB triple makes the fast-path check fail-fast safely.
    m_ActiveFPLB = &m_InlineFPLB_StartPtr;

    // Same invariant for CustomVersionContainer: Ar.SetCustomVersion (called
    // from SerializeVersion when reading the AR.bin header) reads
    // CustomVersionContainer and iterates its `Versions` TArray. With
    // nullptr the iteration AVs at `mov rax, [rcx]`. Point at our stub
    // (zero-init 64 bytes), which presents an empty TArray header
    // {Data=null, Num=0, Max=0} → iteration exits no-op.
    CustomVersionContainer = &m_customVersionsStub[0];
}

FArchiveLoader::~FArchiveLoader() = default;

// Per-virtual logger. m_stubHits++ AND log the function name so we can
// observe which virtuals UAR::Serialize calls. Skipped during the noisy
// vtable-slot probe (s_quietProbe=true).
#define HYDRO_STUB(NAME) do { \
    m_stubHits++; \
    if (!s_quietProbe) Hydro::logInfo("FArchiveLoader: " NAME " called"); \
} while(0)

void FArchiveLoader::rewind() {
    m_offset = 0;
    m_isError = 0;
    ArIsError = 0;
    ArIsCriticalError = 0;
    m_serializeCalls = 0;
    m_totalSizeCalls = 0;
    m_atEndCalls     = 0;
    m_seekCalls      = 0;
    m_tellCalls      = 0;
    m_stubHits       = 0;
    m_otherCalls     = 0;
}

// -- Real implementations (the small subset AR.Serialize hits) -------------

void FArchiveLoader::Serialize(void* V, int64_t Length) {
    m_serializeCalls++;
    if (!s_quietProbe && m_serializeCalls <= 10) {
        Hydro::logInfo("FArchiveLoader: Serialize#%d(len=%lld off=%lld)",
                       m_serializeCalls, (long long)Length, (long long)m_offset);
    }
    if (Length <= 0 || m_isError) return;
    if (m_offset + Length > m_size) {
        m_isError = 1;
        ArIsError = 1;
        if (!s_quietProbe) {
            Hydro::logWarn("FArchiveLoader: Serialize OOB (off=%lld len=%lld size=%lld)",
                (long long)m_offset, (long long)Length, (long long)m_size);
        }
        return;
    }
    std::memcpy(V, m_data + m_offset, (size_t)Length);
    m_offset += Length;
}

int64_t FArchiveLoader::Tell() {
    m_tellCalls++;
    if (!s_quietProbe && m_tellCalls <= 3) Hydro::logInfo("FArchiveLoader: Tell -> %lld", (long long)m_offset);
    return m_offset;
}
int64_t FArchiveLoader::TotalSize() {
    m_totalSizeCalls++;
    if (!s_quietProbe && m_totalSizeCalls <= 3) Hydro::logInfo("FArchiveLoader: TotalSize -> %lld", (long long)m_size);
    return m_size;
}
bool FArchiveLoader::AtEnd() {
    m_atEndCalls++;
    if (!s_quietProbe && m_atEndCalls <= 3) Hydro::logInfo("FArchiveLoader: AtEnd -> %d", m_offset >= m_size ? 1 : 0);
    return m_offset >= m_size;
}

void FArchiveLoader::Seek(int64_t InPos) {
    m_seekCalls++;
    if (!s_quietProbe) Hydro::logInfo("FArchiveLoader: Seek(%lld)", (long long)InPos);
    if (InPos < 0 || InPos > m_size) { m_isError = 1; ArIsError = 1; return; }
    m_offset = InPos;
}

void FArchiveLoader::SetIsLoading(bool b)    { ArIsLoading = b ? 1 : 0; }
void FArchiveLoader::SetIsPersistent(bool b) { ArIsPersistent = b ? 1 : 0; }

FStringShim FArchiveLoader::GetArchiveName() const {
    static wchar_t s_name[] = L"FArchiveLoader";
    FStringShim s;
    s.Data = s_name;
    s.ArrayNum = (int32_t)(sizeof(s_name) / sizeof(wchar_t));
    s.ArrayMax = s.ArrayNum;
    return s;
}

FArchiveLoader& FArchiveLoader::GetInnermostState() { return *this; }

// -- Stubs: bump m_stubHits, return safe defaults --------------------------
//
// During the probe, we sweep ~80 vtable slots and want each call to be
// quiet + fast. Slot fingerprint is read from per-instance counters after
// the call returns.

void  FArchiveLoader::CountBytes(size_t, size_t)                                     { HYDRO_STUB("CountBytes"); }
void* FArchiveLoader::GetLinker()                                                    { HYDRO_STUB("GetLinker"); return nullptr; }
void* FArchiveLoader::GetArchetypeFromLoader(const void*)                            { HYDRO_STUB("GetArchetypeFromLoader"); return nullptr; }
uint32_t    FArchiveLoader::EngineNetVer() const                                     { return 0; }
uint32_t    FArchiveLoader::GameNetVer()   const                                     { return 0; }
const void* FArchiveLoader::GetMigrationContext() const                              { return nullptr; }
// UE 5.6: `virtual const FCustomVersionContainer& GetCustomVersions() const`.
// Returns a REFERENCE - must not be null. Engine code dereferences immediately
// (e.g. iterates `Container.Versions` TArray). Empty stub = empty TArray = no-op
// iteration. Empirically validated 2026-05-09: returning nullptr here AV'd at
// exe+0x134EDA5 (`mov rax, [rcx]` of TArray Data) even after CustomVersionContainer
// field was wired up - the engine reads the container via this virtual, NOT the
// field, in some hot paths.
const void* FArchiveLoader::GetCustomVersions() const                                { return &m_customVersionsStub[0]; }
void  FArchiveLoader::SetCustomVersions(const void*)                                 { HYDRO_STUB("SetCustomVersions"); }
void  FArchiveLoader::ResetCustomVersions()                                          { HYDRO_STUB("ResetCustomVersions"); }
void  FArchiveLoader::SetDebugSerializationFlags(uint32_t)                           { HYDRO_STUB("SetDebugSerializationFlags"); }
void  FArchiveLoader::SetFilterEditorOnly(bool)                                      { HYDRO_STUB("SetFilterEditorOnly"); }
bool  FArchiveLoader::UseToResolveEnumerators() const                                { return false; }
bool  FArchiveLoader::ShouldSkipProperty(const void*) const                          { return false; }
void  FArchiveLoader::SetSerializedProperty(void*)                                   { HYDRO_STUB("SetSerializedProperty"); }
void  FArchiveLoader::SetSerializedPropertyChain(const void*, void*)                 { HYDRO_STUB("SetSerializedPropertyChain"); }
bool  FArchiveLoader::IsEditorOnlyPropertyOnTheStack() const                         { return false; }
void  FArchiveLoader::SetSerializeContext(void*)                                     { HYDRO_STUB("SetSerializeContext"); }
void* FArchiveLoader::GetSerializeContext()                                          { return nullptr; }
void  FArchiveLoader::SetLocalizationNamespace(const FStringShim&)                   { HYDRO_STUB("SetLocalizationNamespace"); }
FStringShim FArchiveLoader::GetLocalizationNamespace() const                         { return {}; }
void  FArchiveLoader::Reset()                                                        { HYDRO_STUB("Reset"); }
void  FArchiveLoader::SetIsLoadingFromCookedPackage(bool)                            { HYDRO_STUB("SetIsLoadingFromCookedPackage"); }
void  FArchiveLoader::SetIsSaving(bool)                                              { HYDRO_STUB("SetIsSaving"); }
void  FArchiveLoader::SetIsTransacting(bool)                                         { HYDRO_STUB("SetIsTransacting"); }
void  FArchiveLoader::SetIsTextFormat(bool)                                          { HYDRO_STUB("SetIsTextFormat"); }
void  FArchiveLoader::SetWantBinaryPropertySerialization(bool)                       { HYDRO_STUB("SetWantBinaryPropertySerialization"); }
void  FArchiveLoader::SetUseUnversionedPropertySerialization(bool)                   { HYDRO_STUB("SetUseUnversionedPropertySerialization"); }
void  FArchiveLoader::SetForceUnicode(bool)                                          { HYDRO_STUB("SetForceUnicode"); }
void  FArchiveLoader::SetUEVer(FPackageFileVersionShim)                              { HYDRO_STUB("SetUEVer"); }
void  FArchiveLoader::SetLicenseeUEVer(int32_t)                                      { HYDRO_STUB("SetLicenseeUEVer"); }
void  FArchiveLoader::SetEngineVer(const FEngineVersionBaseShim&)                    { HYDRO_STUB("SetEngineVer"); }
void  FArchiveLoader::SetEngineNetVer(uint32_t)                                      { HYDRO_STUB("SetEngineNetVer"); }
void  FArchiveLoader::SetGameNetVer(uint32_t)                                        { HYDRO_STUB("SetGameNetVer"); }

// FArchive virtuals
FArchiveLoader& FArchiveLoader::operator<<(FNameDummy&)            { HYDRO_STUB("operator<<(FName)"); return *this; }
FArchiveLoader& FArchiveLoader::operator<<(FTextDummy&)            { HYDRO_STUB("operator<<(FText)"); return *this; }
FArchiveLoader& FArchiveLoader::operator<<(UObjectDummy*&)         { HYDRO_STUB("operator<<(UObject*)"); return *this; }
FArchiveLoader& FArchiveLoader::operator<<(FFieldDummy*&)          { HYDRO_STUB("operator<<(FField*)"); return *this; }
FArchiveLoader& FArchiveLoader::operator<<(FLazyObjectPtrDummy&)   { HYDRO_STUB("operator<<(FLazyObjectPtr)"); return *this; }
FArchiveLoader& FArchiveLoader::operator<<(FObjectPtrDummy&)       { HYDRO_STUB("operator<<(FObjectPtr)"); return *this; }
FArchiveLoader& FArchiveLoader::operator<<(FSoftObjectPtrDummy&)   { HYDRO_STUB("operator<<(FSoftObjectPtr)"); return *this; }
FArchiveLoader& FArchiveLoader::operator<<(FSoftObjectPathDummy&)  { HYDRO_STUB("operator<<(FSoftObjectPath)"); return *this; }
FArchiveLoader& FArchiveLoader::operator<<(FWeakObjectPtrDummy&)   { HYDRO_STUB("operator<<(FWeakObjectPtr)"); return *this; }
void  FArchiveLoader::ForceBlueprintFinalization()                                   { HYDRO_STUB("ForceBlueprintFinalization"); }
void  FArchiveLoader::SerializeBool(bool& D)                                         { m_otherCalls++; Serialize(&D, 1); }
void  FArchiveLoader::SerializeBits(void* V, int64_t LengthBits)                     { m_otherCalls++; Serialize(V, (LengthBits + 7) / 8); }
void  FArchiveLoader::SerializeInt(uint32_t& Value, uint32_t)                        { m_otherCalls++; Serialize(&Value, sizeof(uint32_t)); }
void  FArchiveLoader::SerializeIntPacked(uint32_t&)                                  { m_otherCalls++; }
void  FArchiveLoader::SerializeIntPacked64(uint64_t&)                                { m_otherCalls++; }
void  FArchiveLoader::Preload(void*)                                                 { HYDRO_STUB("Preload"); }
void  FArchiveLoader::AttachBulkData_A(void*, void*)                                 { HYDRO_STUB("AttachBulkData_A"); }
void  FArchiveLoader::AttachBulkData_B(void*)                                        { HYDRO_STUB("AttachBulkData_B"); }
void  FArchiveLoader::DetachBulkData_A(void*, bool)                                  { HYDRO_STUB("DetachBulkData_A"); }
void  FArchiveLoader::DetachBulkData_B(void*, bool)                                  { HYDRO_STUB("DetachBulkData_B"); }
bool  FArchiveLoader::SerializeBulkData(void*, const void*)                          { HYDRO_STUB("SerializeBulkData"); return false; }
bool  FArchiveLoader::IsProxyOf(FArchiveLoader*) const                               { return false; }
bool  FArchiveLoader::Precache(int64_t, int64_t)                                     { return false; }
void  FArchiveLoader::FlushCache()                                                   { HYDRO_STUB("FlushCache"); }
bool  FArchiveLoader::SetCompressionMap(void*, uint32_t)                             { return false; }
void  FArchiveLoader::Flush()                                                        { HYDRO_STUB("Flush"); }
bool  FArchiveLoader::Close()                                                        { return true; }
void  FArchiveLoader::MarkScriptSerializationStart(const void*)                      { HYDRO_STUB("MarkScriptSerializationStart"); }
void  FArchiveLoader::MarkScriptSerializationEnd(const void*)                        { HYDRO_STUB("MarkScriptSerializationEnd"); }
void  FArchiveLoader::MarkSearchableName(const void*, const void*) const             { /* const, can't bump */ }
void  FArchiveLoader::UsingCustomVersion(const void*)                                { HYDRO_STUB("UsingCustomVersion"); }
FArchiveLoader* FArchiveLoader::GetCacheableArchive()                                { return this; }
void  FArchiveLoader::PushSerializedProperty(void*, const bool)                      { HYDRO_STUB("PushSerializedProperty"); }
void  FArchiveLoader::PopSerializedProperty(void*, const bool)                       { HYDRO_STUB("PopSerializedProperty"); }
bool  FArchiveLoader::AttachExternalReadDependency(void*)                            { return false; }
void  FArchiveLoader::PushDebugDataString(const void*)                               { HYDRO_STUB("PushDebugDataString"); }
void  FArchiveLoader::PopDebugDataString()                                           { HYDRO_STUB("PopDebugDataString"); }
void  FArchiveLoader::PushFileRegionType(uint8_t)                                    { HYDRO_STUB("PushFileRegionType"); }
void  FArchiveLoader::PopFileRegionType()                                            { HYDRO_STUB("PopFileRegionType"); }

} // namespace Hydro
