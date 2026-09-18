// ============================================================
// VulkanSwapChain.cpp — Vulkan 交换链实现
// 负责 SwapChain 创建/销毁、窗口缩放、图像获取与呈现
// ============================================================
#include "VulkanSwapChain.h"
#include "Core/Log.h"
#include "Core/Assert.h"
#include <algorithm>

namespace he::rhi {

VulkanSwapChain::VulkanSwapChain(VkDevice device, VkPhysicalDevice physical,
                                 VkSurfaceKHR surface, VkQueue presentQueue,
                                 const SwapChainDesc& desc)
    : m_Device(device), m_Physical(physical), m_Surface(surface)
    , m_PresentQueue(presentQueue)
    , m_Width(desc.width), m_Height(desc.height)
    , m_HDR(desc.hdr)
{
    CreateSwapchain();
    HE_CORE_INFO("Vulkan swapchain created: {}x{}", m_Width, m_Height);
}

VulkanSwapChain::~VulkanSwapChain() {
    DestroySwapchain();
}

void VulkanSwapChain::CreateSwapchain() {
    // 查询表面能力
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_Physical, m_Surface, &caps);

    u32 formatCount;
    vkGetPhysicalDeviceSurfaceFormatsKHR(m_Physical, m_Surface, &formatCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(m_Physical, m_Surface, &formatCount, formats.data());

    // 优先选择格式：HDR10（A2B10G10R10 + ST.2084）否则 BGRA8 sRGB
    m_Format = VK_FORMAT_B8G8R8A8_UNORM;
    m_ColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    if (m_HDR) {
        for (auto& f : formats) {
            if (f.format == VK_FORMAT_A2B10G10R10_UNORM_PACK32
                && f.colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT) {
                m_Format = f.format;
                m_ColorSpace = f.colorSpace;
                break;
            }
        }
    }
    // 未命中 HDR10 时回退 SDR
    for (auto& f : formats) {
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR
            && m_Format == VK_FORMAT_B8G8R8A8_UNORM) {
            m_Format = f.format;
            m_ColorSpace = f.colorSpace;
            break;
        }
    }

    u32 presentModeCount;
    vkGetPhysicalDeviceSurfacePresentModesKHR(m_Physical, m_Surface, &presentModeCount, nullptr);
    std::vector<VkPresentModeKHR> presentModes(presentModeCount);
    vkGetPhysicalDeviceSurfacePresentModesKHR(m_Physical, m_Surface, &presentModeCount, presentModes.data());

    VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR; // 始终可用
    for (auto& mode : presentModes) {
        if (mode == VK_PRESENT_MODE_MAILBOX_KHR) {
            presentMode = mode;
            break;
        }
    }

    // 窗口最小化时尺寸可能为 0，跳过创建
    if (m_Width == 0 || m_Height == 0) return;

    m_Width  = std::clamp(m_Width,  caps.minImageExtent.width,  caps.maxImageExtent.width);
    m_Height = std::clamp(m_Height, caps.minImageExtent.height, caps.maxImageExtent.height);

    m_ImageCount = std::max(kSwapchainImageCount, caps.minImageCount);
    if (caps.maxImageCount > 0) m_ImageCount = std::min(m_ImageCount, caps.maxImageCount);

    VkSwapchainCreateInfoKHR swapInfo{};
    swapInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    swapInfo.surface = m_Surface;
    swapInfo.minImageCount = m_ImageCount;
    swapInfo.imageFormat = m_Format;
    swapInfo.imageColorSpace = m_ColorSpace;
    swapInfo.imageExtent = {m_Width, m_Height};
    swapInfo.imageArrayLayers = 1;
    swapInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                         | VK_IMAGE_USAGE_STORAGE_BIT           // RT 直接写入 BackBuffer
                         | VK_IMAGE_USAGE_TRANSFER_DST_BIT;     // 支持拷贝到 BackBuffer
    swapInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    swapInfo.preTransform = caps.currentTransform;
    swapInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    swapInfo.presentMode = presentMode;
    swapInfo.clipped = VK_TRUE;

    VkResult result = vkCreateSwapchainKHR(m_Device, &swapInfo, nullptr, &m_Swapchain);
    HE_ASSERT(result == VK_SUCCESS, "Failed to create swapchain");

    // 获取交换链图像
    vkGetSwapchainImagesKHR(m_Device, m_Swapchain, &m_ImageCount, nullptr);
    m_Images.resize(m_ImageCount);
    vkGetSwapchainImagesKHR(m_Device, m_Swapchain, &m_ImageCount, m_Images.data());

    // 创建 ImageView
    m_ImageViews.resize(m_ImageCount);
    for (u32 i = 0; i < m_ImageCount; ++i) {
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = m_Images[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = m_Format;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;
        vkCreateImageView(m_Device, &viewInfo, nullptr, &m_ImageViews[i]);
    }

    // 创建同步原语：acquire 信号量按飞行帧槽位各一份（配栅栏），render-complete 按图像各一份
    //（原因见 VulkanSwapChain.h 的成员注释：每帧复用同一个信号量会触发
    //  VUID-vkAcquireNextImageKHR-semaphore-01779）
    {
        VkSemaphoreCreateInfo semInfo{};
        semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;   // 首次使用时无需等待
        for (u32 i = 0; i < kAcquireSlots; ++i) {
            vkCreateSemaphore(m_Device, &semInfo, nullptr, &m_AcquireSemaphores[i]);
            vkCreateFence(m_Device, &fenceInfo, nullptr, &m_AcquireFences[i]);
        }
        m_RenderCompleteSemaphores.resize(m_ImageCount);
        for (u32 i = 0; i < m_ImageCount; ++i) {
            vkCreateSemaphore(m_Device, &semInfo, nullptr, &m_RenderCompleteSemaphores[i]);
        }
        m_AcquireSlot = 0;
    }

    // 创建深度模板纹理（与 SwapChain 同尺寸）
    VkImageCreateInfo depthInfo{};
    depthInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    depthInfo.imageType     = VK_IMAGE_TYPE_2D;
    depthInfo.format        = VK_FORMAT_D32_SFLOAT;
    depthInfo.extent        = {m_Width, m_Height, 1};
    depthInfo.mipLevels     = 1;
    depthInfo.arrayLayers   = 1;
    depthInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
    depthInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
    depthInfo.usage         = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    depthInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    vkCreateImage(m_Device, &depthInfo, nullptr, &m_DepthImage);

    VkMemoryRequirements depthMemReqs;
    vkGetImageMemoryRequirements(m_Device, m_DepthImage, &depthMemReqs);

    VkMemoryAllocateInfo depthAlloc{};
    depthAlloc.sType          = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    depthAlloc.allocationSize = depthMemReqs.size;
    // 查找 Device Local 内存类型
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(m_Physical, &memProps);
    u32 depthMemType = 0;
    for (u32 i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((depthMemReqs.memoryTypeBits & (1 << i)) &&
            (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
            { depthMemType = i; break; }
    }
    depthAlloc.memoryTypeIndex = depthMemType;
    vkAllocateMemory(m_Device, &depthAlloc, nullptr, &m_DepthImageMemory);
    vkBindImageMemory(m_Device, m_DepthImage, m_DepthImageMemory, 0);

    VkImageViewCreateInfo depthViewInfo{};
    depthViewInfo.sType     = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    depthViewInfo.image     = m_DepthImage;
    depthViewInfo.viewType  = VK_IMAGE_VIEW_TYPE_2D;
    depthViewInfo.format    = VK_FORMAT_D32_SFLOAT;
    depthViewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    depthViewInfo.subresourceRange.levelCount = 1;
    depthViewInfo.subresourceRange.layerCount = 1;
    vkCreateImageView(m_Device, &depthViewInfo, nullptr, &m_DepthImageView);
}

void VulkanSwapChain::DestroySwapchain() {
    for (auto& view : m_ImageViews) vkDestroyImageView(m_Device, view, nullptr);
    m_ImageViews.clear();
    if (m_DepthImageView)   { vkDestroyImageView(m_Device, m_DepthImageView, nullptr); m_DepthImageView = VK_NULL_HANDLE; }
    if (m_DepthImage)       { vkDestroyImage(m_Device, m_DepthImage, nullptr); m_DepthImage = VK_NULL_HANDLE; }
    if (m_DepthImageMemory) { vkFreeMemory(m_Device, m_DepthImageMemory, nullptr); m_DepthImageMemory = VK_NULL_HANDLE; }
    // 销毁同步原语：acquire 信号量（按槽位）+ 各自的栅栏 + render-complete 信号量（按图像）
    for (u32 i = 0; i < kAcquireSlots; ++i) {
        if (m_AcquireSemaphores[i]) { vkDestroySemaphore(m_Device, m_AcquireSemaphores[i], nullptr); m_AcquireSemaphores[i] = VK_NULL_HANDLE; }
        if (m_AcquireFences[i])     { vkDestroyFence(m_Device, m_AcquireFences[i], nullptr);         m_AcquireFences[i]     = VK_NULL_HANDLE; }
        // 消费栅栏属于命令列表，不在此销毁；只清掉引用，避免重建后指向旧栅栏
        m_AcquireConsumedFences[i] = VK_NULL_HANDLE;
    }
    // 槽位回到 0：调用方在这之前已 vkDeviceWaitIdle，新信号量从确定状态开始
    m_AcquireSlot = 0;
    for (auto& sem : m_RenderCompleteSemaphores) {
        if (sem) vkDestroySemaphore(m_Device, sem, nullptr);
    }
    m_RenderCompleteSemaphores.clear();
    if (m_Swapchain) vkDestroySwapchainKHR(m_Device, m_Swapchain, nullptr);
    m_Swapchain = VK_NULL_HANDLE;
}

void VulkanSwapChain::Resize(u32 width, u32 height) {
    // 窗口最小化时尺寸为 0，跳过重建（恢复时 GLFW 会再次回调）
    if (width == 0 || height == 0) {
        m_IsMinimized = true;
        return;
    }
    m_IsMinimized = false;
    vkDeviceWaitIdle(m_Device);
    DestroySwapchain();
    m_Width = width;
    m_Height = height;
    CreateSwapchain();
}

bool VulkanSwapChain::AcquireNextImage() {
    // 窗口最小化时跳过图像获取
    if (m_IsMinimized || m_Swapchain == VK_NULL_HANDLE) return false;

    // 轮转到下一个 acquire 槽位
    m_AcquireSlot = (m_AcquireSlot + 1) % kAcquireSlots;

    // 【必须】先等"上一次用该槽位的提交"完成：
    //   处于"已发信号、尚未被任何提交等待"状态的信号量不能再交给
    //   vkAcquireNextImageKHR（VUID-vkAcquireNextImageKHR-semaphore-01779）。
    //   下面的 acquire 栅栏只能证明"信号已经发出"，证明不了"已被等待消费"；
    //   真正等待过该信号量的是那次提交，所以必须等它的栅栏。
    //   只等待、不重置 —— 该栅栏由命令列表拥有并在提交前自行重置。
    if (m_AcquireConsumedFences[m_AcquireSlot] != VK_NULL_HANDLE) {
        vkWaitForFences(m_Device, 1, &m_AcquireConsumedFences[m_AcquireSlot], VK_TRUE, UINT64_MAX);
        m_AcquireConsumedFences[m_AcquireSlot] = VK_NULL_HANDLE;
    }

    // 复用该槽位的信号量之前，先等它的栅栏 —— 保证上一轮 acquire 本身已经完成
    if (m_AcquireFences[m_AcquireSlot] != VK_NULL_HANDLE) {
        vkWaitForFences(m_Device, 1, &m_AcquireFences[m_AcquireSlot], VK_TRUE, UINT64_MAX);
        vkResetFences(m_Device, 1, &m_AcquireFences[m_AcquireSlot]);
    }

    VkResult result = vkAcquireNextImageKHR(m_Device, m_Swapchain, UINT64_MAX,
                                            m_AcquireSemaphores[m_AcquireSlot],
                                            m_AcquireFences[m_AcquireSlot],
                                            &m_CurrentImage);
    return result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR;
}

void VulkanSwapChain::Present(bool /*vsync*/) {
    // 用"当前图像自己的" render-complete 信号量：同一图像再次被 acquire 蕴含上次 present
    // 已完成，因此不会出现"信号量仍有未完成操作"（详见头文件成员注释）
    VkSemaphore waitSem = GetRenderCompleteSemaphore();
    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.swapchainCount  = 1;
    presentInfo.pSwapchains     = &m_Swapchain;
    presentInfo.pImageIndices   = &m_CurrentImage;
    presentInfo.waitSemaphoreCount = waitSem ? 1u : 0u;
    presentInfo.pWaitSemaphores    = waitSem ? &waitSem : nullptr;

    vkQueuePresentKHR(m_PresentQueue, &presentInfo);
}

} // namespace he::rhi
