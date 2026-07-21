#pragma once

// ============================================================
// VulkanConverters.h — RHI 跨平台类型 → Vulkan 专用类型转换函数
//
// 使用前提：调用方必须已 include Vulkan 环境头文件：
//   #define VK_USE_PLATFORM_WIN32_KHR
//   #include <vulkan/vulkan.h>
//
// D3D12/Metal 适配说明：
//   这些函数将 RHI 抽象枚举（Format、CullMode、BlendFactor 等）
//   映射到 Vulkan 专用常量。D3D12 后端需实现对应的 ToD3D12* 函数，
//   Metal 后端需实现对应的 ToMTL* 函数。
// ============================================================

#include "RHI/Types.h"

namespace he::rhi {

// --- 格式转换 ---
VkFormat    ToVkFormat(Format fmt);

// --- 深度/模板 ---
VkCompareOp         ToVkCompareOp(CompareFunc func);
VkAttachmentLoadOp  ToVkLoadOp(LoadOp op);

// --- 光栅化状态 ---
VkCullModeFlags ToVkCullMode(CullMode mode);
VkFrontFace     ToVkFrontFace(FrontFace face);
VkPolygonMode   ToVkFillMode(FillMode mode);

// --- 混合状态 ---
VkBlendFactor          ToVkBlendFactor(BlendFactor factor);
VkBlendOp              ToVkBlendOp(BlendOp op);
VkColorComponentFlags  ToVkColorWriteMask(ColorWriteMask mask);

} // namespace he::rhi
