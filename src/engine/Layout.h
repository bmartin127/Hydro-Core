#pragma once
#include <cstdint>

namespace Hydro::Engine {

// FProperty / UStruct chain-walk offsets. UE 5.1-derived hosts shift these
// by +8 bytes vs. stock UE 5.5, so even findProperty needs the discovered
// values to walk an FProperty chain at all. Stage G/H subclass offsets are
// derived from structStruct and validated via AActor::Owner.
struct PropertyLayoutCache {
    int32_t childPropsOffset = -1;
    int32_t fieldNextOffset  = -1;
    int32_t fieldNameOffset  = -1;
    int32_t offsetInternal   = -1;
    int32_t elementSize      = -1;
    int32_t flags            = -1;
    int32_t arrayInner       = -1;
    int32_t structStruct     = -1;
    int32_t objectPropClass  = -1;
    int32_t enumPropEnum     = -1;
    int32_t bytePropEnum     = -1;
    bool    initialized      = false;
    bool    succeeded        = false;
};

extern PropertyLayoutCache s_layout;
extern int s_superOffset;
extern int s_classFlagsOffset;
extern int s_enumNamesOffset;
extern int s_outerOffset;

bool     discoverPropertyLayout();
void     discoverFieldNameOffset();
void     discoverSuperOffset();
void     discoverClassFlagsOffset();
void     discoverEnumNamesOffset();
void     discoverOuterOffset();
void*    getSuper(void* ustruct);
uint32_t getClassFlags(void* cls);
void*    getOuter(void* obj);

} // namespace Hydro::Engine
