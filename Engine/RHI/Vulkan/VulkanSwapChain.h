#pragma once

// ============================================================
// VulkanSwapChain.h — Vulkan SwapChain 封装
// 从 VulkanInternal.h 拆分，独立于其他 Vulkan 类型
// ============================================================

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include "RHI/SwapChain.h"

#include <vector>

namespace he::rhi {

// ============================================================
// VulkanSwapChain — 完整定义
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
    VkFormat       GetVkFormat()              const { return m_Format; }
    Format         GetColorFormat()           const override {
        return m_Format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 ? Format::A2B10G10R10_UNORM_PACK32 : Format::BGRA8_UNORM;
    }
    u32            GetBackendFormat()         const override { return static_cast<u32>(m_Format); }
    VkImageView    GetImageView(u32 i)        const { return m_ImageViews[i]; }
    VkImageView    GetDepthImageView()        const { return m_DepthImageView; }
    void* GetCurrentBackBufferView() const override { return reinterpret_cast<void*>(m_ImageViews[m_CurrentImage]); }
    void* GetDepthBufferView()       const override { return reinterpret_cast<void*>(m_DepthImageView); }
    VkExtent2D     GetExtent()                const { return {m_Width, m_Height}; }
    VkImage        GetImage(u32 i)            const { return m_Images[i]; }
    VkSemaphore    GetImageAcquiredSemaphore() const { return m_AcquireSemaphores[m_AcquireSlot]; }
    VkSemaphore    GetRenderCompleteSemaphore() const {
        return m_RenderCompleteSemaphores.empty() ? VK_NULL_HANDLE
                                                  : m_RenderCompleteSemaphores[m_CurrentImage];
    }

private:
    void CreateSwapchain();
    void DestroySwapchain();

    VkDevice         m_Device        = VK_NULL_HANDLE;
    VkPhysicalDevice m_Physical      = VK_NULL_HANDLE;
    VkSurfaceKHR     m_Surface       = VK_NULL_HANDLE;
    VkQueue          m_PresentQueue  = VK_NULL_HANDLE;

    VkSwapchainKHR   m_Swapchain     = VK_NULL_HANDLE;
    VkFormat         m_Format        = VK_FORMAT_B8G8R8A8_UNORM;
    VkColorSpaceKHR  m_ColorSpace    = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    bool             m_HDR           = false;
    u32              m_Width         = 0;
    u32              m_Height        = 0;
    u32              m_ImageCount    = 0;
    u32              m_CurrentImage  = 0;

    bool             m_IsMinimized    = false;

    // ── 同步原语：必须"多份"，不能每帧复用同一个 ──
    // 曾经每帧复用单个 acquire / render-complete 信号量，实测每帧触发一次
    // VUID-vkAcquireNextImageKHR-semaphore-01779（"Semaphore must not have any pending
    // operations"）：上一帧对该信号量的等待尚未完成时又把它交给 acquire。
    //
    // 现在：
    //   · acquire 信号量按**飞行帧槽位**各一份（配套一个栅栏；复用前先等该槽位的栅栏，
    //     即保证上一轮 acquire 的等待已经完成）
    //   · render-complete 信号量按**交换链图像**各一份（同一图像能再次被 acquire，
    //     本身就蕴含上一次 present 已完成，因而该信号量的等待也已完成）
    static constexpr u32 kAcquireSlots = 3;   // 与 RHI 的飞行帧数（kMaxFramesInFlight）一致
    VkSemaphore      m_AcquireSemaphores[kAcquireSlots] = {};
    VkFence          m_AcquireFences[kAcquireSlots]     = {};
    u32              m_AcquireSlot = 0;
    std::vector<VkSemaphore> m_RenderCompleteSemaphores;

    VkImage         m_DepthImage        = VK_NULL_HANDLE;
    VkImageView     m_DepthImageView    = VK_NULL_HANDLE;
    VkDeviceMemory  m_DepthImageMemory  = VK_NULL_HANDLE;

    std::vector<VkImage>     m_Images;
    std::vector<VkImageView> m_ImageViews;
};

} // namespace he::rhi
