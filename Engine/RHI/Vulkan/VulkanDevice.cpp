#include "RHI/RHI.h"
#include "RHI/SwapChain.h"
#include "RHI/CommandList.h"
#include "RHI/Buffer.h"
#include "RHI/Shader.h"
#include "Core/Log.h"

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include "VulkanQueryPool.h"
#include "VulkanInternal.h"
#include "Core/Assert.h"

#include <algorithm>
#include <vector>
#include <cstring>

// VulkanSwapChain/VulkanCommandList + 其他 Vulkan 类型完整定义
#include "VulkanInternal.h"

namespace he::rhi {

// 工厂函数（实现在 VulkanResources.cpp 和 VulkanPipeline.cpp）
std::unique_ptr<IRHIBuffer>        CreateVulkanBuffer(VmaAllocator, const BufferDesc&);
std::unique_ptr<IRHITexture>       CreateVulkanTexture(VmaAllocator, VkCommandPool, VkQueue, const TextureDesc&);
std::unique_ptr<IRHISampler>       CreateVulkanSampler(VkDevice, const SamplerDesc&);
std::unique_ptr<IRHIPipelineState> CreateVulkanPipeline(VkDevice, const PipelineStateDesc&,
    const std::vector<VkDescriptorSetLayout>& descLayouts);

// VulkanDevice 析构函数实现（声明在 VulkanInternal.h，需要非内联定义锚定 vtable）
VulkanDevice::~VulkanDevice() { Shutdown(); }

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
// VulkanDevice::FindQueueFamilies — 查询所有队列族，检测 AsyncCompute 能力
// ============================================================
void VulkanDevice::FindQueueFamilies() {
    // 1. 查找 Graphics + Compute + Present 队列族（必须）
    m_GraphicsFamily = FindQueueFamily(m_Physical,
        VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT, m_Surface);
    HE_ASSERT(m_GraphicsFamily != ~0u, "No suitable graphics queue family found");

    // 2. 尝试查找独立 Compute 队列族（不带 GRAPHICS_BIT）
    //    优先选纯 COMPUTE_BIT（不含 TRANSFER）→ 专用硬件引擎
    //    回退: COMPUTE+TRANSFER 族 → 独立调度
    //    无回退: AsyncCompute = false
    u32 computeOnlyFamily  = UINT32_MAX;  // 纯 Compute（最优）
    u32 computeDedicatedFamily = UINT32_MAX; // Compute+Transfer（次选）

    u32 queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(m_Physical, &queueFamilyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(m_Physical, &queueFamilyCount, families.data());

    for (u32 i = 0; i < queueFamilyCount; i++) {
        VkQueueFlags flags = families[i].queueFlags;
        bool hasCompute  = (flags & VK_QUEUE_COMPUTE_BIT) != 0;
        bool hasGraphics = (flags & VK_QUEUE_GRAPHICS_BIT) != 0;
        bool hasTransfer = (flags & VK_QUEUE_TRANSFER_BIT) != 0;

        if (hasCompute && !hasGraphics && i != m_GraphicsFamily) {
            // 最佳选择: 纯 Compute 队列族（异步硬件引擎，如 NVIDIA AMP）
            if (!hasTransfer) {
                computeOnlyFamily = i;
                break;
            }
            // 次选: Compute+Transfer 族（独立但共享 DMA 引擎）
            if (computeDedicatedFamily == UINT32_MAX) {
                computeDedicatedFamily = i;
            }
        }
    }

    if (computeOnlyFamily != UINT32_MAX) {
        m_ComputeFamily    = computeOnlyFamily;
        m_HasAsyncCompute  = true;
        HE_CORE_INFO("AsyncCompute: Tier 2 — 专用纯 Compute 硬件引擎 (family {})", m_ComputeFamily);
    } else if (computeDedicatedFamily != UINT32_MAX) {
        m_ComputeFamily    = computeDedicatedFamily;
        m_HasAsyncCompute  = true;
        HE_CORE_INFO("AsyncCompute: Tier 1 — 独立 Compute 队列族 (family {})", m_ComputeFamily);
    } else {
        // 无独立 Compute 队列 → 回退到 Graphics 队列族
        m_ComputeFamily    = m_GraphicsFamily;
        m_HasAsyncCompute  = false;
        HE_CORE_INFO("AsyncCompute: Tier 0 — 不支持，Compute 与 Graphics 共享队列");
    }
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

    // 异步计算能力
    caps.supportsAsyncCompute  = m_HasAsyncCompute;
    caps.supportsTransferQueue = false;  // Transfer 队列暂未独立实现
    caps.asyncComputeTier      = m_HasAsyncCompute
        ? (m_ComputeFamily != m_GraphicsFamily ? 2u : 1u)
        : 0u;

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

    // 4. Select physical device
    SelectPhysicalDevice();

    // 5. 查询队列族（检测 AsyncCompute 能力，必须在 CreateLogicalDevice 之前）
    FindQueueFamilies();

    // 6. Create logical device
    CreateLogicalDevice();

    // 7. Create command pools
    CreateCommandPools();

    HE_CORE_INFO("Vulkan device fully initialized");
}

void VulkanDevice::Shutdown() {
    if (m_VmaAllocator) { vmaDestroyAllocator(m_VmaAllocator); m_VmaAllocator = VK_NULL_HANDLE; }

    // 清理 Descriptor Set Layouts
    for (auto& info : m_DescLayoutInfos)
        vkDestroyDescriptorSetLayout(m_Device, info.layout, nullptr);
    m_DescLayoutInfos.clear();
    m_DescSets.clear();
    m_DescSetLayoutParents.clear();

    if (m_DescPool)        { vkDestroyDescriptorPool(m_Device, m_DescPool, nullptr); m_DescPool = VK_NULL_HANDLE; }

    // 清理 Timeline Semaphore (Fence)
    for (auto& fs : m_Fences) {
        if (fs.semaphore) vkDestroySemaphore(m_Device, fs.semaphore, nullptr);
    }
    m_Fences.clear();

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
    float queuePriority = 1.0f;

    // 构建队列创建信息（1 或 2 个队列族）
    std::vector<VkDeviceQueueCreateInfo> queueInfos;
    {
        VkDeviceQueueCreateInfo gfxInfo{};
        gfxInfo.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        gfxInfo.queueFamilyIndex = m_GraphicsFamily;
        gfxInfo.queueCount       = 1;
        gfxInfo.pQueuePriorities = &queuePriority;
        queueInfos.push_back(gfxInfo);
    }
    if (m_HasAsyncCompute) {
        VkDeviceQueueCreateInfo compInfo{};
        compInfo.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        compInfo.queueFamilyIndex = m_ComputeFamily;
        compInfo.queueCount       = 1;
        compInfo.pQueuePriorities = &queuePriority;
        queueInfos.push_back(compInfo);
    }

    std::vector<const char*> deviceExtensions = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME,  // Bindless 资源
    };

    // Bindless: descriptor indexing 特性
    VkPhysicalDeviceDescriptorIndexingFeatures descIndexing{};
    descIndexing.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;
    descIndexing.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    descIndexing.runtimeDescriptorArray = VK_TRUE;
    descIndexing.descriptorBindingVariableDescriptorCount = VK_TRUE;
    descIndexing.descriptorBindingPartiallyBound = VK_TRUE;
    descIndexing.descriptorBindingStorageBufferUpdateAfterBind = VK_TRUE;
    descIndexing.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;

    // 启用 bufferDeviceAddress
    VkPhysicalDeviceBufferDeviceAddressFeatures addrFeature{};
    addrFeature.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;
    addrFeature.bufferDeviceAddress = VK_TRUE;
    addrFeature.pNext = nullptr;

    // 启用 Timeline Semaphore（Vulkan 1.2+ 核心特性，需显式开启）
    VkPhysicalDeviceTimelineSemaphoreFeatures timelineFeature{};
    timelineFeature.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
    timelineFeature.timelineSemaphore = VK_TRUE;
    timelineFeature.pNext = nullptr;

    // pNext 链: descIndexing → addrFeature → timelineFeature
    descIndexing.pNext = &addrFeature;
    addrFeature.pNext  = &timelineFeature;

    VkPhysicalDeviceFeatures features{};

    VkDeviceCreateInfo deviceInfo{};
    deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceInfo.pNext = &descIndexing;
    deviceInfo.queueCreateInfoCount = static_cast<u32>(queueInfos.size());
    deviceInfo.pQueueCreateInfos = queueInfos.data();
    deviceInfo.enabledExtensionCount = static_cast<u32>(deviceExtensions.size());
    deviceInfo.ppEnabledExtensionNames = deviceExtensions.data();
    deviceInfo.pEnabledFeatures = &features;

    VkResult result = vkCreateDevice(m_Physical, &deviceInfo, nullptr, &m_Device);
    HE_ASSERT(result == VK_SUCCESS, "Failed to create logical device");

    // 获取队列句柄
    vkGetDeviceQueue(m_Device, m_GraphicsFamily, 0, &m_GraphicsQueue);
    if (m_HasAsyncCompute) {
        vkGetDeviceQueue(m_Device, m_ComputeFamily, 0, &m_ComputeQueue);
        HE_CORE_INFO("Logical device created (Graphics family={} + AsyncCompute family={})",
                     m_GraphicsFamily, m_ComputeFamily);
    } else {
        m_ComputeQueue = m_GraphicsQueue;  // 回退到同一队列
        HE_CORE_INFO("Logical device created (Graphics+Compute shared, family={})",
                     m_GraphicsFamily);
    }

    // 创建 VMA 分配器（替代裸 vkAllocateMemory/vkFreeMemory）
    VmaVulkanFunctions vmaVulkanFunctions{};
    vmaVulkanFunctions.vkGetInstanceProcAddr                = vkGetInstanceProcAddr;
    vmaVulkanFunctions.vkGetDeviceProcAddr                  = vkGetDeviceProcAddr;
    vmaVulkanFunctions.vkAllocateMemory                    = vkAllocateMemory;
    vmaVulkanFunctions.vkBindBufferMemory                  = vkBindBufferMemory;
    vmaVulkanFunctions.vkBindImageMemory                   = vkBindImageMemory;
    vmaVulkanFunctions.vkCreateBuffer                      = vkCreateBuffer;
    vmaVulkanFunctions.vkCreateImage                       = vkCreateImage;
    vmaVulkanFunctions.vkDestroyBuffer                     = vkDestroyBuffer;
    vmaVulkanFunctions.vkDestroyImage                      = vkDestroyImage;
    vmaVulkanFunctions.vkFlushMappedMemoryRanges           = vkFlushMappedMemoryRanges;
    vmaVulkanFunctions.vkFreeMemory                        = vkFreeMemory;
    vmaVulkanFunctions.vkGetBufferMemoryRequirements       = vkGetBufferMemoryRequirements;
    vmaVulkanFunctions.vkGetImageMemoryRequirements        = vkGetImageMemoryRequirements;
    vmaVulkanFunctions.vkGetPhysicalDeviceMemoryProperties = vkGetPhysicalDeviceMemoryProperties;
    vmaVulkanFunctions.vkGetPhysicalDeviceProperties       = vkGetPhysicalDeviceProperties;
    vmaVulkanFunctions.vkInvalidateMappedMemoryRanges      = vkInvalidateMappedMemoryRanges;
    vmaVulkanFunctions.vkMapMemory                         = vkMapMemory;
    vmaVulkanFunctions.vkUnmapMemory                       = vkUnmapMemory;
    vmaVulkanFunctions.vkCmdCopyBuffer                     = vkCmdCopyBuffer;
    // Vulkan 1.1+ functions（Vulkan 1.3 中为核心，VMA 字段名仍用 KHR 后缀）
    vmaVulkanFunctions.vkGetBufferMemoryRequirements2KHR   = vkGetBufferMemoryRequirements2;
    vmaVulkanFunctions.vkGetImageMemoryRequirements2KHR    = vkGetImageMemoryRequirements2;
    vmaVulkanFunctions.vkBindBufferMemory2KHR              = vkBindBufferMemory2;
    vmaVulkanFunctions.vkBindImageMemory2KHR               = vkBindImageMemory2;

    VmaAllocatorCreateInfo vmaCreateInfo{};
    vmaCreateInfo.vulkanApiVersion = VK_API_VERSION_1_3;
    vmaCreateInfo.physicalDevice   = m_Physical;
    vmaCreateInfo.device           = m_Device;
    vmaCreateInfo.instance         = m_Instance;
    vmaCreateInfo.flags            = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    vmaCreateInfo.pVulkanFunctions = &vmaVulkanFunctions;

    VkResult vmaResult = vmaCreateAllocator(&vmaCreateInfo, &m_VmaAllocator);
    HE_ASSERT(vmaResult == VK_SUCCESS, "Failed to create VMA allocator");
    HE_CORE_INFO("VMA allocator created (buffer device address enabled)");
}

void VulkanDevice::CreateCommandPools() {
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

    // Graphics 命令池 — 使用 Graphics 队列族
    poolInfo.queueFamilyIndex = m_GraphicsFamily;
    vkCreateCommandPool(m_Device, &poolInfo, nullptr, &m_GraphicsCmdPool);

    // Compute 命令池 — 使用 Compute 队列族（可能与 Graphics 相同）
    poolInfo.queueFamilyIndex = m_ComputeFamily;
    vkCreateCommandPool(m_Device, &poolInfo, nullptr, &m_ComputeCmdPool);

    HE_CORE_INFO("Command pools: Graphics(family={}), Compute(family={})",
                 m_GraphicsFamily, m_ComputeFamily);
}

std::unique_ptr<IRHISwapChain> VulkanDevice::CreateSwapChain(const SwapChainDesc& desc) {
    return std::make_unique<VulkanSwapChain>(m_Device, m_Physical, m_Surface,
                                            m_GraphicsQueue, desc);
}

std::unique_ptr<IRHICommandList> VulkanDevice::CreateCommandList(QueueType queue) {
    // 根据队列类型选择正确的 VkQueue 和 QueueFamily
    VkQueue vkQueue = m_GraphicsQueue;
    u32     family  = m_GraphicsFamily;

    if (queue == QueueType::Compute) {
        vkQueue = m_ComputeQueue;
        family  = m_ComputeFamily;
    } else if (queue == QueueType::Copy) {
        // Copy 队列暂未独立实现，回退到 Graphics
        vkQueue = m_GraphicsQueue;
        family  = m_GraphicsFamily;
    }

    auto cmdList = std::make_unique<VulkanCommandList>(m_Device, vkQueue, family, this);
    cmdList->SetQueueType(queue);  // 记录队列类型，用于跨队列 Barrier
    return cmdList;
}

std::unique_ptr<IRHICommandList> VulkanDevice::CreateSecondaryCommandList() {
    return std::make_unique<VulkanCommandList>(m_Device, m_GraphicsFamily, this);
}

void VulkanDevice::WaitIdle() {
    if (m_Device) vkDeviceWaitIdle(m_Device);
}

void VulkanDevice::Submit(IRHICommandList* cmdList) {
    auto* vulkanCmd = static_cast<VulkanCommandList*>(cmdList);
    vulkanCmd->Submit();
}

// ── 多队列支持 ──

bool VulkanDevice::HasAsyncComputeQueue() const {
    return m_HasAsyncCompute;
}

u32 VulkanDevice::GetQueueFamily(QueueType queue) const {
    switch (queue) {
        case QueueType::Compute:  return m_ComputeFamily;
        case QueueType::Graphics: return m_GraphicsFamily;
        case QueueType::Copy:     return m_GraphicsFamily;  // 暂用 Graphics 族
        default:                  return m_GraphicsFamily;
    }
}

// ── 跨队列同步原语 (Timeline Semaphore) ──

VkSemaphore VulkanDevice::CreateTimelineSemaphore(u64 initialValue) {
    VkSemaphoreTypeCreateInfo typeInfo{};
    typeInfo.sType         = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    typeInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    typeInfo.initialValue  = initialValue;

    VkSemaphoreCreateInfo semInfo{};
    semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    semInfo.pNext = &typeInfo;

    VkSemaphore semaphore = VK_NULL_HANDLE;
    VkResult result = vkCreateSemaphore(m_Device, &semInfo, nullptr, &semaphore);
    HE_ASSERT(result == VK_SUCCESS, "Failed to create timeline semaphore");
    return semaphore;
}

RHIFenceHandle VulkanDevice::CreateFence() {
    VkSemaphore semaphore = CreateTimelineSemaphore(0);
    FenceState fs;
    fs.semaphore    = semaphore;
    fs.currentValue = 0;
    m_Fences.push_back(fs);
    return static_cast<RHIFenceHandle>(m_Fences.size());  // 1-based handle
}

void VulkanDevice::DestroyFence(RHIFenceHandle fence) {
    if (fence == kInvalidFence || fence > m_Fences.size()) return;
    auto& fs = m_Fences[static_cast<usize>(fence - 1)];
    if (fs.semaphore) {
        vkDestroySemaphore(m_Device, fs.semaphore, nullptr);
        fs.semaphore = VK_NULL_HANDLE;
    }
}

bool VulkanDevice::WaitForFence(RHIFenceHandle fence, u64 value, u64 timeout) {
    if (fence == kInvalidFence || fence > m_Fences.size()) return false;
    auto& fs = m_Fences[static_cast<usize>(fence - 1)];

    VkSemaphoreWaitInfo waitInfo{};
    waitInfo.sType          = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
    waitInfo.semaphoreCount = 1;
    waitInfo.pSemaphores    = &fs.semaphore;
    waitInfo.pValues        = &value;

    VkResult result = vkWaitSemaphores(m_Device, &waitInfo, timeout);
    return result == VK_SUCCESS;
}

u64 VulkanDevice::GetFenceValue(RHIFenceHandle fence) const {
    if (fence == kInvalidFence || fence > m_Fences.size()) return 0;
    auto& fs = m_Fences[static_cast<usize>(fence - 1)];

    u64 value = 0;
    vkGetSemaphoreCounterValue(m_Device, fs.semaphore, &value);
    return value;
}

void VulkanDevice::SignalFenceOnQueue(QueueType queue, RHIFenceHandle fence, u64 value) {
    if (fence == kInvalidFence || fence > m_Fences.size()) return;
    auto& fs = m_Fences[static_cast<usize>(fence - 1)];

    VkQueue vkQueue = (queue == QueueType::Compute) ? m_ComputeQueue : m_GraphicsQueue;

    VkTimelineSemaphoreSubmitInfo timelineInfo{};
    timelineInfo.sType                     = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
    timelineInfo.signalSemaphoreValueCount = 1;
    timelineInfo.pSignalSemaphoreValues    = &value;

    VkSubmitInfo submitInfo{};
    submitInfo.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.pNext              = &timelineInfo;
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores    = &fs.semaphore;

    VkResult result = vkQueueSubmit(vkQueue, 1, &submitInfo, VK_NULL_HANDLE);
    HE_ASSERT(result == VK_SUCCESS, "SignalFenceOnQueue: vkQueueSubmit failed");

    fs.currentValue = value;
}

void VulkanDevice::WaitFenceOnQueue(QueueType queue, RHIFenceHandle fence, u64 value) {
    if (fence == kInvalidFence || fence > m_Fences.size()) return;
    auto& fs = m_Fences[static_cast<usize>(fence - 1)];

    VkQueue vkQueue = (queue == QueueType::Compute) ? m_ComputeQueue : m_GraphicsQueue;

    VkTimelineSemaphoreSubmitInfo timelineInfo{};
    timelineInfo.sType                     = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
    timelineInfo.waitSemaphoreValueCount   = 1;
    timelineInfo.pWaitSemaphoreValues      = &value;

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;

    VkSubmitInfo submitInfo{};
    submitInfo.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.pNext                = &timelineInfo;
    submitInfo.waitSemaphoreCount   = 1;
    submitInfo.pWaitSemaphores      = &fs.semaphore;
    submitInfo.pWaitDstStageMask    = &waitStage;

    VkResult result = vkQueueSubmit(vkQueue, 1, &submitInfo, VK_NULL_HANDLE);
    HE_ASSERT(result == VK_SUCCESS, "WaitFenceOnQueue: vkQueueSubmit failed");
}

void VulkanDevice::SubmitAll(Span<IRHICommandList*> cmdLists) {
    // 按队列分组提交
    std::vector<VulkanCommandList*> gfxLists, compLists;
    for (auto* cl : cmdLists) {
        auto* vkCl = static_cast<VulkanCommandList*>(cl);
        if (vkCl->GetQueueType() == QueueType::Compute)
            compLists.push_back(vkCl);
        else
            gfxLists.push_back(vkCl);
    }

    // 分别提交到各自队列
    for (auto* cl : gfxLists) cl->Submit();
    for (auto* cl : compLists) cl->Submit();
}

// ── GPU Query ──
std::unique_ptr<IRHIQueryPool> VulkanDevice::CreateQueryPool(u32 queryCount) {
    return std::make_unique<VulkanQueryPool>(m_Device, queryCount);
}

float VulkanDevice::GetTimestampPeriod() {
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(m_Physical, &props);
    return float(props.limits.timestampPeriod);  // 纳秒
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
        case DescriptorType::SampledImage:          return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        case DescriptorType::Sampler:               return VK_DESCRIPTOR_TYPE_SAMPLER;
        default:                                    return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    }
}

void VulkanDevice::EnsureDescriptorPool() {
    if (m_DescPool != VK_NULL_HANDLE) return;

    VkDescriptorPoolSize poolSizes[] = {
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 64 },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1024 },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8192 }, // bindless 纹理数组
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 4096 },
        { VK_DESCRIPTOR_TYPE_SAMPLER, 4096 },
    };

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets       = 1024;  // 需容纳 bindless + per-frame 描述符集
    poolInfo.poolSizeCount = 5;
    poolInfo.pPoolSizes    = poolSizes;
    poolInfo.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT
                            | VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;

    vkCreateDescriptorPool(m_Device, &poolInfo, nullptr, &m_DescPool);
}

DescriptorSetLayoutHandle VulkanDevice::CreateDescriptorSetLayout(const DescriptorSetLayoutDesc& desc) {
    DescLayoutInfo info;
    std::vector<VkDescriptorSetLayoutBinding> vkBindings;
    for (auto& b : desc.bindings) {
        VkDescriptorSetLayoutBinding vb{};
        vb.binding            = b.binding;
        vb.descriptorType     = ToVkDescType(b.type);
        vb.descriptorCount    = b.count;
        vb.stageFlags = b.stageMask;
        vkBindings.push_back(vb);

        info.bindings.push_back(b);

        VkDescriptorBindingFlags flags = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
        if (b.bindless) {
            flags |= VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT
                   | VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
        }
        info.bindingFlags.push_back(flags);
    }

    VkDescriptorSetLayoutBindingFlagsCreateInfo flagsInfo{};
    flagsInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
    flagsInfo.bindingCount  = static_cast<u32>(info.bindingFlags.size());
    flagsInfo.pBindingFlags = info.bindingFlags.data();

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.flags        = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    layoutInfo.pNext        = &flagsInfo;
    layoutInfo.bindingCount = static_cast<u32>(vkBindings.size());
    layoutInfo.pBindings    = vkBindings.data();

    VkDescriptorSetLayout layout;
    vkCreateDescriptorSetLayout(m_Device, &layoutInfo, nullptr, &layout);
    info.layout = layout;

    DescriptorSetLayoutHandle handle = static_cast<DescriptorSetLayoutHandle>(m_DescLayoutInfos.size() + 1);
    m_DescLayoutInfos.push_back(info);
    return handle;
}

DescriptorSetHandle VulkanDevice::AllocateDescriptorSet(DescriptorSetLayoutHandle layoutHandle) {
    EnsureDescriptorPool();
    if (layoutHandle == 0 || layoutHandle > m_DescLayoutInfos.size()) return kInvalidSet;

    auto& info = m_DescLayoutInfos[static_cast<usize>(layoutHandle - 1)];
    VkDescriptorSetLayout layout = info.layout;

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool     = m_DescPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts        = &layout;

    // 处理 bindless 可变描述符数量
    // Vulkan 规范：descriptorSetCount 是分配的 set 数（固定为 1），
    // pDescriptorCounts 指向单个 u32，其值为最后一个 VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT 的 maxCount
    VkDescriptorSetVariableDescriptorCountAllocateInfo varCountInfo{};
    u32 maxVarCount = 0;
    bool hasBindless = false;
    for (usize i = 0; i < info.bindings.size(); ++i) {
        if (info.bindings[i].bindless) {
            maxVarCount = std::max(maxVarCount, info.bindings[i].count);
            hasBindless = true;
        }
    }
    if (hasBindless) {
        varCountInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO;
        varCountInfo.descriptorSetCount = 1;           // 只分配一个 descriptor set
        varCountInfo.pDescriptorCounts = &maxVarCount; // 单个 set 的可变计数
        allocInfo.pNext = &varCountInfo;
    }

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

void VulkanDevice::UpdateDescriptorSet(DescriptorSetHandle setHandle, u32 binding,
                                        DescriptorType type,
                                        IRHITexture** textures, IRHISampler** samplers,
                                        u32 count) {
    if (setHandle == 0 || setHandle > m_DescSets.size()) return;
    if (count == 0) return;
    VkDescriptorSet ds = m_DescSets[static_cast<usize>(setHandle - 1)];
    if (ds == VK_NULL_HANDLE) return;

    VkDescriptorType vkType = ToVkDescType(type);
    std::vector<VkDescriptorImageInfo> imageInfos(count);
    for (u32 i = 0; i < count; ++i) {
        // SampledImage: 仅使用 imageView，sampler 字段忽略
        if (vkType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) {
            if (!textures || !textures[i]) {
                imageInfos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                imageInfos[i].imageView   = VK_NULL_HANDLE;
                imageInfos[i].sampler     = VK_NULL_HANDLE;
                continue;
            }
            auto* vkTex = static_cast<VulkanTexture*>(textures[i]);
            imageInfos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            imageInfos[i].imageView   = vkTex->GetImageView();
            imageInfos[i].sampler     = VK_NULL_HANDLE;
            continue;
        }
        // Sampler: 仅使用 sampler，imageView 字段忽略
        if (vkType == VK_DESCRIPTOR_TYPE_SAMPLER) {
            if (!samplers || !samplers[i]) {
                imageInfos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                imageInfos[i].imageView   = VK_NULL_HANDLE;
                imageInfos[i].sampler     = VK_NULL_HANDLE;
                continue;
            }
            auto* vkSampler = static_cast<VulkanSampler*>(samplers[i]);
            imageInfos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            imageInfos[i].imageView   = VK_NULL_HANDLE;
            imageInfos[i].sampler     = vkSampler->GetHandle();
            continue;
        }
        // CombinedImageSampler: 同时使用 imageView + sampler
        if (!textures || !textures[i] || !samplers || !samplers[i]) {
            imageInfos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            imageInfos[i].imageView   = VK_NULL_HANDLE;
            imageInfos[i].sampler     = VK_NULL_HANDLE;
            continue;
        }
        auto* vkTex     = static_cast<VulkanTexture*>(textures[i]);
        auto* vkSampler = static_cast<VulkanSampler*>(samplers[i]);
        imageInfos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imageInfos[i].imageView   = vkTex->GetImageView();
        imageInfos[i].sampler     = vkSampler->GetHandle();
    }

    VkWriteDescriptorSet write{};
    write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet          = ds;
    write.dstBinding      = binding;
    write.dstArrayElement = 0;
    write.descriptorCount = count;
    write.descriptorType  = vkType;
    write.pImageInfo      = imageInfos.data();

    vkUpdateDescriptorSets(m_Device, 1, &write, 0, nullptr);
}

void VulkanDevice::UpdateDescriptorSet(DescriptorSetHandle setHandle, u32 binding,
                                        DescriptorType type, IRHITexture* texture,
                                        IRHISampler* sampler) {
    if (setHandle == 0 || setHandle > m_DescSets.size()) return;
    if (!texture || !sampler) return;  // 防御性检查
    VkDescriptorSet ds = m_DescSets[static_cast<usize>(setHandle - 1)];
    if (ds == VK_NULL_HANDLE) return;

    auto* vkTex     = static_cast<VulkanTexture*>(texture);
    auto* vkSampler = static_cast<VulkanSampler*>(sampler);

    VkImageView imgView = vkTex->GetImageView();
    VkSampler   vkSamp  = vkSampler->GetHandle();

    // 诊断日志：定位非法描述符绑定
    if (imgView == VK_NULL_HANDLE || imgView == (VkImageView)0xdddddddddddddddd ||
        vkSamp == VK_NULL_HANDLE || vkSamp == (VkSampler)0xdddddddddddddddd) {
        HE_CORE_ERROR("UpdateDescriptorSet: 非法句柄! binding={} imgView={:#018x} sampler={:#018x} texPtr={} sampPtr={}",
            binding, (u64)imgView, (u64)vkSamp, (void*)texture, (void*)sampler);
        return;
    }

    VkDescriptorImageInfo imageInfo{};
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfo.imageView   = imgView;
    imageInfo.sampler     = vkSamp;

    VkWriteDescriptorSet write{};
    write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet          = ds;
    write.dstBinding      = binding;
    write.dstArrayElement = 0;
    write.descriptorCount = 1;
    write.descriptorType  = ToVkDescType(type);
    write.pImageInfo      = &imageInfo;

    vkUpdateDescriptorSets(m_Device, 1, &write, 0, nullptr);
}

void VulkanDevice::DestroyDescriptorSetLayout(DescriptorSetLayoutHandle handle) {
    if (handle == 0 || handle > m_DescLayoutInfos.size()) return;
    VkDescriptorSetLayout layout = m_DescLayoutInfos[static_cast<usize>(handle - 1)].layout;
    vkDestroyDescriptorSetLayout(m_Device, layout, nullptr);
}

// ============================================================
// 资源创建委托（实现在 VulkanResources.cpp 和 VulkanPipeline.cpp）
// ============================================================

std::unique_ptr<IRHIBuffer> VulkanDevice::CreateBuffer(const BufferDesc& desc) {
    return CreateVulkanBuffer(m_VmaAllocator, desc);
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
    return CreateVulkanTexture(m_VmaAllocator, m_GraphicsCmdPool, m_GraphicsQueue, desc);
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

VkCommandPool VulkanDeviceAccess::GetGraphicsCmdPool(IRHIDevice* d) {
    return static_cast<VulkanDevice*>(d)->GetGraphicsCmdPool();
}
VkQueue VulkanDeviceAccess::GetComputeQueue(IRHIDevice* d) {
    return static_cast<VulkanDevice*>(d)->GetComputeQueue();
}
u32 VulkanDeviceAccess::GetComputeFamily(IRHIDevice* d) {
    return static_cast<VulkanDevice*>(d)->GetComputeFamily();
}

} // namespace he::rhi
