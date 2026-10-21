#pragma once

// ============================================================
// VulkanSwapChain.h — Vulkan SwapChain 封装
// 从 VulkanInternal.h 拆分，独立于其他 Vulkan 类型
// ============================================================

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include "RHI/SwapChain.h"
#include "RHI/Types.h"   // kMaxFramesInFlight（kAcquireSlots 的派生来源）

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
    /// 【绕过路由】真正呈现（模式 2 由 RHI 线程经 `IRHIDevice::ExecutePresent` 调用）。
    /// imageIndex / waitSemaphore 由渲染线程在 `Present` 时捕获 —— RHI 线程不再读交换链状态。
    void PresentNow(u32 imageIndex, void* waitSemaphore, bool vsync);

    VkSwapchainKHR GetHandle()                const { return m_Swapchain; }
    VkFormat       GetVkFormat()              const { return m_Format; }
    Format         GetColorFormat()           const override {
        return m_Format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 ? Format::A2B10G10R10_UNORM_PACK32 : Format::BGRA8_UNORM;
    }
    u32            GetBackendFormat()         const override { return static_cast<u32>(m_Format); }
    VkImageView    GetImageView(u32 i)        const { return m_ImageViews[i]; }
    VkImageView    GetDepthImageView()        const { return m_DepthImageView; }
    VkImage        GetDepthImage()            const { return m_DepthImage; }   // render pass 布局确保要用 VkImage
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
    //
    // ⚠️ acquire 槽位的栅栏只证明"信号已经发出"，**不证明"信号量已被等待消费"**：
    //    处于"已发信号、尚未被任何提交等待"状态的信号量不能再交给
    //    vkAcquireNextImageKHR。因此复用槽位前还必须等 SetAcquireConsumedFence 登记进来的
    //    **提交栅栏**（提交才是真正等待该信号量的那一方）。缺这一等会偶发卡死：
    //    实测 24 次启动中有 1 次在第 42 帧挂住并报该 VUID（见 GI 文档 §10.1 第 2 项）。
    // 派生自 RHI 的飞行帧数（**不要在这里写独立数字**）：m_AcquireSlot 每调用一次
    // AcquireNextImage 推进一格 ⇒ 等价于"每在飞帧一格"，故环长必须 ≥ kMaxFramesInFlight，
    // 否则每次 acquire 都可能阻塞等上一轮提交完成 ⇒ 退化成静默节流（在飞帧数被压到环长）。
    static constexpr u32 kAcquireSlots = rhi::kMaxFramesInFlight;
    static_assert(kAcquireSlots >= rhi::kMaxFramesInFlight,
                  "acquire 槽位少于在飞帧数 ⇒ 会退化成阻塞式节流");
    VkSemaphore      m_AcquireSemaphores[kAcquireSlots] = {};
    VkFence          m_AcquireFences[kAcquireSlots]     = {};
    // 每个槽位最近一次"等待过该槽位 acquire 信号量"的提交栅栏（提交方在提交后登记）
    VkFence          m_AcquireConsumedFences[kAcquireSlots] = {};
    u32              m_AcquireSlot = 0;
    std::vector<VkSemaphore> m_RenderCompleteSemaphores;

public:
    /// 提交方在提交后调用：告知"本槽位的 acquire 信号量已被这次提交等待消费"。
    /// 复用该槽位前会等这个栅栏，从而保证信号量不再处于已发信号未被等待的状态。
    /// 只等待、不重置：这个栅栏由命令列表拥有并自行重置。
    void SetAcquireConsumedFence(VkFence fence) { m_AcquireConsumedFences[m_AcquireSlot] = fence; }

    /// 交换链的**实际**图像数（= max(kSwapchainImageCount, caps.minImageCount)，可能大于 3）
    /// 【为什么要它】命令列表据此建立逐图像的视图表；写死 kSwapchainImageCount 会在实际
    /// 图像数更大时少建视图，导致超出部分无法参与附件/布局追踪。
    [[nodiscard]] u32 GetImageCount() const { return m_ImageCount; }

private:

    VkImage         m_DepthImage        = VK_NULL_HANDLE;
    VkImageView     m_DepthImageView    = VK_NULL_HANDLE;
    VkDeviceMemory  m_DepthImageMemory  = VK_NULL_HANDLE;

    std::vector<VkImage>     m_Images;
    std::vector<VkImageView> m_ImageViews;
};

} // namespace he::rhi
