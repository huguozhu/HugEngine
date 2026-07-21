// ============================================================
// VulkanConverters.cpp — RHI 跨平台类型 → Vulkan 专用类型转换实现
// ============================================================

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include "VulkanConverters.h"

namespace he::rhi {

// ============================================================
// 格式转换
// ============================================================
VkFormat ToVkFormat(Format fmt) {
    switch (fmt) {
        // 8-bit 颜色
        case Format::R8_UNORM:       return VK_FORMAT_R8_UNORM;
        case Format::R8_SRGB:        return VK_FORMAT_R8_SRGB;
        case Format::RG8_UNORM:      return VK_FORMAT_R8G8_UNORM;
        case Format::RG8_SRGB:       return VK_FORMAT_R8G8_SRGB;
        case Format::RGBA8_UNORM:    return VK_FORMAT_R8G8B8A8_UNORM;
        case Format::RGBA8_SRGB:     return VK_FORMAT_R8G8B8A8_SRGB;
        case Format::BGRA8_UNORM:    return VK_FORMAT_B8G8R8A8_UNORM;
        case Format::BGRA8_SRGB:     return VK_FORMAT_B8G8R8A8_SRGB;
        // 16-bit 浮点
        case Format::R16_FLOAT:      return VK_FORMAT_R16_SFLOAT;
        case Format::RG16_FLOAT:     return VK_FORMAT_R16G16_SFLOAT;
        case Format::RGBA16_FLOAT:   return VK_FORMAT_R16G16B16A16_SFLOAT;
        // 32-bit 浮点
        case Format::R32_FLOAT:      return VK_FORMAT_R32_SFLOAT;
        case Format::RG32_FLOAT:     return VK_FORMAT_R32G32_SFLOAT;
        case Format::RGBA32_FLOAT:   return VK_FORMAT_R32G32B32A32_SFLOAT;
        // 特殊
        case Format::R11G11B10_FLOAT: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
        // 深度/模板
        case Format::D16_UNORM:           return VK_FORMAT_D16_UNORM;
        case Format::D32_FLOAT:           return VK_FORMAT_D32_SFLOAT;
        case Format::D24_UNORM_S8_UINT:   return VK_FORMAT_D24_UNORM_S8_UINT;
        case Format::D32_FLOAT_S8_UINT:   return VK_FORMAT_D32_SFLOAT_S8_UINT;
        // BC 压缩
        case Format::BC1_UNORM:      return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
        case Format::BC3_UNORM:      return VK_FORMAT_BC3_UNORM_BLOCK;
        case Format::BC4_UNORM:      return VK_FORMAT_BC4_UNORM_BLOCK;
        case Format::BC5_UNORM:      return VK_FORMAT_BC5_UNORM_BLOCK;
        case Format::BC7_UNORM:      return VK_FORMAT_BC7_UNORM_BLOCK;
        default:                     return VK_FORMAT_UNDEFINED;
    }
}

// ============================================================
// 深度/模板
// ============================================================
VkCompareOp ToVkCompareOp(CompareFunc func) {
    switch (func) {
        case CompareFunc::Never:        return VK_COMPARE_OP_NEVER;
        case CompareFunc::Less:         return VK_COMPARE_OP_LESS;
        case CompareFunc::Equal:        return VK_COMPARE_OP_EQUAL;
        case CompareFunc::LessEqual:    return VK_COMPARE_OP_LESS_OR_EQUAL;
        case CompareFunc::Greater:      return VK_COMPARE_OP_GREATER;
        case CompareFunc::NotEqual:     return VK_COMPARE_OP_NOT_EQUAL;
        case CompareFunc::GreaterEqual: return VK_COMPARE_OP_GREATER_OR_EQUAL;
        case CompareFunc::Always:       return VK_COMPARE_OP_ALWAYS;
        default:                        return VK_COMPARE_OP_NEVER;
    }
}

VkAttachmentLoadOp ToVkLoadOp(LoadOp op) {
    switch (op) {
        case LoadOp::Clear:  return VK_ATTACHMENT_LOAD_OP_CLEAR;
        case LoadOp::Load:   return VK_ATTACHMENT_LOAD_OP_LOAD;
        default:             return VK_ATTACHMENT_LOAD_OP_CLEAR;
    }
}

// ============================================================
// 光栅化状态
// ============================================================
VkCullModeFlags ToVkCullMode(CullMode mode) {
    switch (mode) {
        case CullMode::None:         return VK_CULL_MODE_NONE;
        case CullMode::Front:        return VK_CULL_MODE_FRONT_BIT;
        case CullMode::Back:         return VK_CULL_MODE_BACK_BIT;
        case CullMode::FrontAndBack: return VK_CULL_MODE_FRONT_AND_BACK;
        default:                     return VK_CULL_MODE_NONE;
    }
}

VkFrontFace ToVkFrontFace(FrontFace face) {
    switch (face) {
        case FrontFace::Clockwise:        return VK_FRONT_FACE_CLOCKWISE;
        case FrontFace::CounterClockwise: return VK_FRONT_FACE_COUNTER_CLOCKWISE;
        default:                          return VK_FRONT_FACE_CLOCKWISE;
    }
}

VkPolygonMode ToVkFillMode(FillMode mode) {
    switch (mode) {
        case FillMode::Solid:     return VK_POLYGON_MODE_FILL;
        case FillMode::Wireframe: return VK_POLYGON_MODE_LINE;
        default:                  return VK_POLYGON_MODE_FILL;
    }
}

// ============================================================
// 混合状态
// ============================================================
VkBlendFactor ToVkBlendFactor(BlendFactor factor) {
    switch (factor) {
        case BlendFactor::Zero:             return VK_BLEND_FACTOR_ZERO;
        case BlendFactor::One:              return VK_BLEND_FACTOR_ONE;
        case BlendFactor::SrcColor:         return VK_BLEND_FACTOR_SRC_COLOR;
        case BlendFactor::OneMinusSrcColor: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case BlendFactor::SrcAlpha:         return VK_BLEND_FACTOR_SRC_ALPHA;
        case BlendFactor::OneMinusSrcAlpha: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case BlendFactor::DstColor:         return VK_BLEND_FACTOR_DST_COLOR;
        case BlendFactor::OneMinusDstColor: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
        case BlendFactor::DstAlpha:         return VK_BLEND_FACTOR_DST_ALPHA;
        case BlendFactor::OneMinusDstAlpha: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        default:                            return VK_BLEND_FACTOR_ONE;
    }
}

VkBlendOp ToVkBlendOp(BlendOp op) {
    switch (op) {
        case BlendOp::Add:             return VK_BLEND_OP_ADD;
        case BlendOp::Subtract:        return VK_BLEND_OP_SUBTRACT;
        case BlendOp::ReverseSubtract: return VK_BLEND_OP_REVERSE_SUBTRACT;
        case BlendOp::Min:             return VK_BLEND_OP_MIN;
        case BlendOp::Max:             return VK_BLEND_OP_MAX;
        default:                       return VK_BLEND_OP_ADD;
    }
}

VkColorComponentFlags ToVkColorWriteMask(ColorWriteMask mask) {
    VkColorComponentFlags flags = 0;
    if (u8(mask) & u8(ColorWriteMask::Red))   flags |= VK_COLOR_COMPONENT_R_BIT;
    if (u8(mask) & u8(ColorWriteMask::Green)) flags |= VK_COLOR_COMPONENT_G_BIT;
    if (u8(mask) & u8(ColorWriteMask::Blue))  flags |= VK_COLOR_COMPONENT_B_BIT;
    if (u8(mask) & u8(ColorWriteMask::Alpha)) flags |= VK_COLOR_COMPONENT_A_BIT;
    return flags;
}

} // namespace he::rhi
