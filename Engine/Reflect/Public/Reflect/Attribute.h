#pragma once

#include "Core/Types.h"

// ============================================================
// [[engine::...]] attribute definitions
//
// These define the standard attributes available for marking
// engine types and properties. In C++26, these will be real
// custom attributes parsed by the compiler. Until then, we
// define them as empty struct tags for documentation and
// future compatibility.
//
// Usage:
//   struct [[engine::component]]
//          [[engine::display_name("Point Light")]]
//          PointLightComponent { ... };
// ============================================================

namespace he::attribute {

// --- Type-level attributes ---

// Marks a type as an engine component
struct component {};

// Marks a type as an engine resource/asset
struct resource {};

// --- Property attributes ---

// Editor display name override
template<const char Name[]>
struct display_name {};

// Editor category grouping
template<const char Category[]>
struct category {};

// Tooltip text
template<const char Text[]>
struct tooltip {};

// Numeric range (min, max)
struct range {
    float min;
    float max;
};

// Clamp range
struct clamp {
    float min;
    float max;
};

// Display as slider
struct slider {};

// Step size for numeric input
struct step {
    float value;
};

// Read-only in editor
struct read_only {};

// Conditionally editable
template<const char Condition[]>
struct edit_condition {};

// Hidden from editor
struct hide_in_editor {};

// Asset picker filter
template<const char Filter[]>
struct asset_picker {};

// Use color widget
struct color_widget {};

// Physical unit
template<const char Unit[]>
struct unit {};

// Sort priority (lower = earlier)
struct sort_priority {
    int value;
};

// Mark as deprecated
template<const char Reason[]>
struct deprecated {};

// Renamed from a previous version
template<const char OldName[]>
struct renamed_from {};

// Network replicated
struct replicated {};

// Streaming source marker
struct streaming_source {};

// Console variable binding
struct cvar {};

// Dependency on another asset
struct depends_on_asset {};

// Required component dependency
template<typename T>
struct require {};

// Optional component dependency
template<typename T>
struct optional_component {};

// Update order hint
struct update_order {
    int value;
};

} // namespace he::attribute
