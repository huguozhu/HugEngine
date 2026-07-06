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

    // 4. Select physical device
    SelectPhysicalDevice();

    // 5. Create logical device
    CreateLogicalDevice();

    // 6. Create command pools
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

    // pNext 链: descIndexing → addrFeature
    descIndexing.pNext = &addrFeature;

    VkPhysicalDeviceFeatures features{};

    VkDeviceCreateInfo deviceInfo{};
    deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceInfo.pNext = &descIndexing;
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    deviceInfo.enabledExtensionCount = static_cast<u32>(deviceExtensions.size());
    deviceInfo.ppEnabledExtensionNames = deviceExtensions.data();
    deviceInfo.pEnabledFeatures = &features;

    VkResult result = vkCreateDevice(m_Physical, &deviceInfo, nullptr, &m_Device);
    HE_ASSERT(result == VK_SUCCESS, "Failed to create logical device");

    vkGetDeviceQueue(m_Device, m_GraphicsFamily, 0, &m_GraphicsQueue);
    HE_CORE_INFO("Logical device + graphics queue created");

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

} // namespace he::rhi
