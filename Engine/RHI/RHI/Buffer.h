#pragma once

#include "RHI/Types.h"
#include "Core/Types.h"

#include <span>

// ============================================================
// RHI Buffer & Texture creation
// ============================================================

namespace he::rhi {

// --- Buffer descriptor ---
struct BufferDesc {
    usize       size        = 0;          // Size in bytes
    BufferUsage usage       = BufferUsage::None;
    bool        cpuAccess   = false;      // Mappable from CPU
    const void* initialData = nullptr;    // Optional initial upload
    usize       stride      = 0;          // For vertex/index buffers
};

// --- Texture descriptor ---
struct TextureDesc {
    u32         width       = 1;
    u32         height      = 1;
    u32         depth       = 1;
    u32         mipLevels   = 1;
    u32         arrayLayers = 1;
    u32         sampleCount = 1;     // MSAA 采样数（1=无 MSAA, 2/4/8）
    Format      format      = Format::RGBA8_UNORM;
    TextureUsage usage      = TextureUsage::ShaderResource;
    const void* initialData = nullptr;
};

// --- GPU buffer handle ---
class IRHIBuffer {
public:
    virtual ~IRHIBuffer() = default;

    virtual usize  GetSize()  const = 0;
    virtual void*  Map()            = 0;  // Map for CPU read/write
    virtual void   Unmap()          = 0;
    virtual u64    GetDeviceAddress() const = 0; // For bindless
};

// --- GPU texture handle ---
class IRHITexture {
public:
    virtual ~IRHITexture() = default;

    virtual u32    GetWidth()  const = 0;
    virtual u32    GetHeight() const = 0;
    virtual u32    GetDepth()  const = 0;
    virtual u32    GetMipLevels()   const = 0;
    virtual u32    GetArrayLayers() const = 0;
    virtual Format GetFormat() const = 0;

    // 原生资源句柄（后端特定：Vulkan=VkImageView, 空=未初始化）
    virtual void*  GetNativeHandle() const = 0;          // ImageView 句柄（默认面/层）
    virtual void*  GetNativeHandle(u32 index) const = 0; // 逐面/逐层句柄（Cubemap 面 0-5）
};

// --- Sampler descriptor ---
struct SamplerDesc {
    FilterMode  minFilter     = FilterMode::Linear;   // 缩小过滤
    FilterMode  magFilter     = FilterMode::Linear;   // 放大过滤
    FilterMode  mipFilter     = FilterMode::Linear;   // Mipmap 过滤
    AddressMode addressU      = AddressMode::Repeat;  // U 方向寻址
    AddressMode addressV      = AddressMode::Repeat;  // V 方向寻址
    AddressMode addressW      = AddressMode::Repeat;  // W 方向寻址
    float       mipLodBias    = 0.0f;   // Mipmap LOD 偏移
    float       minLod        = 0.0f;   // 最小 LOD
    float       maxLod        = 16.0f;  // 最大 LOD
    u32         maxAnisotropy = 1;      // 各向异性过滤级别（1=禁用）
    CompareFunc compareFunc   = CompareFunc::Never;  // 深度比较函数
    bool        enableCompare = false;  // 启用深度比较（用于阴影贴图）
};

// --- Sampler handle ---
class IRHISampler {
public:
    virtual ~IRHISampler() = default;
};

} // namespace he::rhi
