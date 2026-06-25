#include "RHI/RHI.h"
#include "RHI/SwapChain.h"
#include "RHI/CommandList.h"
#include "RHI/Buffer.h"
#include "RHI/Shader.h"
#include "Core/Log.h"
#include "Core/Assert.h"

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include <algorithm>
#include <vector>
#include <cstring>

// VulkanSwapChain/VulkanCommandList + 其他 Vulkan 类型完整定义
#include "VulkanInternal.h"

namespace he::rhi {

// 工厂函数（实现在 VulkanResources.cpp）
std::unique_ptr<IRHIBuffer>        CreateVulkanBuffer(VkDevice, VkPhysicalDevice, const BufferDesc&);
std::unique_ptr<IRHITexture>       CreateVulkanTexture(VkDevice, VkPhysicalDevice, VkCommandPool, VkQueue, const TextureDesc&);
std::unique_ptr<IRHISampler>       CreateVulkanSampler(VkDevice, const SamplerDesc&);
std::unique_ptr<IRHIPipelineState> CreateVulkanPipeline(VkDevice, const PipelineStateDesc&,
    const std::vector<VkDescriptorSetLayout>& descLayouts);

} // namespace he::rhi

namespace he::rhi {

// ============================================================
// Vulkan device
// ============================================================
class VulkanDevice final : public IRHIDevice {
public:
    ~VulkanDevice() override { Shutdown(); }
    Backend    GetBackend() const override { return Backend::Vulkan; }
    DeviceCaps GetCaps()    const override;

    void Initialize(const DeviceInitDesc& desc) override;
    void Shutdown() override;

    std::unique_ptr<IRHISwapChain>    CreateSwapChain(const SwapChainDesc& desc) override;
    std::unique_ptr<IRHICommandList>  CreateCommandList(QueueType queue) override;
    std::unique_ptr<IRHIBuffer>       CreateBuffer(const BufferDesc& desc) override;
    std::unique_ptr<IRHITexture>      CreateTexture(const TextureDesc& desc) override;
    std::unique_ptr<IRHISampler>      CreateSampler(const SamplerDesc& desc) override;
    std::unique_ptr<IRHIPipelineState> CreatePipelineState(const PipelineStateDesc& desc) override;

    void WaitIdle() override;
    void Submit(IRHICommandList* cmdList) override;

    // Descriptor Sets
    DescriptorSetLayoutHandle CreateDescriptorSetLayout(const DescriptorSetLayoutDesc& desc) override;
    DescriptorSetHandle       AllocateDescriptorSet(DescriptorSetLayoutHandle layout) override;
    void                      UpdateDescriptorSet(DescriptorSetHandle set, u32 binding,
                                                  DescriptorType type, IRHIBuffer* buffer) override;
    void                      DestroyDescriptorSetLayout(DescriptorSetLayoutHandle layout) override;

    // Internal
    VkDevice         GetVkDevice()     const { return m_Device; }
    VkPhysicalDevice GetVkPhysical()   const { return m_Physical; }
    VkInstance       GetVkInstance()   const { return m_Instance; }
    VkSurfaceKHR     GetVkSurface()    const { return m_Surface; }
    VkQueue          GetGraphicsQueue() const { return m_GraphicsQueue; }
    VkCommandPool    GetGraphicsCmdPool() const { return m_GraphicsCmdPool; }
    u32              GetGraphicsFamily() const { return m_GraphicsFamily; }

    // Descriptor set handle 解析（供 VulkanCommandList 使用）
    VkDescriptorSet ResolveDescriptorSet(DescriptorSetHandle h) const {
        if (h == 0 || h > m_DescSets.size()) return VK_NULL_HANDLE;
        return m_DescSets[static_cast<usize>(h - 1)];
    }
    VkDescriptorSetLayout ResolveDescriptorSetLayout(DescriptorSetLayoutHandle h) const {
        if (h == 0 || h > m_DescSetLayouts.size()) return VK_NULL_HANDLE;
        return m_DescSetLayouts[static_cast<usize>(h - 1)];
    }

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

    // Descriptor set management
    VkDescriptorPool                  m_DescPool = VK_NULL_HANDLE;
    std::vector<VkDescriptorSetLayout> m_DescSetLayouts;
    std::vector<VkDescriptorSet>       m_DescSets;
    std::vector<DescriptorSetLayoutHandle> m_DescSetLayoutParents;  // layout handle per set

    void EnsureDescriptorPool();
    VkDescriptorType ToVkDescType(DescriptorType type) const;
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

    // 2. 设置调试回调（在创建 instance 后立即设置）
    if (m_ValidationEnabled) {
        VkDebugUtilsMessengerCreateInfoEXT debugInfo{};
        debugInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        debugInfo.messageSeverity =
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        debugInfo.messageType =
            VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        debugInfo.pfnUserCallback = [](
            VkDebugUtilsMessageSeverityFlagBitsEXT severity,
            VkDebugUtilsMessageTypeFlagsEXT,
            const VkDebugUtilsMessengerCallbackDataEXT* data,
            void*) -> VkBool32 {
            if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
                HE_CORE_WARN("[Vulkan] {}", data->pMessage);
            }
            return VK_FALSE;
        };
        auto pfnCreate = (PFN_vkCreateDebugUtilsMessengerEXT)
            vkGetInstanceProcAddr(m_Instance, "vkCreateDebugUtilsMessengerEXT");
        if (pfnCreate) {
            pfnCreate(m_Instance, &debugInfo, nullptr, &m_DebugMessenger);
            HE_CORE_INFO("Vulkan debug messenger attached");
        }
    }

    // 3. Create surface
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
    // 清理 Descriptor Set Layouts
    for (auto& layout : m_DescSetLayouts)
        vkDestroyDescriptorSetLayout(m_Device, layout, nullptr);
    m_DescSetLayouts.clear();
    m_DescSets.clear();
    m_DescSetLayoutParents.clear();

    if (m_DescPool)        { vkDestroyDescriptorPool(m_Device, m_DescPool, nullptr); m_DescPool = VK_NULL_HANDLE; }
    if (m_GraphicsCmdPool) { vkDestroyCommandPool(m_Device, m_GraphicsCmdPool, nullptr); m_GraphicsCmdPool = VK_NULL_HANDLE; }
    if (m_ComputeCmdPool)  { vkDestroyCommandPool(m_Device, m_ComputeCmdPool, nullptr); m_ComputeCmdPool = VK_NULL_HANDLE; }
    if (m_Device)          { vkDestroyDevice(m_Device, nullptr); m_Device = VK_NULL_HANDLE; }
    if (m_Surface)         { vkDestroySurfaceKHR(m_Instance, m_Surface, nullptr); m_Surface = VK_NULL_HANDLE; }
    if (m_Instance)        { vkDestroyInstance(m_Instance, nullptr); m_Instance = VK_NULL_HANDLE; }

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

    // 启用 bufferDeviceAddress（缓冲设备地址查询需要）
    VkPhysicalDeviceBufferDeviceAddressFeatures addrFeature{};
    addrFeature.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;
    addrFeature.bufferDeviceAddress = VK_TRUE;

    VkPhysicalDeviceFeatures features{};

    VkDeviceCreateInfo deviceInfo{};
    deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceInfo.pNext = &addrFeature;
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
    return std::make_unique<VulkanCommandList>(m_Device, m_GraphicsQueue,
                                               m_GraphicsFamily, this);
}

void VulkanDevice::WaitIdle() {
    if (m_Device) vkDeviceWaitIdle(m_Device);
}

void VulkanDevice::Submit(IRHICommandList* cmdList) {
    auto* vulkanCmd = static_cast<VulkanCommandList*>(cmdList);
    vulkanCmd->Submit();
}

// ============================================================
// Descriptor Set Implementation
// ============================================================

VkDescriptorType VulkanDevice::ToVkDescType(DescriptorType type) const {
    switch (type) {
        case DescriptorType::UniformBuffer:         return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        case DescriptorType::StorageBuffer:         return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        case DescriptorType::CombinedImageSampler:  return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        case DescriptorType::StorageImage:          return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        case DescriptorType::Sampler:               return VK_DESCRIPTOR_TYPE_SAMPLER;
        default:                                    return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    }
}

void VulkanDevice::EnsureDescriptorPool() {
    if (m_DescPool != VK_NULL_HANDLE) return;

    VkDescriptorPoolSize poolSizes[] = {
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 16 },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 16 },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 256 },
    };

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets       = 64;
    poolInfo.poolSizeCount = 3;
    poolInfo.pPoolSizes    = poolSizes;
    poolInfo.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;

    vkCreateDescriptorPool(m_Device, &poolInfo, nullptr, &m_DescPool);
}

DescriptorSetLayoutHandle VulkanDevice::CreateDescriptorSetLayout(const DescriptorSetLayoutDesc& desc) {
    std::vector<VkDescriptorSetLayoutBinding> vkBindings;
    for (auto& b : desc.bindings) {
        VkDescriptorSetLayoutBinding vb{};
        vb.binding            = b.binding;
        vb.descriptorType     = ToVkDescType(b.type);
        vb.descriptorCount    = b.count;
        vb.stageFlags         = 0;
        if (u8(b.stageFlags) & 1) vb.stageFlags |= VK_SHADER_STAGE_VERTEX_BIT;
        // Match by ShaderStage enum values
        switch (b.stageFlags) {
            case ShaderStage::Vertex:   vb.stageFlags = VK_SHADER_STAGE_VERTEX_BIT; break;
            case ShaderStage::Pixel:    vb.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT; break;
            case ShaderStage::Compute:  vb.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT; break;
            default:
                vb.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
                break;
        }
        vkBindings.push_back(vb);
    }

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<u32>(vkBindings.size());
    layoutInfo.pBindings    = vkBindings.data();

    VkDescriptorSetLayout layout;
    vkCreateDescriptorSetLayout(m_Device, &layoutInfo, nullptr, &layout);

    DescriptorSetLayoutHandle handle = static_cast<DescriptorSetLayoutHandle>(m_DescSetLayouts.size() + 1);
    m_DescSetLayouts.push_back(layout);
    return handle;
}

DescriptorSetHandle VulkanDevice::AllocateDescriptorSet(DescriptorSetLayoutHandle layoutHandle) {
    EnsureDescriptorPool();
    if (layoutHandle == 0 || layoutHandle > m_DescSetLayouts.size()) return kInvalidSet;

    VkDescriptorSetLayout layout = m_DescSetLayouts[static_cast<usize>(layoutHandle - 1)];

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool     = m_DescPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts        = &layout;

    VkDescriptorSet ds;
    VkResult result = vkAllocateDescriptorSets(m_Device, &allocInfo, &ds);
    if (result != VK_SUCCESS) return kInvalidSet;

    DescriptorSetHandle handle = static_cast<DescriptorSetHandle>(m_DescSets.size() + 1);
    m_DescSets.push_back(ds);
    m_DescSetLayoutParents.push_back(layoutHandle);
    return handle;
}

void VulkanDevice::UpdateDescriptorSet(DescriptorSetHandle setHandle, u32 binding,
                                        DescriptorType type, IRHIBuffer* buffer) {
    if (setHandle == 0 || setHandle > m_DescSets.size()) return;
    VkDescriptorSet ds = m_DescSets[static_cast<usize>(setHandle - 1)];

    auto* vkBuf = static_cast<VulkanBuffer*>(buffer);
    VkDescriptorBufferInfo bufInfo{};
    bufInfo.buffer = vkBuf->GetHandle();
    bufInfo.offset = 0;
    bufInfo.range  = vkBuf->GetSize();

    VkWriteDescriptorSet write{};
    write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet          = ds;
    write.dstBinding      = binding;
    write.dstArrayElement = 0;
    write.descriptorCount = 1;
    write.descriptorType  = ToVkDescType(type);
    write.pBufferInfo     = &bufInfo;

    vkUpdateDescriptorSets(m_Device, 1, &write, 0, nullptr);
}

void VulkanDevice::DestroyDescriptorSetLayout(DescriptorSetLayoutHandle handle) {
    if (handle == 0 || handle > m_DescSetLayouts.size()) return;
    VkDescriptorSetLayout layout = m_DescSetLayouts[static_cast<usize>(handle - 1)];
    vkDestroyDescriptorSetLayout(m_Device, layout, nullptr);
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

    // 创建同步原语
    VkSemaphoreCreateInfo semInfo{};
    semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    vkCreateSemaphore(m_Device, &semInfo, nullptr, &m_ImageAcquired);
    vkCreateSemaphore(m_Device, &semInfo, nullptr, &m_RenderComplete);

    // 创建深度模板纹理
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
    if (m_ImageAcquired)  { vkDestroySemaphore(m_Device, m_ImageAcquired, nullptr);  m_ImageAcquired  = VK_NULL_HANDLE; }
    if (m_RenderComplete) { vkDestroySemaphore(m_Device, m_RenderComplete, nullptr); m_RenderComplete = VK_NULL_HANDLE; }
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
    VkResult result = vkAcquireNextImageKHR(m_Device, m_Swapchain, UINT64_MAX,
                                            m_ImageAcquired, VK_NULL_HANDLE, &m_CurrentImage);
    return result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR;
}

void VulkanSwapChain::Present(bool /*vsync*/) {
    // 纯 Fence 同步：GPU 完成由 Begin 中的 vkWaitForFences 保证
    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains     = &m_Swapchain;
    presentInfo.pImageIndices   = &m_CurrentImage;

    vkQueuePresentKHR(m_PresentQueue, &presentInfo);
}

// ============================================================
// Vulkan CommandList Implementation
// ============================================================

VulkanCommandList::VulkanCommandList(VkDevice device, VkQueue queue, u32 queueFamily,
                                     VulkanDevice* vulkanDevice)
    : m_Device(device), m_Queue(queue), m_QueueFamily(queueFamily)
    , m_VulkanDevice(vulkanDevice)
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
    // 直接重置命令缓冲（GPU 在上一帧 Submit 中已同步完成）
    vkResetCommandBuffer(m_CmdBuffer, 0);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(m_CmdBuffer, &beginInfo);
}

void VulkanCommandList::End() {
    vkEndCommandBuffer(m_CmdBuffer);
}

void VulkanCommandList::BeginRenderPass(u32 colorCount, Format, Format depthFormat,
                                        const ClearValue* clear) {
    // 从 SwapChain 获取当前图像索引
    if (m_pSwapChain)
        m_CurrentImageIndex = m_pSwapChain->GetCurrentBackBufferIndex();

    if (m_SwapchainViews.empty() || !m_CurrentRenderPass) {
        HE_CORE_ERROR("BeginRenderPass: no swapchain views or render pass set");
        return;
    }

    // 创建 Framebuffer（颜色 + 深度附件）
    if (m_Framebuffers.empty()) {
        u32 count = static_cast<u32>(m_SwapchainViews.size());
        m_Framebuffers.resize(count);
        for (u32 i = 0; i < count; ++i) {
            VkImageView attachments[2] = { m_SwapchainViews[i] };
            u32 attachmentCount = 1;

            // 如果 SwapChain 有深度纹理，作为第二个附件
            if (m_pSwapChain && m_pSwapChain->GetDepthImageView()) {
                attachments[1] = m_pSwapChain->GetDepthImageView();
                attachmentCount = 2;
            }

            VkFramebufferCreateInfo fbInfo{};
            fbInfo.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            fbInfo.renderPass      = m_CurrentRenderPass;
            fbInfo.attachmentCount = attachmentCount;
            fbInfo.pAttachments    = attachments;
            fbInfo.width           = m_SwapchainExtent.width;
            fbInfo.height          = m_SwapchainExtent.height;
            fbInfo.layers          = 1;

            vkCreateFramebuffer(m_Device, &fbInfo, nullptr, &m_Framebuffers[i]);
        }
    }

    // 构建清除值（颜色 + 深度，始终 2 个以匹配 RenderPass 附件数）
    VkClearValue vkClearValues[2]{};
    if (clear) {
        vkClearValues[0].color.float32[0] = clear[0].color[0];
        vkClearValues[0].color.float32[1] = clear[0].color[1];
        vkClearValues[0].color.float32[2] = clear[0].color[2];
        vkClearValues[0].color.float32[3] = clear[0].color[3];
    } else {
        vkClearValues[0].color.float32[0] = 0.1f;
        vkClearValues[0].color.float32[1] = 0.1f;
        vkClearValues[0].color.float32[2] = 0.15f;
        vkClearValues[0].color.float32[3] = 1.0f;
    }
    vkClearValues[1].depthStencil.depth   = 1.0f;
    vkClearValues[1].depthStencil.stencil = 0;

    VkRenderPassBeginInfo rpBegin{};
    rpBegin.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpBegin.renderPass        = m_CurrentRenderPass;
    rpBegin.framebuffer       = m_Framebuffers[m_CurrentImageIndex];
    rpBegin.renderArea.extent = m_SwapchainExtent;
    rpBegin.clearValueCount   = 2;
    rpBegin.pClearValues      = vkClearValues;

    vkCmdBeginRenderPass(m_CmdBuffer, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);

    // 绑定管线
    if (m_CurrentPipeline) {
        vkCmdBindPipeline(m_CmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_CurrentPipeline);
    }
    // 顶点/索引缓冲在 Draw/DrawIndexed 时按需绑定，以支持正确的 binding 参数
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

void VulkanCommandList::SetSwapChain(IRHISwapChain* swapchain) {
    // 保存 Vulkan SwapChain 指针，后续 BeginRenderPass/Submit 自动使用
    m_pSwapChain = static_cast<VulkanSwapChain*>(swapchain);

    // 预创建 Framebuffer 用的 ImageView 列表
    u32 count = 3;
    m_SwapchainViews.resize(count);
    for (u32 i = 0; i < count; ++i)
        m_SwapchainViews[i] = m_pSwapChain->GetImageView(i);
    m_SwapchainExtent = {m_pSwapChain->GetWidth(), m_pSwapChain->GetHeight()};
    m_Framebuffers.clear();  // 强制重建
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

void VulkanCommandList::SetVertexBuffer(IRHIBuffer* buffer, u32 binding) {
    auto* vkBuf = static_cast<VulkanBuffer*>(buffer);
    m_CurrentVB = vkBuf->GetHandle();
    m_VBBinding = binding;
}

void VulkanCommandList::SetIndexBuffer(IRHIBuffer* buffer, u32 offset) {
    auto* vkBuf = static_cast<VulkanBuffer*>(buffer);
    m_CurrentIB = vkBuf->GetHandle();
    m_IBOffset  = offset;
    // 根据缓冲区大小推断索引类型（4 字节 = UINT32，2 字节 = UINT16）
    m_CurrentIndexType = (vkBuf->GetSize() >= 4) ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
}

void VulkanCommandList::Draw(u32 vertexCount, u32 instanceCount,
                              u32 firstVertex, u32 firstInstance) {
    // 绑定当前顶点缓冲（使用记录下的 binding 索引）
    if (m_CurrentVB) {
        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(m_CmdBuffer, m_VBBinding, 1, &m_CurrentVB, &offset);
    }
    vkCmdDraw(m_CmdBuffer, vertexCount, instanceCount, firstVertex, firstInstance);
}

void VulkanCommandList::DrawIndexed(u32 indexCount, u32 instanceCount,
                                     u32 firstIndex, i32 vertexOffset, u32 firstInstance) {
    // 绑定索引缓冲
    if (m_CurrentIB)
        vkCmdBindIndexBuffer(m_CmdBuffer, m_CurrentIB, m_IBOffset, m_CurrentIndexType);
    // 绑定当前顶点缓冲
    if (m_CurrentVB) {
        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(m_CmdBuffer, m_VBBinding, 1, &m_CurrentVB, &offset);
    }
    vkCmdDrawIndexed(m_CmdBuffer, indexCount, instanceCount,
                     firstIndex, vertexOffset, firstInstance);
}

void VulkanCommandList::SetPushConstants(u32 offset, u32 size, const void* data) {
    if (m_CurrentLayout) {
        vkCmdPushConstants(m_CmdBuffer, m_CurrentLayout,
                          VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                          offset, size, data);
    }
}

// ResourceState → VkImageLayout 映射
static VkImageLayout ToVkImageLayout(ResourceState state) {
    switch (state) {
        case ResourceState::RenderTarget:       return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        case ResourceState::DepthStencilWrite:  return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        case ResourceState::DepthStencilRead:   return VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        case ResourceState::ShaderResource:     return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        case ResourceState::Present:            return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        case ResourceState::CopySrc:            return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        case ResourceState::CopyDst:            return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        case ResourceState::UnorderedAccess:    return VK_IMAGE_LAYOUT_GENERAL;
        default:                                return VK_IMAGE_LAYOUT_UNDEFINED;
    }
}

void VulkanCommandList::PipelineBarrier(
    PipelineStage srcStage, PipelineStage dstStage,
    ResourceState srcState, ResourceState dstState)
{
    // 仅处理全局内存屏障（简化实现，后续版本会扩展为 Image Buffer Barrier）
    VkMemoryBarrier memoryBarrier{};
    memoryBarrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    memoryBarrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    memoryBarrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;

    // PipelineStage → VkPipelineStageFlags 映射
    VkPipelineStageFlags vkSrcStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkPipelineStageFlags vkDstStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;

    if (u32(srcStage) & u32(PipelineStage::ColorAttachmentOutput))
        vkSrcStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    if (u32(srcStage) & u32(PipelineStage::FragmentShader))
        vkSrcStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    if (u32(srcStage) & u32(PipelineStage::ComputeShader))
        vkSrcStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    if (u32(srcStage) & u32(PipelineStage::Transfer))
        vkSrcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;

    if (u32(dstStage) & u32(PipelineStage::FragmentShader))
        vkDstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    if (u32(dstStage) & u32(PipelineStage::ColorAttachmentOutput))
        vkDstStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    if (u32(dstStage) & u32(PipelineStage::ComputeShader))
        vkDstStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    if (u32(dstStage) & u32(PipelineStage::Transfer))
        vkDstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;

    vkCmdPipelineBarrier(m_CmdBuffer,
        vkSrcStage, vkDstStage,
        0, 1, &memoryBarrier, 0, nullptr, 0, nullptr);
}

void VulkanCommandList::BindDescriptorSet(u32 setIndex, DescriptorSetHandle setHandle) {
    if (!m_VulkanDevice) return;
    VkDescriptorSet ds = m_VulkanDevice->ResolveDescriptorSet(setHandle);
    if (ds == VK_NULL_HANDLE) return;
    vkCmdBindDescriptorSets(m_CmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            m_CurrentLayout, setIndex, 1, &ds, 0, nullptr);
}

void VulkanCommandList::CopyBuffer(IRHIBuffer* src, IRHIBuffer* dst,
                                    u64 size, u64 srcOffset, u64 dstOffset) {
    auto* vkSrc = static_cast<VulkanBuffer*>(src);
    auto* vkDst = static_cast<VulkanBuffer*>(dst);

    VkBufferCopy region{};
    region.srcOffset = srcOffset;
    region.dstOffset = dstOffset;
    region.size      = size;

    vkCmdCopyBuffer(m_CmdBuffer, vkSrc->GetHandle(), vkDst->GetHandle(), 1, &region);
}

void VulkanCommandList::Submit() {
    // 等待 SwapChain 图像可用
    VkSemaphore waitSem   = VK_NULL_HANDLE;
    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    if (m_pSwapChain) waitSem = m_pSwapChain->GetImageAcquiredSemaphore();

    VkSubmitInfo submitInfo{};
    submitInfo.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.waitSemaphoreCount   = waitSem ? 1u : 0u;
    submitInfo.pWaitSemaphores      = &waitSem;
    submitInfo.pWaitDstStageMask    = &waitStage;
    submitInfo.commandBufferCount   = 1;
    submitInfo.pCommandBuffers      = &m_CmdBuffer;

    vkResetFences(m_Device, 1, &m_Fence);
    vkQueueSubmit(m_Queue, 1, &submitInfo, m_Fence);
    // 同步等待 GPU 完成
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
    // 解析 DescriptorSetLayout handles
    std::vector<VkDescriptorSetLayout> descLayouts;
    for (auto& handle : desc.descriptorSetLayouts) {
        VkDescriptorSetLayout l = ResolveDescriptorSetLayout(handle);
        if (l != VK_NULL_HANDLE)
            descLayouts.push_back(l);
    }
    return CreateVulkanPipeline(m_Device, desc, descLayouts);
}

std::unique_ptr<IRHITexture> VulkanDevice::CreateTexture(const TextureDesc& desc) {
    return CreateVulkanTexture(m_Device, m_Physical, m_GraphicsCmdPool, m_GraphicsQueue, desc);
}

std::unique_ptr<IRHISampler> VulkanDevice::CreateSampler(const SamplerDesc& desc) {
    return CreateVulkanSampler(m_Device, desc);
}

// ============================================================
// Factory
// ============================================================

std::unique_ptr<IRHIDevice> CreateDevice(Backend backend) {
    if (backend == Backend::Vulkan)
        return std::make_unique<VulkanDevice>();
    return nullptr;
}

// --- VulkanDeviceAccess — 内部桥接实现 ---
VkInstance VulkanDeviceAccess::GetInstance(IRHIDevice* d) {
    return static_cast<VulkanDevice*>(d)->GetVkInstance();
}
VkPhysicalDevice VulkanDeviceAccess::GetPhysical(IRHIDevice* d) {
    return static_cast<VulkanDevice*>(d)->GetVkPhysical();
}
VkDevice VulkanDeviceAccess::GetDevice(IRHIDevice* d) {
    return static_cast<VulkanDevice*>(d)->GetVkDevice();
}
u32 VulkanDeviceAccess::GetGraphicsFamily(IRHIDevice* d) {
    return static_cast<VulkanDevice*>(d)->GetGraphicsFamily();
}
VkQueue VulkanDeviceAccess::GetGraphicsQueue(IRHIDevice* d) {
    return static_cast<VulkanDevice*>(d)->GetGraphicsQueue();
}

} // namespace he::rhi
