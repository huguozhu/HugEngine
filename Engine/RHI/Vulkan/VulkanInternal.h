#pragma once

// ============================================================
// VulkanInternal.h — Internal bridge for sample/testing code
//
// NOT part of the public RHI API. Used only by samples
// until the full RHI abstraction is complete (Phase 1-2).
// ============================================================

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include "RHI/RHI.h"
#include <span>
#include <vector>

namespace he::rhi {

// ============================================================
// VulkanSwapChain — 完整定义（供 Sample 和内部分享）
// ============================================================
class VulkanSwapChain final : public IRHISwapChain {
public:
    VulkanSwapChain(VkDevice device, VkPhysicalDevice physical, VkSurfaceKHR surface,
                    VkQueue presentQueue, const SwapChainDesc& desc);
    ~VulkanSwapChain() override;

    void Resize(u32 width, u32 height) override;
    u32  GetCurrentBackBufferIndex() const override { return m_CurrentImage; }
    u32  GetWidth()  const override { return m_Width; }
    u32  GetHeight() const override { return m_Height; }
    bool AcquireNextImage() override;
    void Present(bool vsync) override;

    VkSwapchainKHR GetHandle()                const { return m_Swapchain; }
    VkFormat       GetFormat()                const { return m_Format; }
    VkImageView    GetImageView(u32 i)        const { return m_ImageViews[i]; }
    VkImageView    GetDepthImageView()        const { return m_DepthImageView; }
    VkExtent2D     GetExtent()                const { return {m_Width, m_Height}; }
    VkImage        GetImage(u32 i)            const { return m_Images[i]; }
    VkSemaphore    GetImageAcquiredSemaphore() const { return m_ImageAcquired; }
    VkSemaphore    GetRenderCompleteSemaphore() const { return m_RenderComplete; }

private:
    void CreateSwapchain();
    void DestroySwapchain();

    VkDevice         m_Device        = VK_NULL_HANDLE;
    VkPhysicalDevice m_Physical      = VK_NULL_HANDLE;
    VkSurfaceKHR     m_Surface       = VK_NULL_HANDLE;
    VkQueue          m_PresentQueue  = VK_NULL_HANDLE;

    VkSwapchainKHR   m_Swapchain     = VK_NULL_HANDLE;
    VkFormat         m_Format        = VK_FORMAT_B8G8R8A8_UNORM;
    u32              m_Width         = 0;
    u32              m_Height        = 0;
    u32              m_ImageCount    = 0;
    u32              m_CurrentImage  = 0;

    VkSemaphore      m_ImageAcquired  = VK_NULL_HANDLE;  // 图像可用信号
    VkSemaphore      m_RenderComplete = VK_NULL_HANDLE;  // 渲染完成信号

    // 深度模板缓冲（与 SwapChain 同尺寸）
    VkImage         m_DepthImage        = VK_NULL_HANDLE;
    VkImageView     m_DepthImageView    = VK_NULL_HANDLE;
    VkDeviceMemory  m_DepthImageMemory  = VK_NULL_HANDLE;

    std::vector<VkImage>     m_Images;
    std::vector<VkImageView> m_ImageViews;
};

// ============================================================
// VulkanCommandList — 完整定义（供 Sample 和内部分享）
// ============================================================
class VulkanCommandList final : public IRHICommandList {
public:
    VulkanCommandList(VkDevice device, VkQueue queue, u32 queueFamily);
    ~VulkanCommandList() override;

    void Begin() override;
    void End()   override;
    void BeginRenderPass(u32 colorCount, Format colorFmt, Format depthFmt,
                         const ClearValue* clear) override;
    void EndRenderPass() override;
    void SetSwapChain(IRHISwapChain* swapchain) override;
    void SetPipeline(IRHIPipelineState* pso) override;
    void SetVertexBuffer(IRHIBuffer* buffer, u32 binding) override;
    void SetIndexBuffer(IRHIBuffer* buffer, u32 offset) override;
    void SetViewport(const Viewport& vp) override;
    void SetScissor(const ScissorRect& sc) override;
    void Draw(u32 vertexCount, u32 instanceCount, u32 firstVertex, u32 firstInstance) override;
    void DrawIndexed(u32 indexCount, u32 instanceCount, u32 firstIndex,
                     i32 vertexOffset, u32 firstInstance) override;
    void Submit() override;

    // Phase 1 桥接：直接设置 Framebuffer
    void SetSwapchainViews(Span<VkImageView> views, VkExtent2D extent) {
        m_SwapchainViews.assign(views.begin(), views.end());
        m_SwapchainExtent = extent;
    }
    void SetCurrentImageIndex(u32 index) { m_CurrentImageIndex = index; }

    // 设置 Submit 时的等待/信号 Semaphore
    void SetSyncSemaphores(VkSemaphore wait, VkSemaphore signal) {
        m_WaitSemaphore   = wait;
        m_SignalSemaphore = signal;
    }

    VkCommandBuffer GetHandle() const { return m_CmdBuffer; }

private:
    VkDevice         m_Device      = VK_NULL_HANDLE;
    VkQueue          m_Queue       = VK_NULL_HANDLE;
    u32              m_QueueFamily = 0;

    VkCommandPool    m_CmdPool     = VK_NULL_HANDLE;
    VkCommandBuffer  m_CmdBuffer   = VK_NULL_HANDLE;
    VkFence          m_Fence       = VK_NULL_HANDLE;

    // 关联的 SwapChain（自动管理 Framebuffer + 同步 + 图像索引）
    VulkanSwapChain* m_pSwapChain = nullptr;

    // 同步原语
    VkSemaphore      m_WaitSemaphore   = VK_NULL_HANDLE;
    VkSemaphore      m_SignalSemaphore = VK_NULL_HANDLE;

    VkPipeline       m_CurrentPipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_CurrentLayout   = VK_NULL_HANDLE;
    VkRenderPass     m_CurrentRenderPass = VK_NULL_HANDLE;
    VkBuffer         m_CurrentVB       = VK_NULL_HANDLE;
    u32              m_VBBinding       = 0;
    VkBuffer         m_CurrentIB       = VK_NULL_HANDLE;
    VkIndexType      m_CurrentIndexType = VK_INDEX_TYPE_UINT32;
    u32              m_IBOffset         = 0;

    std::vector<VkImageView> m_SwapchainViews;
    VkExtent2D               m_SwapchainExtent{};
    std::vector<VkFramebuffer> m_Framebuffers;
    u32                       m_CurrentImageIndex = 0;
};

// ============================================================
// VulkanPipelineState — 完整定义（非 inline 方法在 VulkanResources.cpp）
// ============================================================
class VulkanPipelineState final : public IRHIPipelineState {
public:
    VulkanPipelineState(VkDevice device, VkPipeline pipeline,
                        VkPipelineLayout layout, VkRenderPass renderPass)
        : m_Device(device), m_Pipeline(pipeline)
        , m_PipelineLayout(layout), m_RenderPass(renderPass) {}
    ~VulkanPipelineState();
    VkPipeline       GetPipeline()       const { return m_Pipeline; }
    VkPipelineLayout GetPipelineLayout() const { return m_PipelineLayout; }
    VkRenderPass     GetRenderPass()     const { return m_RenderPass; }
private:
    VkDevice         m_Device;
    VkPipeline       m_Pipeline;
    VkPipelineLayout m_PipelineLayout;
    VkRenderPass     m_RenderPass;
};

// ============================================================
// VulkanBuffer — 完整定义（非 inline 方法在 VulkanResources.cpp）
// ============================================================
class VulkanBuffer final : public IRHIBuffer {
public:
    VulkanBuffer(VkDevice device, VkPhysicalDevice physical, const BufferDesc& desc);
    ~VulkanBuffer() override;
    usize GetSize()  const override { return m_Size; }
    void* Map()            override;
    void  Unmap()          override;
    u64   GetDeviceAddress() const override { return m_DeviceAddress; }
    VkBuffer GetHandle() const { return m_Buffer; }
private:
    VkDevice          m_Device        = VK_NULL_HANDLE;
    VkBuffer          m_Buffer        = VK_NULL_HANDLE;
    VkDeviceMemory    m_Memory        = VK_NULL_HANDLE;
    usize             m_Size          = 0;
    u64               m_DeviceAddress = 0;
    bool              m_IsMapped      = false;
    void*             m_MappedPtr     = nullptr;
};

// ============================================================
// VulkanTexture — 完整定义（非 inline 方法在 VulkanResources.cpp）
// ============================================================
class VulkanTexture final : public IRHITexture {
public:
    VulkanTexture(VkDevice device, VkPhysicalDevice physical,
                  VkCommandPool cmdPool, VkQueue queue,
                  const TextureDesc& desc);
    ~VulkanTexture() override;
    u32    GetWidth()        const override { return m_Width; }
    u32    GetHeight()       const override { return m_Height; }
    u32    GetDepth()        const override { return m_Depth; }
    u32    GetMipLevels()    const override { return m_MipLevels; }
    u32    GetArrayLayers()  const override { return m_ArrayLayers; }
    Format GetFormat()       const override { return m_Format; }
    VkImage     GetImage()     const { return m_Image; }
    VkImageView GetImageView() const { return m_ImageView; }
private:
    void UploadInitialData(VkCommandPool cmdPool, VkQueue queue, const TextureDesc& desc);
    VkDevice         m_Device       = VK_NULL_HANDLE;
    VkPhysicalDevice m_Physical     = VK_NULL_HANDLE;
    VkImage          m_Image        = VK_NULL_HANDLE;
    VkImageView      m_ImageView    = VK_NULL_HANDLE;
    VkDeviceMemory   m_Memory       = VK_NULL_HANDLE;
    u32              m_Width        = 1;
    u32              m_Height       = 1;
    u32              m_Depth        = 1;
    u32              m_MipLevels    = 1;
    u32              m_ArrayLayers  = 1;
    Format           m_Format       = Format::RGBA8_UNORM;
    VkFormat         m_VkFormat     = VK_FORMAT_R8G8B8A8_UNORM;
};

// ============================================================
// VulkanSampler — 完整定义（非 inline 方法在 VulkanResources.cpp）
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
