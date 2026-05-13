#pragma once

// UUserWidget::DuplicateAndInitializeFromWidgetTree resolver + call wrapper.
//
// UE 5.6 cooked WBP runtime instantiation: `widget.WidgetTree` is non-null on
// entry to `UWidgetBlueprintGeneratedClass::InitializeWidgetStatic`, so the
// gate `if (CreatedWidgetTree == nullptr)` skips D&IFWT entirely. The widget
// ends up with an empty WidgetTree (no RootWidget). Modders can't ship UI.
//
// Bypass: null `widget->WidgetTree`, then invoke D&IFWT directly to perform
// the deep-copy that the Initialize path should have done. Skips the broken
// decision chain without caring what set WidgetTree non-null.
//
// See project_wbp_init_failure memory for the diagnostic chain.

namespace Hydro::Engine {

extern void* s_duplicateAndInitFromWidgetTree;

bool findDuplicateAndInitFromWidgetTree();

// Public call wrapper - see EngineAPI.h for the canonical declaration.
bool duplicateAndInitializeWidgetTree(void* widget, void* srcWidgetTree);

} // namespace Hydro::Engine
