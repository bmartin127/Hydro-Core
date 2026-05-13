#pragma once

// StaticDuplicateObject discovery + call wrapper.
//
// UE 5.6 cooked WBP runtime instantiation requires deep-copying the class
// WidgetTree into the widget instance. The standard UMG init path uses
// NewObject-with-template + an InstancingGraph filter that skips
// `UWidgetTree::RootWidget` (no `CPF_InstancedReference`), leaving the
// instance tree empty. StaticDuplicateObject bypasses that - archive-based
// serialization writes every UPROPERTY regardless of Instanced/Transient.
//
// See project_wbp_init_failure memory for the diagnostic chain.

namespace Hydro::Engine {

extern void* s_staticDuplicateObjectEx; // StaticDuplicateObjectEx(FObjectDuplicationParameters&)
extern void* s_staticDuplicateObject;   // 7-arg public wrapper (may be inlined away by LTO)

bool findStaticDuplicateObject();

// Public callable wrapper - see EngineAPI.h for the canonical declaration.
void* staticDuplicateObject(void* source, void* outer);

} // namespace Hydro::Engine
