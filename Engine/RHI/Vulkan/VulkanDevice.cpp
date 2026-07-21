#include "RHI/RHI.h"
#include "RHI/SwapChain.h"
#include "RHI/CommandList.h"
#include "RHI/Buffer.h"
#include "RHI/Shader.h"
#include "Core/Log.h"

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include "VulkanQueryPool.h"
#include "VulkanDevice.h"
#include "Core/Assert.h"

#include <algorithm>
#include <vector>
#include <cstring>

namespace he::rhi {

// ============================================================
// 跨编译单元共享的工厂/辅助函数声明
// ============================================================

// 工厂函数（实现在 VulkanResources.cpp 和 VulkanPipeline.cpp）
std::unique_ptr<IRHIBuffer>        CreateVulkanBuffer(VmaAllocator, const BufferDesc&);
std::unique_ptr<IRHITexture>       CreateVulkanTexture(VmaAllocator, VkCommandPool, VkQueue, const TextureDesc&);
std::unique_ptr<IRHISampler>       CreateVulkanSampler(VkDevice, const SamplerDesc&);
// 注意：CreateVulkanPipeline 现在接受 PSOCache + DeferredDestructionQueue 参数
std::unique_ptr<IRHIPipelineState> CreateVulkanPipeline(VkDevice, const PipelineStateDesc&,
    const std::vector<VkDescriptorSetLayout>& descLayouts,
    class VulkanDevice* vulkanDevice = nullptr);

// PSO 哈希函数声明（实现在 VulkanPipeline.cpp）
uint64_t HashPipelineStateDesc(const PipelineStateDesc& desc);

// VulkanDevice 析构函数实现（声明在 VulkanInternal.h，需要非内联定义锚定 vtable）
VulkanDevice::~VulkanDevice() { Shutdown(); }

// ============================================================
// Helper: 查找队列族
// ============================================================
// ASBuildFlags → VkBuildAccelerationStructureFlagsKHR 映射（跨编译单元共享）
VkBuildAccelerationStructureFlagsKHR ToVkBuildFlags(ASBuildFlags flags) {
    VkBuildAccelerationStructureFlagsKHR vkFlags = 0;
    u8 f = u8(flags);
    if (f & u8(ASBuildFlags::AllowUpdate))     vkFlags |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
    if (f & u8(ASBuildFlags::AllowCompaction)) vkFlags |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR;
    if (f & u8(ASBuildFlags::PreferFastTrace)) vkFlags |= VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    if (f & u8(ASBuildFlags::PreferFastBuild)) vkFlags |= VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
    if (f & u8(ASBuildFlags::MinimizeMemory))  vkFlags |= VK_BUILD_ACCELERATION_STRUCTURE_LOW_MEMORY_BIT_KHR;
    return vkFlags;
}

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
// FindQueueFamilies — 查询所有队列族，检测 AsyncCompute 能力
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
            if (!hasTransfer) {
                computeOnlyFamily = i;
                break;
            }
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
        m_ComputeFamily    = m_GraphicsFamily;
        m_HasAsyncCompute  = false;
        HE_CORE_INFO("AsyncCompute: Tier 0 — 不支持，Compute 与 Graphics 共享队列");
    }
}

// ============================================================
// GetCaps — 查询设备能力
// ============================================================
DeviceCaps VulkanDevice::GetCaps() const {
    DeviceCaps caps;
    caps.maxBindlessResources = kDefaultMaxBindlessResources;  // 引擎默认 Bindless 上限
    caps.maxPushConstantsSize = kMaxPushConstantSize;  // 引擎全局 Push Constant 上限

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(m_Physical, &props);
    caps.maxSamplerAnisotropy = static_cast<u32>(props.limits.maxSamplerAnisotropy);

    // Ray Tracing
    caps.supportsRayTracing       = m_SupportsRT;
    caps.supportsVRS              = false;
    caps.maxRayRecursionDepth     = m_MaxRayRecursionDepth;
    caps.shaderGroupHandleSize    = m_ShaderGroupHandleSize;
    caps.shaderGroupBaseAlignment = m_ShaderGroupBaseAlignment;
    caps.maxRTDispatchSize        = m_MaxRTDispatchSize;
    caps.maxASInstanceCount       = m_MaxASInstanceCount;
    caps.maxASGeometryCount       = m_MaxASGeometryCount;
    caps.maxASPrimitiveCount      = m_MaxASPrimitiveCount;
    caps.minASScratchAlignment    = m_MinASScratchAlignment;

    // Mesh Shader
    caps.supportsMeshShaders        = m_SupportsMesh;
    caps.maxMeshWorkGroupInvocations = m_MaxMeshWorkGroupInvocations;
    caps.maxMeshOutputVertices       = m_MaxMeshOutputVertices;
    caps.maxMeshOutputPrimitives     = m_MaxMeshOutputPrimitives;
    caps.maxTaskWorkGroupInvocations = m_MaxTaskWorkGroupInvocations;
    caps.maxTaskPayloadSize          = m_MaxTaskPayloadSize;
    caps.maxMeshWorkGroupCountX      = m_MaxMeshWorkGroupCountX;
    caps.maxMeshWorkGroupCountY      = m_MaxMeshWorkGroupCountY;
    caps.maxMeshWorkGroupCountZ      = m_MaxMeshWorkGroupCountZ;

    // 异步计算
    caps.supportsAsyncCompute  = m_HasAsyncCompute;
    caps.supportsTransferQueue = false;
    caps.asyncComputeTier      = m_HasAsyncCompute
        ? (m_ComputeFamily != m_GraphicsFamily ? 2u : 1u)
        : 0u;

    // DGC
    caps.supportsDGC = m_SupportsDGC;

    return caps;
}

// ============================================================
// Initialize — 创建 VkInstance → Device → 设置一切
// ============================================================
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
    // 始终加载 debug utils 扩展（对象命名 + 调试标签无需验证层）
    instanceExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

    std::vector<const char*> validationLayers;

    if (m_ValidationEnabled) {
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

    // 2. 设置调试回调
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

    // 5. 查询硬件能力（在创建逻辑设备之前）
    QueryRTCapabilities();       // → VulkanDevice_RT.cpp
    QueryMeshCapabilities();     // → VulkanDevice_MeshShader.cpp
    QueryDGCCapabilities();      // → VulkanDevice_MeshShader.cpp
    FindQueueFamilies();

    // 6. Create logical device
    CreateLogicalDevice();

    // 7. Create command pools
    CreateCommandPools();

    HE_CORE_INFO("Vulkan device fully initialized");
}

// ============================================================
// Shutdown — 销毁所有 Vulkan 资源
// ============================================================
void VulkanDevice::Shutdown() {
    // 1. 等待 GPU 完成所有工作
    if (m_Device) vkDeviceWaitIdle(m_Device);

    // 2. 清空 PSO 缓存（等待所有外部引用释放后销毁所有缓存的 Vulkan 对象）
    m_PSOCache.clear();

    // 3. 立即执行延迟销毁队列中的所有待处理资源（GPU 已 idle，安全）
    m_DeferredDestroy.FlushAll();

    // 4. 销毁 VMA 分配器（在清理 Vulkan 资源之前）
    if (m_VmaAllocator) { vmaDestroyAllocator(m_VmaAllocator); m_VmaAllocator = VK_NULL_HANDLE; }

    for (auto& info : m_DescLayoutInfos)
        vkDestroyDescriptorSetLayout(m_Device, info.layout, nullptr);
    m_DescLayoutInfos.clear();
    m_DescSets.clear();
    m_DescSetLayoutParents.clear();

    if (m_DescPool)        { vkDestroyDescriptorPool(m_Device, m_DescPool, nullptr); m_DescPool = VK_NULL_HANDLE; }

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

// ============================================================
// CreateSurface — Win32 窗口表面
// ============================================================
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

// ============================================================
// SelectPhysicalDevice — 选取独立 GPU
// ============================================================
void VulkanDevice::SelectPhysicalDevice() {
    u32 count = 0;
    vkEnumeratePhysicalDevices(m_Instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(m_Instance, &count, devices.data());

    HE_ASSERT(count > 0, "No Vulkan-capable GPU found");

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

// ============================================================
// CreateLogicalDevice — 创建 VkDevice + VMA 分配器 + 扩展函数加载
// ============================================================
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

    // 条件启用 Ray Tracing 扩展
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR rtPipelineFeature{};
    rtPipelineFeature.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR;
    rtPipelineFeature.rayTracingPipeline = VK_TRUE;

    VkPhysicalDeviceAccelerationStructureFeaturesKHR asFeature{};
    asFeature.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
    asFeature.accelerationStructure = VK_TRUE;

    VkPhysicalDeviceRayTracingPositionFetchFeaturesKHR posFetchFeature{};
    posFetchFeature.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_POSITION_FETCH_FEATURES_KHR;
    posFetchFeature.rayTracingPositionFetch = VK_TRUE;

    if (m_SupportsRT) {
        deviceExtensions.push_back(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
        deviceExtensions.push_back(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME);
        deviceExtensions.push_back(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
        if (m_SupportsRTPositionFetch) {
            deviceExtensions.push_back(VK_KHR_RAY_TRACING_POSITION_FETCH_EXTENSION_NAME);
            HE_CORE_INFO("RT 扩展已启用: VK_KHR_acceleration_structure + VK_KHR_ray_tracing_pipeline + position_fetch");
        } else {
            HE_CORE_INFO("RT 扩展已启用: VK_KHR_acceleration_structure + VK_KHR_ray_tracing_pipeline (position_fetch 不可用)");
        }
    }

    // 条件启用 Mesh Shader 扩展
    VkPhysicalDeviceMeshShaderFeaturesEXT meshFeature{};
    meshFeature.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT;
    meshFeature.taskShader = VK_TRUE;
    meshFeature.meshShader = VK_TRUE;

    if (m_SupportsMesh) {
        deviceExtensions.push_back(VK_EXT_MESH_SHADER_EXTENSION_NAME);
        HE_CORE_INFO("Mesh Shader 扩展已启用: VK_EXT_mesh_shader");
    }

    // 条件启用 DGC 扩展
    VkPhysicalDeviceDeviceGeneratedCommandsFeaturesEXT dgcFeature{};
    dgcFeature.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_GENERATED_COMMANDS_FEATURES_EXT;
    dgcFeature.deviceGeneratedCommands = VK_TRUE;

    if (m_SupportsDGC) {
        deviceExtensions.push_back(VK_EXT_DEVICE_GENERATED_COMMANDS_EXTENSION_NAME);
        HE_CORE_INFO("DGC 扩展已启用: VK_EXT_device_generated_commands");
    }

    // Bindless: descriptor indexing 特性
    VkPhysicalDeviceDescriptorIndexingFeatures descIndexing{};
    descIndexing.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;
    descIndexing.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    descIndexing.runtimeDescriptorArray = VK_TRUE;
    descIndexing.descriptorBindingVariableDescriptorCount = VK_TRUE;
    descIndexing.descriptorBindingPartiallyBound = VK_TRUE;
    descIndexing.descriptorBindingStorageBufferUpdateAfterBind = VK_TRUE;
    descIndexing.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
    descIndexing.descriptorBindingStorageImageUpdateAfterBind = VK_TRUE;    // StorageImage bindless
    descIndexing.descriptorBindingUniformBufferUpdateAfterBind = VK_TRUE;   // UniformBuffer bindless
    descIndexing.descriptorBindingStorageTexelBufferUpdateAfterBind = VK_TRUE; // TexelBuffer bindless
    descIndexing.shaderUniformBufferArrayNonUniformIndexing = VK_TRUE;
    descIndexing.shaderStorageBufferArrayNonUniformIndexing = VK_TRUE;

    // Vulkan 1.1: shaderDrawParameters（SPIR-V gl_DrawID 需要）
    VkPhysicalDeviceVulkan11Features vulkan11Features{};
    vulkan11Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    vulkan11Features.shaderDrawParameters = VK_TRUE;

    // bufferDeviceAddress
    VkPhysicalDeviceBufferDeviceAddressFeatures addrFeature{};
    addrFeature.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;
    addrFeature.bufferDeviceAddress = VK_TRUE;

    // Timeline Semaphore
    VkPhysicalDeviceTimelineSemaphoreFeatures timelineFeature{};
    timelineFeature.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
    timelineFeature.timelineSemaphore = VK_TRUE;

    // pNext 链: descIndexing → vulkan11 → addrFeature → timelineFeature → [RT] → [Mesh] → [DGC]
    void** ppNext = &descIndexing.pNext;
    *ppNext = &vulkan11Features; ppNext = &vulkan11Features.pNext;
    *ppNext = &addrFeature; ppNext = &addrFeature.pNext;
    *ppNext = &timelineFeature; ppNext = &timelineFeature.pNext;
    if (m_SupportsRT) {
        *ppNext = &asFeature; ppNext = &asFeature.pNext;
        *ppNext = &rtPipelineFeature; ppNext = &rtPipelineFeature.pNext;
        if (m_SupportsRTPositionFetch) {
            *ppNext = &posFetchFeature; ppNext = &posFetchFeature.pNext;
        }
    }
    if (m_SupportsMesh) {
        *ppNext = &meshFeature; ppNext = &meshFeature.pNext;
    }
    // Compute Shader Derivatives（粒子系统 SSAO 等 Compute Shader 中使用 ddx/ddy）
    VkPhysicalDeviceComputeShaderDerivativesFeaturesKHR computeDerivativesFeature{};
    bool hasDerivatives = false;
    {
        u32 extCount = 0;
        vkEnumerateDeviceExtensionProperties(m_Physical, nullptr, &extCount, nullptr);
        std::vector<VkExtensionProperties> exts(extCount);
        vkEnumerateDeviceExtensionProperties(m_Physical, nullptr, &extCount, exts.data());
        for (auto& e : exts) {
            if (strcmp(e.extensionName, VK_KHR_COMPUTE_SHADER_DERIVATIVES_EXTENSION_NAME) == 0)
                { hasDerivatives = true; break; }
        }
        if (hasDerivatives) {
            computeDerivativesFeature.sType =
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COMPUTE_SHADER_DERIVATIVES_FEATURES_KHR;
            computeDerivativesFeature.computeDerivativeGroupQuads = VK_TRUE;
            deviceExtensions.push_back(VK_KHR_COMPUTE_SHADER_DERIVATIVES_EXTENSION_NAME);
            HE_CORE_INFO("Compute Shader Derivatives 扩展已启用");
        }
    }

    if (m_SupportsDGC) {
        *ppNext = &dgcFeature; ppNext = &dgcFeature.pNext;
    }
    if (hasDerivatives) {
        *ppNext = &computeDerivativesFeature; ppNext = &computeDerivativesFeature.pNext;
    }
    *ppNext = nullptr;

    VkPhysicalDeviceFeatures features{};
    features.multiDrawIndirect = VK_TRUE;  // GPU Driven 需要多绘制间接

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

    // 加载扩展函数指针（必须在 vkCreateDevice 之后）
    LoadRTFunctions();       // → VulkanDevice_RT.cpp
    LoadMeshFunctions();     // → VulkanDevice_MeshShader.cpp
    LoadDGCFunctions();      // → VulkanDevice_MeshShader.cpp
    LoadDebugUtilsFunctions(); // → VulkanDevice_DebugUtils.cpp

    // 获取队列句柄
    vkGetDeviceQueue(m_Device, m_GraphicsFamily, 0, &m_GraphicsQueue);
    if (m_HasAsyncCompute) {
        vkGetDeviceQueue(m_Device, m_ComputeFamily, 0, &m_ComputeQueue);
        HE_CORE_INFO("Logical device created (Graphics family={} + AsyncCompute family={})",
                     m_GraphicsFamily, m_ComputeFamily);
    } else {
        m_ComputeQueue = m_GraphicsQueue;
        HE_CORE_INFO("Logical device created (Graphics+Compute shared, family={})",
                     m_GraphicsFamily);
    }

    // 创建 VMA 分配器
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

// ============================================================
// CreateCommandPools
// ============================================================
void VulkanDevice::CreateCommandPools() {
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

    poolInfo.queueFamilyIndex = m_GraphicsFamily;
    vkCreateCommandPool(m_Device, &poolInfo, nullptr, &m_GraphicsCmdPool);

    poolInfo.queueFamilyIndex = m_ComputeFamily;
    vkCreateCommandPool(m_Device, &poolInfo, nullptr, &m_ComputeCmdPool);

    HE_CORE_INFO("Command pools: Graphics(family={}), Compute(family={})",
                 m_GraphicsFamily, m_ComputeFamily);
}

// ============================================================
// 资源创建工厂方法
// ============================================================
std::unique_ptr<IRHISwapChain> VulkanDevice::CreateSwapChain(const SwapChainDesc& desc) {
    return std::make_unique<VulkanSwapChain>(m_Device, m_Physical, m_Surface,
                                            m_GraphicsQueue, desc);
}

std::unique_ptr<IRHICommandList> VulkanDevice::CreateCommandList(QueueType queue) {
    VkQueue vkQueue = m_GraphicsQueue;
    u32     family  = m_GraphicsFamily;

    if (queue == QueueType::Compute) {
        vkQueue = m_ComputeQueue;
        family  = m_ComputeFamily;
    } else if (queue == QueueType::Copy) {
        vkQueue = m_GraphicsQueue;
        family  = m_GraphicsFamily;
    }

    auto cmdList = std::make_unique<VulkanCommandList>(m_Device, vkQueue, family, this);
    cmdList->SetQueueType(queue);
    return cmdList;
}

std::unique_ptr<IRHICommandList> VulkanDevice::CreateSecondaryCommandList() {
    return std::make_unique<VulkanCommandList>(m_Device, m_GraphicsFamily, this);
}

// ============================================================
// 队列管理
// ============================================================
void VulkanDevice::WaitIdle() {
    if (m_Device) vkDeviceWaitIdle(m_Device);
}

void VulkanDevice::Submit(IRHICommandList* cmdList) {
    auto* vulkanCmd = static_cast<VulkanCommandList*>(cmdList);
    vulkanCmd->Submit();
}

bool VulkanDevice::HasAsyncComputeQueue() const {
    return m_HasAsyncCompute;
}

u32 VulkanDevice::GetQueueFamily(QueueType queue) const {
    switch (queue) {
        case QueueType::Compute:  return m_ComputeFamily;
        case QueueType::Graphics: return m_GraphicsFamily;
        case QueueType::Copy:     return m_GraphicsFamily;
        default:                  return m_GraphicsFamily;
    }
}

// ============================================================
// Timeline Semaphore — 跨队列同步
// ============================================================
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
    return static_cast<RHIFenceHandle>(m_Fences.size());
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
    std::vector<VulkanCommandList*> gfxLists, compLists;
    for (auto* cl : cmdLists) {
        auto* vkCl = static_cast<VulkanCommandList*>(cl);
        if (vkCl->GetQueueType() == QueueType::Compute)
            compLists.push_back(vkCl);
        else
            gfxLists.push_back(vkCl);
    }

    for (auto* cl : gfxLists) cl->Submit();
    for (auto* cl : compLists) cl->Submit();
}

// ============================================================
// GPU Query
// ============================================================
std::unique_ptr<IRHIQueryPool> VulkanDevice::CreateQueryPool(u32 queryCount, QueryType type) {
    return std::make_unique<VulkanQueryPool>(m_Device, queryCount, type);
}

float VulkanDevice::GetTimestampPeriod() {
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(m_Physical, &props);
    return float(props.limits.timestampPeriod);
}

// ============================================================
// 资源创建委托（实现在 VulkanResources.cpp 和 VulkanPipeline.cpp）
// ============================================================
std::unique_ptr<IRHIBuffer> VulkanDevice::CreateBuffer(const BufferDesc& desc) {
    return CreateVulkanBuffer(m_VmaAllocator, desc);
}

std::unique_ptr<IRHIPipelineState> VulkanDevice::CreatePipelineState(const PipelineStateDesc& desc) {
    std::vector<VkDescriptorSetLayout> descLayouts;
    for (auto& handle : desc.descriptorSetLayouts) {
        VkDescriptorSetLayout l = ResolveDescriptorSetLayout(handle);
        if (l != VK_NULL_HANDLE)
            descLayouts.push_back(l);
    }
    // 传入 this 指针以使用 PSO 缓存 + 延迟销毁队列
    return CreateVulkanPipeline(m_Device, desc, descLayouts, this);
}

// ============================================================
// PSO 缓存方法
// ============================================================

void VulkanDevice::InsertPSOToCache(uint64_t hash, VkPipeline pipeline,
                                    VkPipelineLayout layout, VkRenderPass rp) {
    PSOCacheEntryInternal entry;
    entry.pipeline       = pipeline;
    entry.layout         = layout;
    entry.renderPass     = rp;
    entry.lastUsedFrame  = m_CurrentFrame;
    entry.refCount       = std::make_shared<u32>(0);
    m_PSOCache[hash] = std::move(entry);
    HE_CORE_INFO("PSO cache: inserted new entry (hash=0x{:016x}, total={})",
                 hash, m_PSOCache.size());
}

std::shared_ptr<u32> VulkanDevice::GetCachedPSORef(uint64_t hash,
                                                    VkPipeline& outPipeline,
                                                    VkPipelineLayout& outLayout,
                                                    VkRenderPass& outRenderPass) {
    auto it = m_PSOCache.find(hash);
    if (it == m_PSOCache.end()) return nullptr;

    auto& entry = it->second;
    // 校验缓存条目有效性
    if (entry.pipeline == VK_NULL_HANDLE) return nullptr;

    entry.lastUsedFrame = m_CurrentFrame;
    outPipeline = entry.pipeline;
    outLayout   = entry.layout;
    outRenderPass = entry.renderPass;
    return entry.refCount;  // 共享引用计数
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
