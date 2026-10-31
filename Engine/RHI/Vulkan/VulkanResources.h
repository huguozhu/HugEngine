#pragma once

// ============================================================
// VulkanResources.h — Vulkan 资源类型（Buffer / Texture / Sampler）
// 从 VulkanInternal.h 拆分
// ============================================================

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

// VMA (Vulkan Memory Allocator)
#define VMA_VULKAN_VERSION 1003000
#include "vk_mem_alloc.h"

#include "RHI/RHI.h"

#include <vector>

namespace he::rhi {

// ============================================================
// VulkanBuffer — GPU 缓冲（VMA 管理）
// ============================================================
class VulkanBuffer final : public IRHIBuffer {
public:
    VulkanBuffer(VmaAllocator allocator, const BufferDesc& desc);
    ~VulkanBuffer() override;
    usize GetSize()  const override { return m_Size; }
    void* Map()            override;
    void  Unmap()          override;
    u64   GetDeviceAddress() const override { return m_DeviceAddress; }
    VkBuffer GetHandle() const { return m_Buffer; }
private:
    VmaAllocator      m_Allocator     = VK_NULL_HANDLE;
    VkDevice          m_Device        = VK_NULL_HANDLE;
    VkBuffer          m_Buffer        = VK_NULL_HANDLE;
    VmaAllocation     m_Allocation    = VK_NULL_HANDLE;
    usize             m_Size          = 0;
    u64               m_DeviceAddress = 0;
    bool              m_IsMapped      = false;
    void*             m_MappedPtr     = nullptr;
    bool              m_IsCoherent    = true;  // 内存是否 host-coherent（无需手动 flush/invalidate）
};

// ============================================================
// VulkanTexture — GPU 纹理（VMA 管理）
//
// 【P6】它同时是"瞬态纹理"的基类：VulkanPlacedTexture 接管一张已经绑定到共享堆的
// VkImage（不走 VMA 分配）。这样后端里既有的 `static_cast<VulkanTexture*>(tex)` 对
// 瞬态纹理也成立 —— 之前 VulkanPlacedTexture 是与 VulkanTexture 无关的另一个类型，
// 那些 static_cast 会把 m_Image 读成别的成员（校验层报 "Invalid VkImage Object"）。
// ============================================================
class VulkanTexture : public IRHITexture {
public:
    /// 接管外部 VkImage 的标签（瞬态纹理用）
    struct AdoptExternal {};

    VulkanTexture(VmaAllocator allocator, VkCommandPool cmdPool, VkQueue queue,
                  const TextureDesc& desc);
    ~VulkanTexture() override;
    u32    GetWidth()        const override { return m_Width; }
    u32    GetHeight()       const override { return m_Height; }
    u32    GetDepth()        const override { return m_Depth; }
    u32    GetMipLevels()    const override { return m_MipLevels; }
    u32    GetArrayLayers()  const override { return m_ArrayLayers; }
    Format GetFormat()       const override { return m_Format; }
    void*  GetNativeHandle() const override { return reinterpret_cast<void*>(m_ImageView); }
    void*  GetNativeHandle(u32 index) const override {
        return (index < m_FaceViews.size())
            ? reinterpret_cast<void*>(m_FaceViews[index])
            : reinterpret_cast<void*>(m_ImageView);
    }
    VkImage     GetImage()     const { return m_Image; }
    VkImageView GetImageView() const { return m_ImageView; }
    VkImageView GetFaceView(u32 face) const {
        return (face < m_FaceViews.size()) ? m_FaceViews[face] : VK_NULL_HANDLE;
    }
    VkFormat   GetVkFormat() const { return m_VkFormat; }
    VkDevice   GetDevice()   const { return m_Device; }
    /// 创建时声明的用途位（诊断用：判断某纹理是否可能由 compute 写入，见 TextureLayoutTracker.h）
    TextureUsage GetUsage()  const { return m_Usage; }

protected:
    /// 【P6】接管一张外部创建并已绑定内存的 VkImage（瞬态分配器所有）：不建 VMA 分配，
    /// 但 ImageView / 逐面视图 / 布局追踪登记与普通纹理完全一致。
    VulkanTexture(AdoptExternal, VkDevice device, VkImage image, VkFormat vkFormat,
                  const TextureDesc& desc);

    /// 创建 ImageView 与逐面视图，并登记「视图 → 图像」（两个构造函数共用）
    void CreateImageViews(const TextureDesc& desc);

private:
    void UploadInitialData(VkCommandPool cmdPool, VkQueue queue, const TextureDesc& desc);
    VkDevice         m_Device       = VK_NULL_HANDLE;
    VmaAllocator     m_Allocator    = VK_NULL_HANDLE;
    VkImage          m_Image        = VK_NULL_HANDLE;
    VkImageView      m_ImageView    = VK_NULL_HANDLE;
    VmaAllocation    m_Allocation   = VK_NULL_HANDLE;
    /// true：m_Image 由外部（瞬态堆）提供，析构时直接 vkDestroyImage，不走 vmaDestroyImage
    bool             m_AdoptedImage = false;
    std::vector<VkImageView> m_FaceViews;
    u32              m_Width        = 1;
    u32              m_Height       = 1;
    u32              m_Depth        = 1;
    u32              m_MipLevels    = 1;
    u32              m_ArrayLayers  = 1;
    u32              m_SampleCount  = 1;
    Format           m_Format       = Format::RGBA8_UNORM;
    VkFormat         m_VkFormat     = VK_FORMAT_R8G8B8A8_UNORM;
    TextureUsage     m_Usage        = TextureUsage::ShaderResource;   // 创建时声明的用途位
};

// ============================================================
// VulkanPlacedTexture — 瞬态纹理（使用外部 VkDeviceMemory）
// 与 VulkanTexture 的关键区别：不创建/拥有 VkDeviceMemory（内存由 TransientResourceAllocator
// 管理），它只接管 VkImage 对象本身。继承 VulkanTexture 是为了让后端里
// `static_cast<VulkanTexture*>` 的既有路径对瞬态纹理同样成立（见上）。
// ============================================================
class VulkanPlacedTexture final : public VulkanTexture {
public:
    /// 构造函数：接收已绑定到 Heap 的 VkImage，创建 ImageView
    /// @param offset / size 该图在瞬态堆内的偏移与分配大小（P6：内存别名的验收口）
    VulkanPlacedTexture(VkDevice device, VkImage image, VkFormat vkFormat,
                        const TextureDesc& desc, u64 offset = 0, u64 size = 0);
    ~VulkanPlacedTexture() override = default;   // 视图与 VkImage 由 VulkanTexture 析构统一销毁

    // P6：瞬态纹理在共享堆内的偏移与分配大小（偏移相同 = 生命周期不重叠的两张图共用内存）
    bool GetTransientPlacement(u64& offset, u64& size) const override {
        offset = m_TransientOffset;
        size   = m_TransientSize;
        return true;
    }

private:
    u64 m_TransientOffset = 0;
    u64 m_TransientSize   = 0;
};

// ============================================================
// VulkanSampler — GPU 采样器
// ============================================================
class VulkanSampler final : public IRHISampler {
public:
    VulkanSampler(VkDevice device, const SamplerDesc& desc);
    ~VulkanSampler() override;
    VkSampler GetHandle() const { return m_Sampler; }
private:
    VkDevice   m_Device   = VK_NULL_HANDLE;
    VkSampler  m_Sampler  = VK_NULL_HANDLE;
};

} // namespace he::rhi
