#pragma once

#include "Core/Types.h"

// ============================================================
// 属性系统 — 引擎标准注解
//
// 这些定义作为 HE_ATTR_* 宏的文档参考，也为未来
// C++ 编译器原生属性支持预留接口。
//
// 当前使用方式（宏驱动）:
//   HE_REGISTER_PROPERTY(float, health)
//       HE_ATTR_CATEGORY("Stats")
//       HE_ATTR_RANGE(0.0f, 100.0f)
//
// 属性列表:
//   Category      — 编辑器分类（如 "Transform", "Rendering"）
//   DisplayName   — 编辑器显示名
//   Tooltip       — 悬停提示
//   Range         — 数值范围 (min, max)
//   Clamp         — 值钳制范围
//   ReadOnly      — 只读标记
//   Hidden        — 编辑器隐藏
//   AssetPicker   — 资产选择器（过滤器如 ".gltf"）
//   ColorWidget   — 颜色拾取器
//   Unit          — 物理单位（"cm", "degree"）
//   Deprecated    — 弃用标记 + 替代说明
//   Replicated    — 网络复制标记
//   Streaming     — 流式加载源标记
// ============================================================

namespace he::reflect {

// 标准属性键名常量
namespace AttrKey {
    constexpr const char* Category     = "Category";
    constexpr const char* DisplayName  = "DisplayName";
    constexpr const char* Tooltip      = "Tooltip";
    constexpr const char* Range        = "Range";       // 格式: "min,max"
    constexpr const char* Clamp        = "Clamp";       // 格式: "min,max"
    constexpr const char* AssetPicker  = "AssetPicker"; // 格式: ".gltf"
    constexpr const char* ColorWidget  = "ColorWidget";
    constexpr const char* Unit         = "Unit";        // 格式: "cm"
    constexpr const char* Deprecated   = "Deprecated";  // 格式: "替代说明"
    constexpr const char* SortPriority = "SortPriority";
    constexpr const char* EditCondition = "EditCondition";
}

} // namespace he::reflect
