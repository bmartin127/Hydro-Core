#pragma once
#include <cstdint>
#include <vector>
#include <utility>

namespace Hydro::Engine {

// Per-UFunction parameter layout derived from the FProperty chain.
// ParmsSize = max(Offset_Internal + ElementSize) across all params.
// RetOffset = Offset_Internal of the CPF_ReturnParm property.
// If the chain walk yields nothing (native UFunction with no reflected params,
// or Layer 2 not yet up), derived=false and the fallback raw reads are used.
struct UFuncLayoutCache {
    uint16_t parmsSize = 0;
    uint16_t retOffset = 0;
    bool     hasReturn = false;
    bool     derived   = false;
};

void*    findFunctionOnClass(void* uclass, const wchar_t* funcName);
void*    findFunction(void* uclass, const wchar_t* funcName);
void*    findProperty(void* uclass, const wchar_t* propName);
uint64_t getPropertyFlags(void* prop);
void*    getArrayInner(void* arrayProp);
void*    getStructStruct(void* structProp);
uint16_t getUFunctionParmsSize(void* ufunc);
uint16_t getUFunctionRetOffset(void* ufunc);
uint32_t modifyClassFlags(void* cls, uint32_t setMask, uint32_t clearMask);
int      readEnumNames(void* uenum, std::vector<std::pair<uint32_t, int64_t>>& out);

// Called when Layer 2 offsets are updated so stale cached layouts are evicted.
void clearUFuncLayoutCache();

} // namespace Hydro::Engine
