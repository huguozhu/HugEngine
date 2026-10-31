// ============================================================
// VulkanSwapChain.cpp — Vulkan 交换链实现
// 负责 SwapChain 创建/销毁、窗口缩放、图像获取与呈现
// ============================================================
#include "VulkanSwapChain.h"
#include "RHI/RHI.h"                    // he::rhi::GetDevice()：查询提交路由（模式 2）
#include "RHI/ScreenCapture.h"          // P8：按帧截图公共工具（环境变量 / 默认路径 / WIC 写 PNG）
#include "RHI/TextureLayoutTracker.h"   // 呈现后记录 PRESENT_SRC 布局（见 Present）
#include "RHI/ThreadAffinity.h"         // T0.1：交换链操作只允许在拥有线程（渲染线程）
#include "Core/Log.h"
#include "Core/Assert.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace he::rhi {

VulkanSwapChain::VulkanSwapChain(VkDevice device, VkPhysicalDevice physical,
                                 VkSurfaceKHR surface, VkQueue presentQueue,
                                 u32 graphicsFamily, const SwapChainDesc& desc)
    : m_Device(device), m_Physical(physical), m_Surface(surface)
    , m_PresentQueue(presentQueue)
    , m_QueueFamily(graphicsFamily)
    , m_Width(desc.width), m_Height(desc.height)
    , m_HDR(desc.hdr)
    , m_Vsync(desc.vsync)
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

    // 呈现模式：此前不看 desc.vsync，只在有 MAILBOX 时用 MAILBOX，否则 FIFO ⇒ 不支持 MAILBOX 的设备上
    // 关掉垂直同步（HE_NO_VSYNC=1）帧率仍锁在刷新率。关闭时优先 IMMEDIATE（不等垂直消隐，可能撕裂，
    // 对应 D3D12 的 ALLOW_TEARING），其次 MAILBOX；开启时沿用 MAILBOX 优先、FIFO 兜底
    auto hasMode = [&](VkPresentModeKHR m) {
        return std::find(presentModes.begin(), presentModes.end(), m) != presentModes.end();
    };
    VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR; // 始终可用
    if (!m_Vsync && hasMode(VK_PRESENT_MODE_IMMEDIATE_KHR))  presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
    else if (hasMode(VK_PRESENT_MODE_MAILBOX_KHR))           presentMode = VK_PRESENT_MODE_MAILBOX_KHR;

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
                         | VK_IMAGE_USAGE_TRANSFER_DST_BIT      // 支持拷贝到 BackBuffer
                         | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;     // P8：截图时把交换链图像拷到读回缓冲
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
    for (auto& view : m_ImageViews) {
        // 布局追踪器按视图句柄记录真实布局；视图销毁必须清掉记录，
        // 否则句柄被复用后会把「上一条命令流的布局」误当成新图的当前布局
        ForgetTrackedTextureLayout(reinterpret_cast<void*>(view));
        vkDestroyImageView(m_Device, view, nullptr);
    }
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
    // 【模式 2 / A-4】acquire 之前先把 RHI 线程上排队的提交/呈现**排空**：
    // 图像复用判定与 acquire 槽位记账都建立在"上一帧的 present 已真正执行"之上，而模式 2 里
    // present 是异步的 ⇒ 不排空就会在 present 尚未落地时复用 acquire 信号量/图像（实测命中
    // VUID-vkAcquireNextImageKHR-semaphore-01779 / -01286 / -surface-07783）。
    if (auto* dev = he::rhi::GetDevice(); dev != nullptr) {
        if (auto* router = dev->GetSubmitRouter(); router != nullptr) router->FlushAndWait();
    }
    HE_ASSERT_RENDER_THREAD();   // T0.1：交换链图像获取只允许在拥有线程（渲染线程）

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

void VulkanSwapChain::Present(bool vsync) {
    // 参数必须在**渲染线程**上捕获（见 SubmitRouter.h 的说明）
    const u32         imageIndex = m_CurrentImage;
    const VkSemaphore waitSem    = GetRenderCompleteSemaphore();
    // 模式 2：交给提交路由（RHI 线程稍后呈现）；未注入路由时与改造前完全一致
    if (auto* dev = he::rhi::GetDevice(); dev != nullptr) {
        if (auto* router = dev->GetSubmitRouter();
            router != nullptr && router->RoutePresent(this, imageIndex,
                                                      reinterpret_cast<void*>(waitSem), vsync))
            return;
    }
    PresentNow(imageIndex, reinterpret_cast<void*>(waitSem), vsync);
}

void VulkanSwapChain::PresentNow(u32 imageIndex, void* waitSemaphore, bool /*vsync*/) {
    // 拥有线程（模式 0/1）或已登记的提交者（模式 2 的 RHI 线程）
    HE_ASSERT_SUBMIT_THREAD();
    if (imageIndex >= m_ImageViews.size()) return;

    // 【P8】帧计数 + 按帧截图。必须在 vkQueuePresentKHR **之前**：
    //   · 交换链图像在 PRESENT_SRC 布局下属于呈现引擎，Present 之后内容不再由应用拥有；
    //   · 截图命令提交到同一个队列，同队列内 FIFO 有序 ⇒ 本帧绘制一定先完成。
    ++m_FrameCount;
    CaptureIfRequested(imageIndex);

    // 用"该图像自己的" render-complete 信号量：同一图像再次被 acquire 蕴含上次 present
    // 已完成，因此不会出现"信号量仍有未完成操作"（详见头文件成员注释）
    VkSemaphore waitSem = reinterpret_cast<VkSemaphore>(waitSemaphore);
    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.swapchainCount  = 1;
    presentInfo.pSwapchains     = &m_Swapchain;
    presentInfo.pImageIndices   = &imageIndex;
    presentInfo.waitSemaphoreCount = waitSem ? 1u : 0u;
    presentInfo.pWaitSemaphores    = waitSem ? &waitSem : nullptr;

    VkResult presentResult = vkQueuePresentKHR(m_PresentQueue, &presentInfo);
    // 呈现失败（尤其 VK_ERROR_DEVICE_LOST）必须显式打印：此前完全静默，
    // 设备丢失后样例仍会空转，日志里看不出"画面其实已经不再更新"。
    if (presentResult != VK_SUCCESS && presentResult != VK_SUBOPTIMAL_KHR) {
        HE_CORE_WARN("[Vulkan] vkQueuePresentKHR 失败：VkResult={}（第 {} 帧）",
                     static_cast<int>(presentResult), m_FrameCount);
    }

    // 呈现后交换链图像的真实布局是 PRESENT_SRC_KHR，必须记进布局追踪器：
    // 否则下一帧 RenderGraph 对导入的后备缓冲会沿用上一帧记录的"颜色附件"状态
    // 去发 barrier，而图像实际停在 PRESENT_SRC —— 校验层每帧报一次
    // VUID-VkImageMemoryBarrier-oldLayout-01197。
    if (presentResult == VK_SUCCESS || presentResult == VK_SUBOPTIMAL_KHR) {
        TrackTextureLayout(reinterpret_cast<void*>(m_ImageViews[imageIndex]),
                           ResourceState::Present);
    }
}

// ============================================================
// P8：按帧截图（与 D3D12 同一 RHI 接口，见 RHI/SwapChain.h 的 RequestCapture）
//
// 流程（在提交线程上、Present 之前）：
//   ① 建 host-visible + coherent 读回缓冲（BGRA8 / HDR10 都是 4 字节/像素）；
//   ② 临时命令池 + 命令缓冲（队列族 = 呈现队列族）；
//   ③ 布局：当前真实布局 → TRANSFER_SRC；
//   ④ vkCmdCopyImageToBuffer 拷到读回缓冲；
//   ⑤ 布局：TRANSFER_SRC → PRESENT_SRC（呈现要求，并写回布局追踪器）；
//   ⑥ 提交到呈现队列（与引擎绘制同一队列 ⇒ 队列内 FIFO 有序），栅栏等到完成后写 PNG。
//
// 【同步要点】
//   · 交换链图像是 EXCLUSIVE 共享模式、归呈现队列族所有 ⇒ 不涉及队列族所有权转移
//     （src/dstQueueFamilyIndex = IGNORED）；
//   · 拷贝命令提交到 `m_PresentQueue`，它同时也是绘制/呈现队列 —— 在同一个队列里
//     提交顺序即执行顺序，所以本帧绘制一定先于拷贝完成，无需额外等待信号量；
//   · 自建栅栏等到拷贝落地后再 Map（coherent 内存无需 invalidate）。
//
// 【布局为什么这样取】起点查 TextureLayoutTracker（render pass 边界会写回真实布局），
// 未记录时按 PRESENT_SRC（引擎颜色附件的 finalLayout 就是它）。终点固定回到
// PRESENT_SRC：vkQueuePresentKHR 要求该布局，而且这里紧接着就呈现。
// ============================================================

namespace {

/// 找一段满足 `typeBits` 且带 `want` 属性的内存类型；找不到返回 ~0u
u32 FindMemoryType(VkPhysicalDevice physical, u32 typeBits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties props{};
    vkGetPhysicalDeviceMemoryProperties(physical, &props);
    for (u32 i = 0; i < props.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) &&
            (props.memoryTypes[i].propertyFlags & want) == want) {
            return i;
        }
    }
    return ~0u;
}

/// 状态追踪器 → VkImageLayout（只覆盖交换链图像可能出现的状态；优先级与
/// VulkanCommandList.cpp 里那份映射保持一致：写状态优先）
VkImageLayout TrackedStateToLayout(ResourceState state) {
    const u32 s = u32(state);
    if (s & u32(ResourceState::RenderTarget))    return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    if (s & u32(ResourceState::ShaderResource))  return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if (s & u32(ResourceState::Present))         return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    if (s & u32(ResourceState::UnorderedAccess)) return VK_IMAGE_LAYOUT_GENERAL;
    if (s & u32(ResourceState::CopySrc))         return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    if (s & u32(ResourceState::CopyDst))         return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    return VK_IMAGE_LAYOUT_UNDEFINED;
}

} // namespace

void VulkanSwapChain::RequestCapture(u32 frameIndex, const char* path) {
    // 显式请求优先于环境变量：置 m_CaptureInit 后 CaptureIfRequested 不再读 env
    m_CaptureInit = true;
    m_CaptureAt   = static_cast<int>(frameIndex);
    m_CapturePath = (path && *path) ? path : std::string();
    const std::string out = m_CapturePath.empty()
                          ? rhi::DefaultCapturePath(frameIndex) : m_CapturePath;
    HE_CORE_INFO("[Vulkan 截图] 已按帧号请求截图：第 {} 帧 → {}", frameIndex, out);
}

void VulkanSwapChain::CaptureIfRequested(u32 imageIndex) {
    if (!m_CaptureInit) {
        m_CaptureInit = true;
        m_CaptureAt = rhi::CaptureFrameFromEnv();
        HE_CORE_INFO("[Vulkan 截图] HE_CAPTURE_FRAME={} (0/-1 表示不截图)", m_CaptureAt);
    }
    if (m_CaptureAt <= 0 || static_cast<int>(m_FrameCount) != m_CaptureAt) return;
    if (m_Swapchain == VK_NULL_HANDLE || m_Device == VK_NULL_HANDLE) return;
    if (imageIndex >= m_Images.size() || imageIndex >= m_ImageViews.size()) return;
    if (m_Width == 0 || m_Height == 0) return;

    const VkImage image = m_Images[imageIndex];
    const u32 w = m_Width;
    const u32 h = m_Height;
    const VkDeviceSize total = static_cast<VkDeviceSize>(w) * h * 4u;

    VkBuffer       buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkCommandPool  pool   = VK_NULL_HANDLE;
    VkCommandBuffer cmd   = VK_NULL_HANDLE;
    VkFence        fence  = VK_NULL_HANDLE;
    auto cleanup = [&] {
        if (fence)  vkDestroyFence(m_Device, fence, nullptr);
        if (cmd && pool) vkFreeCommandBuffers(m_Device, pool, 1, &cmd);
        if (pool)   vkDestroyCommandPool(m_Device, pool, nullptr);
        if (buffer) vkDestroyBuffer(m_Device, buffer, nullptr);
        if (memory) vkFreeMemory(m_Device, memory, nullptr);
    };

    // ① 读回缓冲（host-visible + coherent）
    VkBufferCreateInfo bi{};
    bi.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size        = total;
    bi.usage       = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(m_Device, &bi, nullptr, &buffer) != VK_SUCCESS) {
        HE_CORE_ERROR("[Vulkan 截图] 读回缓冲创建失败（{}x{}）", w, h);
        cleanup();
        return;
    }
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(m_Device, buffer, &req);
    const u32 memType = FindMemoryType(m_Physical, req.memoryTypeBits,
                                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (memType == ~0u) {
        HE_CORE_ERROR("[Vulkan 截图] 找不到 HOST_VISIBLE|HOST_COHERENT 内存类型");
        cleanup();
        return;
    }
    VkMemoryAllocateInfo ai{};
    ai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize  = req.size;
    ai.memoryTypeIndex = memType;
    if (vkAllocateMemory(m_Device, &ai, nullptr, &memory) != VK_SUCCESS ||
        vkBindBufferMemory(m_Device, buffer, memory, 0) != VK_SUCCESS) {
        HE_CORE_ERROR("[Vulkan 截图] 读回缓冲内存分配/绑定失败");
        cleanup();
        return;
    }

    // ② 临时命令池 + 命令缓冲
    VkCommandPoolCreateInfo pci{};
    pci.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pci.queueFamilyIndex = m_QueueFamily;
    if (vkCreateCommandPool(m_Device, &pci, nullptr, &pool) != VK_SUCCESS) {
        HE_CORE_ERROR("[Vulkan 截图] 命令池创建失败（队列族 {}）", m_QueueFamily);
        cleanup();
        return;
    }
    VkCommandBufferAllocateInfo cai{};
    cai.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cai.commandPool        = pool;
    cai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(m_Device, &cai, &cmd) != VK_SUCCESS) {
        HE_CORE_ERROR("[Vulkan 截图] 命令缓冲分配失败");
        cleanup();
        return;
    }

    // ③ 起点布局：优先取追踪到的真实布局
    void* view = reinterpret_cast<void*>(m_ImageViews[imageIndex]);
    VkImageLayout oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    ResourceState tracked;
    if (QueryTrackedTextureLayout(view, tracked)) {
        const VkImageLayout mapped = TrackedStateToLayout(tracked);
        if (mapped != VK_IMAGE_LAYOUT_UNDEFINED) oldLayout = mapped;
    } else {
        HE_CORE_INFO("[Vulkan 截图] 交换链图像尚无布局记录，按 PRESENT_SRC 处理（第 {} 帧）", m_FrameCount);
    }

    // ④ 录制：TRANSFER 前的布局转换 → 拷贝 → 转回 PRESENT_SRC
    VkCommandBufferBeginInfo cbbi{};
    cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cmd, &cbbi) != VK_SUCCESS) {
        HE_CORE_ERROR("[Vulkan 截图] 命令缓冲开始录制失败");
        cleanup();
        return;
    }

    VkImageMemoryBarrier b{};
    b.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask       = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT;
    b.oldLayout           = oldLayout;
    b.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image               = image;
    b.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);

    VkBufferImageCopy region{};
    region.bufferOffset      = 0;
    region.bufferRowLength   = 0;   // 0 = 紧凑排布（行距 = 宽度 × 4 字节）
    region.bufferImageHeight = 0;
    region.imageSubresource  = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.imageOffset       = { 0, 0, 0 };
    region.imageExtent       = { w, h, 1 };
    vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           buffer, 1, &region);

    b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b.dstAccessMask = 0;
    b.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.newLayout     = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);

    if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
        HE_CORE_ERROR("[Vulkan 截图] 命令缓冲录制失败");
        cleanup();
        return;
    }

    // ⑤ 提交到呈现队列 + 栅栏等待
    // 【同步】提交到与引擎绘制同一个队列：队列内 FIFO 有序 ⇒ 本帧绘制必然先完成，
    // 不需要额外等 acquire / render-complete 信号量；也不做 vkQueueWaitIdle
    //（那会多一次全队列停顿，而这里只需要等我们自己这一批）。
    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (vkCreateFence(m_Device, &fci, nullptr, &fence) != VK_SUCCESS) {
        HE_CORE_ERROR("[Vulkan 截图] 栅栏创建失败");
        cleanup();
        return;
    }
    VkSubmitInfo si{};
    si.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount   = 1;
    si.pCommandBuffers      = &cmd;
    const VkResult submitRes = vkQueueSubmit(m_PresentQueue, 1, &si, fence);
    if (submitRes != VK_SUCCESS) {
        // 最常见的是 VK_ERROR_DEVICE_LOST(-4)：设备已被 TDR 复位（另一处独立问题），
        // 这时截图必然拿不到内容，日志要能明确指出来而不是静默失败。
        HE_CORE_ERROR("[Vulkan 截图] 截图命令提交失败：VkResult={}（-4 = VK_ERROR_DEVICE_LOST）",
                      static_cast<int>(submitRes));
        cleanup();
        return;
    }
    constexpr u64 kTimeoutNs = 5ull * 1000ull * 1000ull * 1000ull;   // 5 秒
    const VkResult waitRes = vkWaitForFences(m_Device, 1, &fence, VK_TRUE, kTimeoutNs);
    if (waitRes != VK_SUCCESS) {
        HE_CORE_ERROR("[Vulkan 截图] 等待截图拷贝完成失败（VkResult={}）", static_cast<int>(waitRes));
        cleanup();
        return;
    }

    // 拷贝完成后图像停在 PRESENT_SRC：立刻写回追踪器，紧接着的呈现与下一帧都依赖它
    TrackTextureLayout(view, ResourceState::Present);

    // ⑥ Map → 拷成 RGBA8 → 写 PNG
    void* mapped = nullptr;
    if (vkMapMemory(m_Device, memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS || !mapped) {
        HE_CORE_ERROR("[Vulkan 截图] 映射读回缓冲失败");
        cleanup();
        return;
    }
    std::vector<unsigned char> rgba(static_cast<size_t>(w) * h * 4u);
    {
        const unsigned char* base = static_cast<const unsigned char*>(mapped);
        // 【通道顺序】交换链是 B8G8R8A8（默认），内存顺序 = B,G,R,A；WIC 的
        // GUID_WICPixelFormat32bppRGBA 编码器在本机按 BGRA 解读（见 RHI/ScreenCapture.h），
        // 因此这里**原样拷贝**，与 D3D12 后端的处理逐位一致，便于逐像素对比。
        // alpha 统一写 255（与 D3D12 相同）；HDR10 的 10-bit 打包不做色彩转换，两后端一致。
        for (u32 y = 0; y < h; ++y) {
            const unsigned char* s = base + static_cast<size_t>(y) * w * 4u;
            unsigned char* d = rgba.data() + static_cast<size_t>(y) * w * 4u;
            for (u32 x = 0; x < w; ++x) {
                d[x * 4 + 0] = s[x * 4 + 0];
                d[x * 4 + 1] = s[x * 4 + 1];
                d[x * 4 + 2] = s[x * 4 + 2];
                d[x * 4 + 3] = 255;
            }
        }
    }
    vkUnmapMemory(m_Device, memory);
    cleanup();

    const std::string path = m_CapturePath.empty()
                           ? rhi::DefaultCapturePath(m_FrameCount) : m_CapturePath;
    const bool ok = rhi::SaveRgbaToPng(path.c_str(), rgba.data(), w, h);
    HE_CORE_INFO("[Vulkan 截图] 第 {} 帧 {}x{} 保存{}: {}", m_FrameCount, w, h,
                 ok ? "成功" : "失败", path);
}

} // namespace he::rhi
