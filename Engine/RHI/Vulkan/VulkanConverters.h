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

// 颜色格式是否属于"终端输出"（交换链/HDR10）——决定 RenderPass 颜色附件的 finalLayout：
//   true  → VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
//   false → VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
// 这是 RHI 里**唯一**的判定规则：VulkanPipeline 建 RenderPass 时用它，
// VulkanCommandList 在 render pass 边界回写布局追踪器时也用它（见 TextureLayoutTracker.h）。
inline bool UsesPresentSrcFinalLayout(Format fmt) {
    return fmt == Format::BGRA8_UNORM || fmt == Format::BGRA8_SRGB
        || fmt == Format::A2B10G10R10_UNORM_PACK32;
}

// --- 纹理使用标志转换 ---
VkImageUsageFlags ToVkImageUsage(TextureUsage usage);

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
