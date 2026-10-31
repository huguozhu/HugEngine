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
    // 【P10】调试名（仅用于统计 / 诊断；不改变任何行为）。
    // Storage 缓冲的堆类型统计（`HE_STORAGE_HEAP_STATS=1`）按它列清单，
    // 便于把"GPU 频繁读写的大缓冲"和"CPU 每帧 Map 的小缓冲"区分开。
    // 仍以聚合初始化传参 ⇒ 必须保持在**最后一个**字段之后新增。
    const char* debugName   = nullptr;
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

    // ========================================================
    // 瞬态纹理的存放位置（P6：内存别名的验收/诊断口）
    //
    // 语义：仅当该纹理是**瞬态分配**（CreateTransientTexture 创建，放在后端共享的
    //       瞬态堆上）时返回 true，并给出它在堆内的字节偏移与分配大小。
    //       普通纹理、或后端没有瞬态堆时返回 false（默认实现）。
    // 用途：验证"生命周期不重叠的两张瞬态纹理共用同一段内存"——直接比较偏移即可，
    //       不需要按后端私有类型做转换（样例/测试保持 RHI 中立）。
    // ========================================================
    virtual bool GetTransientPlacement(u64& /*offset*/, u64& /*size*/) const { return false; }
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
