#include "RHI/RHI.h"
#include "RHI/SwapChain.h"
#include "RHI/CommandList.h"
#include "RHI/Buffer.h"
#include "RHI/Shader.h"
#include "Core/Log.h"
#include "Core/Assert.h"

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <vector>
#include <cstring>

namespace he::rhi {

// Forward declarations from VulkanResources.cpp
class VulkanBuffer;
class VulkanPipelineState;
std::unique_ptr<IRHIBuffer> CreateVulkanBuffer(VkDevice, VkPhysicalDevice, const BufferDesc&);
std::unique_ptr<IRHIPipelineState> CreateVulkanPipeline(VkDevice, const PipelineStateDesc&);

// ============================================================
// Vulkan swapchain
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

    VkSwapchainKHR GetHandle()     const { return m_Swapchain; }
    VkFormat       GetFormat()     const { return m_Format; }
    VkImageView    GetImageView(u32 i) const { return m_ImageViews[i]; }
    VkExtent2D     GetExtent()     const { return {m_Width, m_Height}; }
    VkImage        GetImage(u32 i) const { return m_Images[i]; }

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

    std::vector<VkImage>     m_Images;
    std::vector<VkImageView> m_ImageViews;
};

// ============================================================
// Vulkan command list
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
    void SetPipeline(IRHIPipelineState* pso) override;
    void SetVertexBuffer(IRHIBuffer* buffer, u32 binding) override;
    void SetViewport(const Viewport& vp) override;
    void SetScissor(const ScissorRect& sc) override;
    void Draw(u32 vertexCount, u32 instanceCount, u32 firstVertex, u32 firstInstance) override;
    void DrawIndexed(u32 indexCount, u32 instanceCount, u32 firstIndex,
                     i32 vertexOffset, u32 firstInstance) override;
    void Submit() override;

    // Vulkan-specific: set swapchain images for framebuffer creation
    void SetSwapchainViews(Span<VkImageView> views, VkExtent2D extent) {
        m_SwapchainViews.assign(views.begin(), views.end());
        m_SwapchainExtent = extent;
    }

    VkCommandBuffer GetHandle() const { return m_CmdBuffer; }

private:
    VkDevice         m_Device      = VK_NULL_HANDLE;
    VkQueue          m_Queue       = VK_NULL_HANDLE;
    u32              m_QueueFamily = 0;

    VkCommandPool    m_CmdPool     = VK_NULL_HANDLE;
    VkCommandBuffer  m_CmdBuffer   = VK_NULL_HANDLE;
    VkFence          m_Fence       = VK_NULL_HANDLE;

    // State tracking
    VkPipeline       m_CurrentPipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_CurrentLayout   = VK_NULL_HANDLE;
    VkRenderPass     m_CurrentRenderPass = VK_NULL_HANDLE;
    VkBuffer         m_CurrentVB       = VK_NULL_HANDLE;

    // Swapchain framebuffers
    std::vector<VkImageView> m_SwapchainViews;
    VkExtent2D               m_SwapchainExtent{};
    std::vector<VkFramebuffer> m_Framebuffers;
    u32                       m_CurrentImageIndex = 0;
};

// ============================================================
// Vulkan device
// ============================================================
class VulkanDevice final : public IRHIDevice {
public:
    Backend    GetBackend() const override { return Backend::Vulkan; }
    DeviceCaps GetCaps()    const override;

    void Initialize(const DeviceInitDesc& desc) override;
    void Shutdown() override;

    std::unique_ptr<IRHISwapChain>    CreateSwapChain(const SwapChainDesc& desc) override;
    std::unique_ptr<IRHICommandList>  CreateCommandList(QueueType queue) override;

    void WaitIdle() override;
    void Submit(IRHICommandList* cmdList) override;

    // Internal
    VkDevice         GetVkDevice()     const { return m_Device; }
    VkPhysicalDevice GetVkPhysical()   const { return m_Physical; }
    VkInstance       GetVkInstance()   const { return m_Instance; }
    VkSurfaceKHR     GetVkSurface()    const { return m_Surface; }
    VkQueue          GetGraphicsQueue() const { return m_GraphicsQueue; }
    u32              GetGraphicsFamily() const { return m_GraphicsFamily; }

private:
    void CreateSurface(void* windowHandle);
    void SelectPhysicalDevice();
    void CreateLogicalDevice();
    void CreateCommandPools();

    VkInstance       m_Instance       = VK_NULL_HANDLE;
    VkPhysicalDevice m_Physical       = VK_NULL_HANDLE;
    VkDevice         m_Device         = VK_NULL_HANDLE;
    VkSurfaceKHR     m_Surface        = VK_NULL_HANDLE;

    VkQueue          m_GraphicsQueue   = VK_NULL_HANDLE;
    u32              m_GraphicsFamily  = 0;

    VkCommandPool    m_GraphicsCmdPool = VK_NULL_HANDLE;
    VkCommandPool    m_ComputeCmdPool  = VK_NULL_HANDLE;

    VkDebugUtilsMessengerEXT m_DebugMessenger = VK_NULL_HANDLE;
    bool m_ValidationEnabled = false;
};

// ============================================================
// Helper: find queue family
// ============================================================
static u32 FindQueueFamily(VkPhysicalDevice physical, VkQueueFlags required, VkSurfaceKHR surface = VK_NULL_HANDLE) {
    u32 count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());

    for (u32 i = 0; i < count; ++i) {
        if ((families[i].queueFlags & required) == required) {
            if (surface) {
                VkBool32 present = VK_FALSE;
                vkGetPhysicalDeviceSurfaceSupportKHR(physical, i, surface, &present);
                if (present) return i;
            } else {
                return i;
            }
        }
    }
    return ~0u;
}

// ============================================================
// Vulkan Device Implementation
// ============================================================

DeviceCaps VulkanDevice::GetCaps() const {
    DeviceCaps caps;
    caps.maxBindlessResources = 1000000;
    caps.maxPushConstantsSize = 256;
    caps.supportsRayTracing   = true;
    caps.supportsMeshShaders  = true;
    caps.supportsVRS          = true;

    // Query actual limits
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(m_Physical, &props);
    caps.maxSamplerAnisotropy = static_cast<u32>(props.limits.maxSamplerAnisotropy);

    return caps;
}

void VulkanDevice::Initialize(const DeviceInitDesc& desc) {
    IRHIDevice::Initialize(desc);
    m_ValidationEnabled = desc.enableValidation;

    // 1. Create VkInstance
    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "HugEngine";
    appInfo.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    appInfo.pEngineName = "HugEngine";
    appInfo.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    appInfo.apiVersion = VK_API_VERSION_1_3;

    std::vector<const char*> instanceExtensions = {
        VK_KHR_SURFACE_EXTENSION_NAME,
#ifdef VK_USE_PLATFORM_WIN32_KHR
        VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
#endif
    };
    std::vector<const char*> validationLayers;

    if (m_ValidationEnabled) {
        instanceExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        validationLayers.push_back("VK_LAYER_KHRONOS_validation");
    }

    VkInstanceCreateInfo instanceInfo{};
    instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instanceInfo.pApplicationInfo = &appInfo;
    instanceInfo.enabledExtensionCount = static_cast<u32>(instanceExtensions.size());
    instanceInfo.ppEnabledExtensionNames = instanceExtensions.data();
    instanceInfo.enabledLayerCount = static_cast<u32>(validationLayers.size());
    instanceInfo.ppEnabledLayerNames = validationLayers.data();

    VkResult result = vkCreateInstance(&instanceInfo, nullptr, &m_Instance);
    HE_ASSERT(result == VK_SUCCESS, "Failed to create Vulkan instance");

    HE_CORE_INFO("Vulkan instance created");

    // 2. Create surface
    CreateSurface(desc.windowHandle);

    // 3. Select physical device
    SelectPhysicalDevice();

    // 4. Create logical device
    CreateLogicalDevice();

    // 5. Create command pools
    CreateCommandPools();

    HE_CORE_INFO("Vulkan device fully initialized");
}

void VulkanDevice::Shutdown() {
    WaitIdle();

    if (m_GraphicsCmdPool) { vkDestroyCommandPool(m_Device, m_GraphicsCmdPool, nullptr); }
    if (m_ComputeCmdPool)  { vkDestroyCommandPool(m_Device, m_ComputeCmdPool, nullptr); }
    if (m_Device)          { vkDestroyDevice(m_Device, nullptr); }
    if (m_Surface)         { vkDestroySurfaceKHR(m_Instance, m_Surface, nullptr); }
    if (m_Instance)        { vkDestroyInstance(m_Instance, nullptr); }

    HE_CORE_INFO("Vulkan device destroyed");
}

void VulkanDevice::CreateSurface(void* windowHandle) {
#if defined(VK_USE_PLATFORM_WIN32_KHR)
    VkWin32SurfaceCreateInfoKHR surfaceInfo{};
    surfaceInfo.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    surfaceInfo.hinstance = GetModuleHandle(nullptr);
    surfaceInfo.hwnd = static_cast<HWND>(windowHandle);

    VkResult result = vkCreateWin32SurfaceKHR(m_Instance, &surfaceInfo, nullptr, &m_Surface);
    HE_ASSERT(result == VK_SUCCESS, "Failed to create Win32 surface");
    HE_CORE_INFO("Vulkan Win32 surface created");
#else
    HE_ASSERT(false, "Only Win32 surface supported for now");
#endif
}

void VulkanDevice::SelectPhysicalDevice() {
    u32 count = 0;
    vkEnumeratePhysicalDevices(m_Instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(m_Instance, &count, devices.data());

    HE_ASSERT(count > 0, "No Vulkan-capable GPU found");

    // Select first discrete GPU
    m_Physical = devices[0];
    for (auto& dev : devices) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(dev, &props);
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            m_Physical = dev;
            HE_CORE_INFO("Selected discrete GPU: {}", props.deviceName);
            return;
        }
    }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(m_Physical, &props);
    HE_CORE_INFO("Using GPU: {}", props.deviceName);
}

void VulkanDevice::CreateLogicalDevice() {
    m_GraphicsFamily = FindQueueFamily(m_Physical,
        VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT, m_Surface);
    HE_ASSERT(m_GraphicsFamily != ~0u, "No suitable queue family found");

    float queuePriority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{};
    queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfo.queueFamilyIndex = m_GraphicsFamily;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &queuePriority;

    std::vector<const char*> deviceExtensions = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
    };

    VkPhysicalDeviceFeatures features{};

    VkDeviceCreateInfo deviceInfo{};
    deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    deviceInfo.enabledExtensionCount = static_cast<u32>(deviceExtensions.size());
    deviceInfo.ppEnabledExtensionNames = deviceExtensions.data();
    deviceInfo.pEnabledFeatures = &features;

    VkResult result = vkCreateDevice(m_Physical, &deviceInfo, nullptr, &m_Device);
    HE_ASSERT(result == VK_SUCCESS, "Failed to create logical device");

    vkGetDeviceQueue(m_Device, m_GraphicsFamily, 0, &m_GraphicsQueue);
    HE_CORE_INFO("Logical device + graphics queue created");
}

void VulkanDevice::CreateCommandPools() {
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.queueFamilyIndex = m_GraphicsFamily;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

    vkCreateCommandPool(m_Device, &poolInfo, nullptr, &m_GraphicsCmdPool);

    poolInfo.queueFamilyIndex = m_GraphicsFamily; // Use same family for compute
    vkCreateCommandPool(m_Device, &poolInfo, nullptr, &m_ComputeCmdPool);
}

std::unique_ptr<IRHISwapChain> VulkanDevice::CreateSwapChain(const SwapChainDesc& desc) {
    return std::make_unique<VulkanSwapChain>(m_Device, m_Physical, m_Surface,
                                            m_GraphicsQueue, desc);
}

std::unique_ptr<IRHICommandList> VulkanDevice::CreateCommandList(QueueType queue) {
    return std::make_unique<VulkanCommandList>(m_Device, m_GraphicsQueue, m_GraphicsFamily);
}

void VulkanDevice::WaitIdle() {
    if (m_Device) vkDeviceWaitIdle(m_Device);
}

void VulkanDevice::Submit(IRHICommandList* cmdList) {
    auto* vulkanCmd = static_cast<VulkanCommandList*>(cmdList);
    vulkanCmd->Submit();
}

// ============================================================
// Vulkan SwapChain Implementation
// ============================================================

VulkanSwapChain::VulkanSwapChain(VkDevice device, VkPhysicalDevice physical,
                                 VkSurfaceKHR surface, VkQueue presentQueue,
                                 const SwapChainDesc& desc)
    : m_Device(device), m_Physical(physical), m_Surface(surface)
    , m_PresentQueue(presentQueue)
    , m_Width(desc.width), m_Height(desc.height)
{
    CreateSwapchain();
    HE_CORE_INFO("Vulkan swapchain created: {}x{}", m_Width, m_Height);
}

VulkanSwapChain::~VulkanSwapChain() {
    DestroySwapchain();
}

void VulkanSwapChain::CreateSwapchain() {
    // Query surface capabilities
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_Physical, m_Surface, &caps);

    u32 formatCount;
    vkGetPhysicalDeviceSurfaceFormatsKHR(m_Physical, m_Surface, &formatCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(m_Physical, m_Surface, &formatCount, formats.data());

    // Pick BGRA8 sRGB or fallback to first format
    m_Format = VK_FORMAT_B8G8R8A8_UNORM;
    for (auto& f : formats) {
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            m_Format = f.format;
            break;
        }
    }

    u32 presentModeCount;
    vkGetPhysicalDeviceSurfacePresentModesKHR(m_Physical, m_Surface, &presentModeCount, nullptr);
    std::vector<VkPresentModeKHR> presentModes(presentModeCount);
    vkGetPhysicalDeviceSurfacePresentModesKHR(m_Physical, m_Surface, &presentModeCount, presentModes.data());

    VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR; // Always available
    for (auto& mode : presentModes) {
        if (mode == VK_PRESENT_MODE_MAILBOX_KHR) {
            presentMode = mode;
            break;
        }
    }

    m_Width  = std::clamp(m_Width,  caps.minImageExtent.width,  caps.maxImageExtent.width);
    m_Height = std::clamp(m_Height, caps.minImageExtent.height, caps.maxImageExtent.height);

    m_ImageCount = std::max(3u, caps.minImageCount);
    if (caps.maxImageCount > 0) m_ImageCount = std::min(m_ImageCount, caps.maxImageCount);

    VkSwapchainCreateInfoKHR swapInfo{};
    swapInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    swapInfo.surface = m_Surface;
    swapInfo.minImageCount = m_ImageCount;
    swapInfo.imageFormat = m_Format;
    swapInfo.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    swapInfo.imageExtent = {m_Width, m_Height};
    swapInfo.imageArrayLayers = 1;
    swapInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    swapInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    swapInfo.preTransform = caps.currentTransform;
    swapInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    swapInfo.presentMode = presentMode;
    swapInfo.clipped = VK_TRUE;

    VkResult result = vkCreateSwapchainKHR(m_Device, &swapInfo, nullptr, &m_Swapchain);
    HE_ASSERT(result == VK_SUCCESS, "Failed to create swapchain");

    // Get images
    vkGetSwapchainImagesKHR(m_Device, m_Swapchain, &m_ImageCount, nullptr);
    m_Images.resize(m_ImageCount);
    vkGetSwapchainImagesKHR(m_Device, m_Swapchain, &m_ImageCount, m_Images.data());

    // Create image views
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
}

void VulkanSwapChain::DestroySwapchain() {
    for (auto& view : m_ImageViews) vkDestroyImageView(m_Device, view, nullptr);
    m_ImageViews.clear();
    if (m_Swapchain) vkDestroySwapchainKHR(m_Device, m_Swapchain, nullptr);
    m_Swapchain = VK_NULL_HANDLE;
}

void VulkanSwapChain::Resize(u32 width, u32 height) {
    vkDeviceWaitIdle(m_Device);
    DestroySwapchain();
    m_Width = width;
    m_Height = height;
    CreateSwapchain();
}

bool VulkanSwapChain::AcquireNextImage() {
    VkSemaphore semaphore = VK_NULL_HANDLE; // Placeholder
    VkResult result = vkAcquireNextImageKHR(m_Device, m_Swapchain, UINT64_MAX,
                                            semaphore, VK_NULL_HANDLE, &m_CurrentImage);
    return result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR;
}

void VulkanSwapChain::Present(bool /*vsync*/) {
    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &m_Swapchain;
    presentInfo.pImageIndices = &m_CurrentImage;

    vkQueuePresentKHR(m_PresentQueue, &presentInfo);
}

// ============================================================
// Vulkan CommandList Implementation
// ============================================================

VulkanCommandList::VulkanCommandList(VkDevice device, VkQueue queue, u32 queueFamily)
    : m_Device(device), m_Queue(queue), m_QueueFamily(queueFamily)
{
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.queueFamilyIndex = m_QueueFamily;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    vkCreateCommandPool(m_Device, &poolInfo, nullptr, &m_CmdPool);

    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = m_CmdPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;
    vkAllocateCommandBuffers(m_Device, &allocInfo, &m_CmdBuffer);

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    vkCreateFence(m_Device, &fenceInfo, nullptr, &m_Fence);
}

VulkanCommandList::~VulkanCommandList() {
    vkDeviceWaitIdle(m_Device);
    for (auto& fb : m_Framebuffers) vkDestroyFramebuffer(m_Device, fb, nullptr);
    if (m_Fence)     vkDestroyFence(m_Device, m_Fence, nullptr);
    if (m_CmdPool)   vkDestroyCommandPool(m_Device, m_CmdPool, nullptr);
}

void VulkanCommandList::Begin() {
    vkResetCommandPool(m_Device, m_CmdPool, 0);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(m_CmdBuffer, &beginInfo);
}

void VulkanCommandList::End() {
    vkEndCommandBuffer(m_CmdBuffer);
}

void VulkanCommandList::BeginRenderPass(u32, Format, Format,
                                        const ClearValue* clear) {
    if (m_SwapchainViews.empty() || !m_CurrentRenderPass) {
        HE_CORE_ERROR("BeginRenderPass: no swapchain views or render pass set");
        return;
    }

    // Create framebuffers lazily
    if (m_Framebuffers.empty()) {
        u32 count = static_cast<u32>(m_SwapchainViews.size());
        m_Framebuffers.resize(count);
        for (u32 i = 0; i < count; ++i) {
            VkImageView attachments[] = { m_SwapchainViews[i] };

            VkFramebufferCreateInfo fbInfo{};
            fbInfo.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            fbInfo.renderPass      = m_CurrentRenderPass;
            fbInfo.attachmentCount = 1;
            fbInfo.pAttachments    = attachments;
            fbInfo.width           = m_SwapchainExtent.width;
            fbInfo.height          = m_SwapchainExtent.height;
            fbInfo.layers          = 1;

            vkCreateFramebuffer(m_Device, &fbInfo, nullptr, &m_Framebuffers[i]);
        }
    }

    VkClearValue vkClear{};
    if (clear) {
        vkClear.color.float32[0] = clear->color[0];
        vkClear.color.float32[1] = clear->color[1];
        vkClear.color.float32[2] = clear->color[2];
        vkClear.color.float32[3] = clear->color[3];
    } else {
        vkClear.color.float32[0] = 0.1f;
        vkClear.color.float32[1] = 0.1f;
        vkClear.color.float32[2] = 0.15f;
        vkClear.color.float32[3] = 1.0f;
    }

    VkRenderPassBeginInfo rpBegin{};
    rpBegin.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpBegin.renderPass        = m_CurrentRenderPass;
    rpBegin.framebuffer       = m_Framebuffers[m_CurrentImageIndex];
    rpBegin.renderArea.extent = m_SwapchainExtent;
    rpBegin.clearValueCount   = 1;
    rpBegin.pClearValues      = &vkClear;

    vkCmdBeginRenderPass(m_CmdBuffer, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);

    // Bind pipeline if set
    if (m_CurrentPipeline) {
        vkCmdBindPipeline(m_CmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_CurrentPipeline);
    }

    // Bind vertex buffer if set
    if (m_CurrentVB) {
        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(m_CmdBuffer, 0, 1, &m_CurrentVB, &offset);
    }
}

void VulkanCommandList::EndRenderPass() {
    vkCmdEndRenderPass(m_CmdBuffer);
}

void VulkanCommandList::SetViewport(const Viewport& vp) {
    VkViewport vkViewport{};
    vkViewport.x = vp.x;
    vkViewport.y = vp.y;
    vkViewport.width = vp.width;
    vkViewport.height = vp.height;
    vkViewport.minDepth = vp.minDepth;
    vkViewport.maxDepth = vp.maxDepth;
    vkCmdSetViewport(m_CmdBuffer, 0, 1, &vkViewport);
}

void VulkanCommandList::SetScissor(const ScissorRect& sc) {
    VkRect2D vkScissor{};
    vkScissor.offset = {sc.x, sc.y};
    vkScissor.extent = {sc.width, sc.height};
    vkCmdSetScissor(m_CmdBuffer, 0, 1, &vkScissor);
}

void VulkanCommandList::SetPipeline(IRHIPipelineState* pso) {
    auto* vkPso = static_cast<VulkanPipelineState*>(pso);
    m_CurrentPipeline   = vkPso->GetPipeline();
    m_CurrentLayout     = vkPso->GetPipelineLayout();
    m_CurrentRenderPass = vkPso->GetRenderPass();

    // Invalidate framebuffers since render pass changed
    for (auto& fb : m_Framebuffers)
        vkDestroyFramebuffer(m_Device, fb, nullptr);
    m_Framebuffers.clear();
}

void VulkanCommandList::SetCurrentImageIndex(u32 index) {
    m_CurrentImageIndex = index;
}

void VulkanCommandList::SetVertexBuffer(IRHIBuffer* buffer, u32 /*binding*/) {
    auto* vkBuf = static_cast<VulkanBuffer*>(buffer);
    m_CurrentVB = vkBuf->GetHandle();
}

void VulkanCommandList::Draw(u32 vertexCount, u32 instanceCount,
                              u32 firstVertex, u32 firstInstance) {
    vkCmdDraw(m_CmdBuffer, vertexCount, instanceCount, firstVertex, firstInstance);
}

void VulkanCommandList::DrawIndexed(u32 indexCount, u32 instanceCount,
                                     u32 firstIndex, i32 vertexOffset, u32 firstInstance) {
    vkCmdDrawIndexed(m_CmdBuffer, indexCount, instanceCount,
                     firstIndex, vertexOffset, firstInstance);
}

void VulkanCommandList::Submit() {
    vkResetFences(m_Device, 1, &m_Fence);

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &m_CmdBuffer;

    vkQueueSubmit(m_Queue, 1, &submitInfo, m_Fence);
    vkWaitForFences(m_Device, 1, &m_Fence, VK_TRUE, UINT64_MAX);
}

// ============================================================
// Vulkan device: CreateBuffer / CreatePipelineState (impl in VulkanResources.cpp)
// ============================================================

// Override IRHIDevice methods
std::unique_ptr<IRHIBuffer> VulkanDevice::CreateBuffer(const BufferDesc& desc) {
    return CreateVulkanBuffer(m_Device, m_Physical, desc);
}

std::unique_ptr<IRHIPipelineState> VulkanDevice::CreatePipelineState(const PipelineStateDesc& desc) {
    return CreateVulkanPipeline(m_Device, desc);
}

std::unique_ptr<IRHITexture> VulkanDevice::CreateTexture(const TextureDesc&) {
    HE_CORE_WARN("CreateTexture not yet implemented");
    return nullptr;
}

// ============================================================
// Factory
// ============================================================

std::unique_ptr<IRHIDevice> CreateDevice(Backend backend) {
    switch (backend) {
        case Backend::Vulkan:
            return std::make_unique<VulkanDevice>();
        case Backend::D3D12:
            HE_CORE_WARN("D3D12 backend not yet implemented");
            return nullptr;
        default:
            return nullptr;
    }
}

} // namespace he::rhi
