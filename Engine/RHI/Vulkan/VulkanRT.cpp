// ============================================================
// VulkanRT.cpp — Ray Tracing 全部实现
//
// 从以下文件合并：
//   VulkanDevice_RT.cpp  — VulkanDevice 的 RT 方法（查询/加载/创建）
//   VulkanRayTracing.cpp — AS / RTPSO 资源类型的构造析构
// ============================================================

#include "RHI/RHI.h"
#include "RHI/Shader.h"
#include "Core/Log.h"

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include "VulkanRT.h"
#include "VulkanDevice.h"
#include "Core/Assert.h"

#include <vector>
#include <cstring>

namespace he::rhi {

// ============================================================
// VulkanAccelerationStructure 实现
// ============================================================

VulkanAccelerationStructure::VulkanAccelerationStructure(
    VkDevice device, VmaAllocator allocator,
    AccelerationStructureType type, const VulkanRTDispatch& rt, u64 asSize)
    : m_Device(device), m_Allocator(allocator), m_Type(type), m_Size(asSize)
    , m_DestroyAS(rt.destroyAS)
{
    // 1. 创建底层存储缓冲（DEVICE_LOCAL，GPU 端读写）
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size  = asSize;
    bufferInfo.usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR
                     | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO;

    VkResult result = vmaCreateBuffer(allocator, &bufferInfo, &allocInfo,
                                       &m_Buffer, &m_Allocation, nullptr);
    HE_ASSERT(result == VK_SUCCESS, "VMA: 创建 AS 存储缓冲失败");

    VkBufferDeviceAddressInfo addrInfo{};
    addrInfo.sType  = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    addrInfo.buffer = m_Buffer;
    m_BufferAddress = vkGetBufferDeviceAddress(device, &addrInfo);

    // 2. 创建 VkAccelerationStructureKHR
    VkAccelerationStructureCreateInfoKHR asInfo{};
    asInfo.sType  = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
    asInfo.buffer = m_Buffer;
    asInfo.size   = asSize;
    asInfo.type   = (type == AccelerationStructureType::BottomLevel)
                    ? VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR
                    : VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;

    result = rt.createAS(device, &asInfo, nullptr, &m_AS);
    HE_ASSERT(result == VK_SUCCESS, "创建 VkAccelerationStructureKHR 失败");

    // 3. 获取 AS GPU 地址
    VkAccelerationStructureDeviceAddressInfoKHR asAddrInfo{};
    asAddrInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
    asAddrInfo.accelerationStructure = m_AS;
    m_DeviceAddress = rt.getASDeviceAddress(device, &asAddrInfo);

    HE_CORE_INFO("VulkanAccelerationStructure: type={} size={}MB deviceAddr={:#x}",
                 (type == AccelerationStructureType::BottomLevel) ? "BLAS" : "TLAS",
                 asSize / (1024 * 1024), m_DeviceAddress);
}

VulkanAccelerationStructure::~VulkanAccelerationStructure() {
    if (m_AS && m_DestroyAS) {
        m_DestroyAS(m_Device, m_AS, nullptr);
    }
    if (m_Buffer) {
        vmaDestroyBuffer(m_Allocator, m_Buffer, m_Allocation);
    }
}

// ============================================================
// VulkanRTPipelineState 实现
// ============================================================

VulkanRTPipelineState::VulkanRTPipelineState(
    VkDevice device, VkPipeline pipeline, VkPipelineLayout layout,
    u32 groupCount, u32 handleSize, std::vector<u8> handles)
    : m_Device(device), m_Pipeline(pipeline), m_PipelineLayout(layout)
    , m_GroupCount(groupCount), m_HandleSize(handleSize)
    , m_Handles(std::move(handles))
{
    HE_CORE_INFO("VulkanRTPipelineState: {} groups, handleSize={}, totalHandles={}B",
                 groupCount, handleSize, m_Handles.size());
}

VulkanRTPipelineState::~VulkanRTPipelineState() {
    if (m_Pipeline)       vkDestroyPipeline(m_Device, m_Pipeline, nullptr);
    if (m_PipelineLayout) vkDestroyPipelineLayout(m_Device, m_PipelineLayout, nullptr);
}

// ============================================================
// VulkanDevice::LoadRTFunctions — 加载 RT 扩展函数指针
// ============================================================
void VulkanDevice::LoadRTFunctions() {
    if (!m_SupportsRT) return;

    m_RT.createAS              = reinterpret_cast<PFN_vkCreateAccelerationStructureKHR>(
        vkGetDeviceProcAddr(m_Device, "vkCreateAccelerationStructureKHR"));
    m_RT.destroyAS             = reinterpret_cast<PFN_vkDestroyAccelerationStructureKHR>(
        vkGetDeviceProcAddr(m_Device, "vkDestroyAccelerationStructureKHR"));
    m_RT.getASBuildSizes       = reinterpret_cast<PFN_vkGetAccelerationStructureBuildSizesKHR>(
        vkGetDeviceProcAddr(m_Device, "vkGetAccelerationStructureBuildSizesKHR"));
    m_RT.cmdBuildAS            = reinterpret_cast<PFN_vkCmdBuildAccelerationStructuresKHR>(
        vkGetDeviceProcAddr(m_Device, "vkCmdBuildAccelerationStructuresKHR"));
    m_RT.getASDeviceAddress    = reinterpret_cast<PFN_vkGetAccelerationStructureDeviceAddressKHR>(
        vkGetDeviceProcAddr(m_Device, "vkGetAccelerationStructureDeviceAddressKHR"));
    m_RT.createRTPipelines     = reinterpret_cast<PFN_vkCreateRayTracingPipelinesKHR>(
        vkGetDeviceProcAddr(m_Device, "vkCreateRayTracingPipelinesKHR"));
    m_RT.getRTShaderGroupHandles = reinterpret_cast<PFN_vkGetRayTracingShaderGroupHandlesKHR>(
        vkGetDeviceProcAddr(m_Device, "vkGetRayTracingShaderGroupHandlesKHR"));
    m_RT.cmdTraceRays          = reinterpret_cast<PFN_vkCmdTraceRaysKHR>(
        vkGetDeviceProcAddr(m_Device, "vkCmdTraceRaysKHR"));

    HE_ASSERT(m_RT.createAS,              "加载 vkCreateAccelerationStructureKHR 失败");
    HE_ASSERT(m_RT.destroyAS,             "加载 vkDestroyAccelerationStructureKHR 失败");
    HE_ASSERT(m_RT.getASBuildSizes,       "加载 vkGetAccelerationStructureBuildSizesKHR 失败");
    HE_ASSERT(m_RT.cmdBuildAS,            "加载 vkCmdBuildAccelerationStructuresKHR 失败");
    HE_ASSERT(m_RT.getASDeviceAddress,    "加载 vkGetAccelerationStructureDeviceAddressKHR 失败");
    HE_ASSERT(m_RT.createRTPipelines,     "加载 vkCreateRayTracingPipelinesKHR 失败");
    HE_ASSERT(m_RT.getRTShaderGroupHandles, "加载 vkGetRayTracingShaderGroupHandlesKHR 失败");
    HE_ASSERT(m_RT.cmdTraceRays,          "加载 vkCmdTraceRaysKHR 失败");

    HE_CORE_INFO("RT 扩展函数全部加载成功");
}

// ============================================================
// VulkanDevice::QueryRTCapabilities — 查询硬件 RT 能力
// ============================================================
void VulkanDevice::QueryRTCapabilities() {
    u32 extCount = 0;
    vkEnumerateDeviceExtensionProperties(m_Physical, nullptr, &extCount, nullptr);
    std::vector<VkExtensionProperties> extensions(extCount);
    vkEnumerateDeviceExtensionProperties(m_Physical, nullptr, &extCount, extensions.data());

    bool hasAS  = false;
    bool hasRTP = false;
    bool hasDHO = false;
    bool hasPosFetch = false;

    for (auto& ext : extensions) {
        if (strcmp(ext.extensionName, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) == 0)
            hasAS = true;
        if (strcmp(ext.extensionName, VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME) == 0)
            hasRTP = true;
        if (strcmp(ext.extensionName, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME) == 0)
            hasDHO = true;
        if (strcmp(ext.extensionName, VK_KHR_RAY_TRACING_POSITION_FETCH_EXTENSION_NAME) == 0)
            hasPosFetch = true;
    }
    m_SupportsRTPositionFetch = hasPosFetch;

    m_SupportsRT = hasAS && hasRTP;
    if (!m_SupportsRT) {
        HE_CORE_INFO("Ray Tracing: 不支持（缺少 VK_KHR_acceleration_structure 或 VK_KHR_ray_tracing_pipeline）");
        return;
    }

    HE_CORE_INFO("Ray Tracing: 硬件支持已检测 (AS={}, RTPipeline={}, DeferredHostOps={})",
                 hasAS, hasRTP, hasDHO);

    VkPhysicalDeviceRayTracingPipelinePropertiesKHR rtProps{};
    rtProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR;

    VkPhysicalDeviceAccelerationStructurePropertiesKHR asProps{};
    asProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR;

    rtProps.pNext = &asProps;

    VkPhysicalDeviceProperties2 props2{};
    props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    props2.pNext = &rtProps;

    vkGetPhysicalDeviceProperties2(m_Physical, &props2);

    m_MaxRayRecursionDepth     = rtProps.maxRayRecursionDepth;
    m_ShaderGroupHandleSize    = rtProps.shaderGroupHandleSize;
    m_ShaderGroupBaseAlignment = rtProps.shaderGroupBaseAlignment;
    m_MaxRTDispatchSize        = rtProps.maxRayDispatchInvocationCount;
    m_MaxASInstanceCount       = asProps.maxInstanceCount;
    m_MaxASGeometryCount       = asProps.maxGeometryCount;
    m_MaxASPrimitiveCount      = asProps.maxPrimitiveCount;
    m_MinASScratchAlignment    = asProps.minAccelerationStructureScratchOffsetAlignment;

    HE_CORE_INFO("RT 属性: maxRecursion={}, groupHandleSize={}, groupAlign={}, maxDispatch={}",
                 m_MaxRayRecursionDepth, m_ShaderGroupHandleSize,
                 m_ShaderGroupBaseAlignment, m_MaxRTDispatchSize);
    HE_CORE_INFO("AS  属性: maxInstances={}, maxGeometries={}, maxPrimitives={}, scratchAlign={}",
                 m_MaxASInstanceCount, m_MaxASGeometryCount,
                 m_MaxASPrimitiveCount, m_MinASScratchAlignment);
}

// ============================================================
// VulkanDevice::CreateBLAS
// ============================================================
std::unique_ptr<IRHIAccelerationStructure>
VulkanDevice::CreateBLAS(const BLASBuildDesc& desc) {
    if (!m_SupportsRT) {
        HE_CORE_ERROR("CreateBLAS: 设备不支持 Ray Tracing");
        return nullptr;
    }

    ASBuildSizes sizes = GetBLASBuildSizes(desc);
    u64 asSize = sizes.accelerationStructureSize;

    auto blas = std::make_unique<VulkanAccelerationStructure>(
        m_Device, m_VmaAllocator,
        AccelerationStructureType::BottomLevel, m_RT, asSize);

    blas->SetBLASDesc(desc);

    HE_CORE_INFO("CreateBLAS: {} geometries, AS size={}MB, scratch={}MB",
                 desc.geometries.size(), asSize / (1024 * 1024),
                 sizes.buildScratchSize / (1024 * 1024));
    return blas;
}

// ============================================================
// VulkanDevice::CreateTLAS
// ============================================================
std::unique_ptr<IRHIAccelerationStructure>
VulkanDevice::CreateTLAS(const TLASBuildDesc& desc) {
    if (!m_SupportsRT) {
        HE_CORE_ERROR("CreateTLAS: 设备不支持 Ray Tracing");
        return nullptr;
    }

    ASBuildSizes sizes = GetTLASBuildSizes(desc.maxInstanceCount);
    u64 asSize = sizes.accelerationStructureSize;

    auto tlas = std::make_unique<VulkanAccelerationStructure>(
        m_Device, m_VmaAllocator,
        AccelerationStructureType::TopLevel, m_RT, asSize);

    HE_CORE_INFO("CreateTLAS: maxInstances={}, AS size={}MB, scratch={}MB",
                 desc.maxInstanceCount, asSize / (1024 * 1024),
                 sizes.buildScratchSize / (1024 * 1024));
    return tlas;
}

// ============================================================
// VulkanDevice::GetBLASBuildSizes
// ============================================================
ASBuildSizes VulkanDevice::GetBLASBuildSizes(const BLASBuildDesc& desc) {
    if (!m_SupportsRT || !m_RT.getASBuildSizes) return ASBuildSizes{};

    std::vector<VkAccelerationStructureGeometryKHR> vkGeometries;
    std::vector<u32> maxPrimCounts;
    for (auto& g : desc.geometries) {
        VkAccelerationStructureGeometryKHR vkGeo{};
        vkGeo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
        vkGeo.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        vkGeo.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        vkGeo.geometry.triangles.vertexFormat = ToVkFormat(g.vertexFormat);
        vkGeo.geometry.triangles.vertexStride = g.vertexStride;
        vkGeo.geometry.triangles.maxVertex     = g.maxVertex;
        vkGeo.geometry.triangles.indexType     = ToVkFormat(g.indexFormat) == VK_FORMAT_R32_UINT
                                                 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
        vkGeo.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
        vkGeometries.push_back(vkGeo);
        maxPrimCounts.push_back(g.maxPrimitiveCount);
    }

    VkAccelerationStructureBuildGeometryInfoKHR buildInfo{};
    buildInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    buildInfo.type          = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    buildInfo.flags         = ToVkBuildFlags(desc.flags);
    buildInfo.geometryCount = static_cast<u32>(vkGeometries.size());
    buildInfo.pGeometries   = vkGeometries.data();

    VkAccelerationStructureBuildSizesInfoKHR sizeInfo{};
    sizeInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;

    m_RT.getASBuildSizes(m_Device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                         &buildInfo, maxPrimCounts.data(), &sizeInfo);

    ASBuildSizes sizes;
    sizes.accelerationStructureSize = sizeInfo.accelerationStructureSize;
    sizes.buildScratchSize          = sizeInfo.buildScratchSize;
    sizes.updateScratchSize         = sizeInfo.updateScratchSize;
    return sizes;
}

// ============================================================
// VulkanDevice::GetTLASBuildSizes
// ============================================================
ASBuildSizes VulkanDevice::GetTLASBuildSizes(u32 maxInstanceCount) {
    if (!m_SupportsRT || !m_RT.getASBuildSizes) return ASBuildSizes{};

    VkAccelerationStructureGeometryKHR tlasGeo{};
    tlasGeo.sType        = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    tlasGeo.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;

    VkAccelerationStructureBuildGeometryInfoKHR buildInfo{};
    buildInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    buildInfo.type          = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    buildInfo.flags         = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    buildInfo.geometryCount = 1;
    buildInfo.pGeometries   = &tlasGeo;

    VkAccelerationStructureBuildSizesInfoKHR sizeInfo{};
    sizeInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;

    m_RT.getASBuildSizes(m_Device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                         &buildInfo, &maxInstanceCount, &sizeInfo);

    ASBuildSizes sizes;
    sizes.accelerationStructureSize = sizeInfo.accelerationStructureSize;
    sizes.buildScratchSize          = sizeInfo.buildScratchSize;
    sizes.updateScratchSize         = sizeInfo.updateScratchSize;
    return sizes;
}

// ============================================================
// VulkanDevice::CreateRTPipelineState
// ============================================================
std::unique_ptr<IRHIRayTracingPipelineState>
VulkanDevice::CreateRTPipelineState(const RTPipelineStateDesc& desc) {
    if (!m_SupportsRT || !m_RT.createRTPipelines) {
        HE_CORE_ERROR("CreateRTPipelineState: 设备不支持 Ray Tracing");
        return nullptr;
    }

    std::vector<VkPipelineShaderStageCreateInfo> stages;
    std::vector<VkShaderModule> modules;

    for (u32 i = 0; i < desc.shaders.size(); ++i) {
        auto& bc = desc.shaders[i];
        if (bc.spirv.empty()) continue;

        VkShaderModuleCreateInfo modInfo{};
        modInfo.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        modInfo.codeSize = bc.spirv.size() * sizeof(u32);
        modInfo.pCode    = bc.spirv.data();

        VkShaderModule mod;
        vkCreateShaderModule(m_Device, &modInfo, nullptr, &mod);
        modules.push_back(mod);

        VkShaderStageFlagBits vkStage;
        switch (bc.stage) {
            case ShaderStage::RayGen:       vkStage = VK_SHADER_STAGE_RAYGEN_BIT_KHR; break;
            case ShaderStage::Miss:         vkStage = VK_SHADER_STAGE_MISS_BIT_KHR; break;
            case ShaderStage::ClosestHit:   vkStage = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR; break;
            case ShaderStage::AnyHit:       vkStage = VK_SHADER_STAGE_ANY_HIT_BIT_KHR; break;
            case ShaderStage::Callable:     vkStage = VK_SHADER_STAGE_CALLABLE_BIT_KHR; break;
            default:
                HE_CORE_WARN("CreateRTPipelineState: 跳过不支持的 stage 类型 ({})", int(bc.stage));
                continue;
        }

        VkPipelineShaderStageCreateInfo stageInfo{};
        stageInfo.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stageInfo.stage  = vkStage;
        stageInfo.module = mod;
        stageInfo.pName  = bc.entryPoint.c_str();
        stages.push_back(stageInfo);
    }

    std::vector<VkRayTracingShaderGroupCreateInfoKHR> vkGroups;
    for (auto& group : desc.shaderGroups) {
        VkRayTracingShaderGroupCreateInfoKHR vkGroup{};
        vkGroup.sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;

        switch (group.type) {
            case RTShaderGroupType::RayGen:
                vkGroup.type               = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
                vkGroup.generalShader      = group.generalShader;
                vkGroup.closestHitShader   = VK_SHADER_UNUSED_KHR;
                vkGroup.anyHitShader       = VK_SHADER_UNUSED_KHR;
                vkGroup.intersectionShader = VK_SHADER_UNUSED_KHR;
                break;
            case RTShaderGroupType::Miss:
                vkGroup.type               = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
                vkGroup.generalShader      = group.generalShader;
                vkGroup.closestHitShader   = VK_SHADER_UNUSED_KHR;
                vkGroup.anyHitShader       = VK_SHADER_UNUSED_KHR;
                vkGroup.intersectionShader = VK_SHADER_UNUSED_KHR;
                break;
            case RTShaderGroupType::Hit:
                vkGroup.type               = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
                vkGroup.generalShader      = VK_SHADER_UNUSED_KHR;
                vkGroup.closestHitShader   = group.closestHitShader;
                vkGroup.anyHitShader       = group.anyHitShader;
                vkGroup.intersectionShader = group.intersectionShader;
                break;
            case RTShaderGroupType::Callable:
                vkGroup.type               = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
                vkGroup.generalShader      = group.generalShader;
                vkGroup.closestHitShader   = VK_SHADER_UNUSED_KHR;
                vkGroup.anyHitShader       = VK_SHADER_UNUSED_KHR;
                vkGroup.intersectionShader = VK_SHADER_UNUSED_KHR;
                break;
        }
        vkGroups.push_back(vkGroup);
    }

    std::vector<VkDescriptorSetLayout> descLayouts;
    for (auto& handle : desc.descriptorSetLayouts) {
        VkDescriptorSetLayout l = ResolveDescriptorSetLayout(handle);
        if (l != VK_NULL_HANDLE) descLayouts.push_back(l);
    }

    std::vector<VkPushConstantRange> vkPushRanges;
    for (auto& pc : desc.pushConstantRanges) {
        VkPushConstantRange range{};
        range.stageFlags = pc.stageMask;
        range.offset     = pc.offset;
        range.size       = pc.size;
        vkPushRanges.push_back(range);
    }

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount         = static_cast<u32>(descLayouts.size());
    layoutInfo.pSetLayouts            = descLayouts.empty() ? nullptr : descLayouts.data();
    layoutInfo.pushConstantRangeCount = static_cast<u32>(vkPushRanges.size());
    layoutInfo.pPushConstantRanges    = vkPushRanges.empty() ? nullptr : vkPushRanges.data();

    VkPipelineLayout pipelineLayout;
    vkCreatePipelineLayout(m_Device, &layoutInfo, nullptr, &pipelineLayout);

    VkRayTracingPipelineCreateInfoKHR rtInfo{};
    rtInfo.sType                        = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR;
    rtInfo.stageCount                   = static_cast<u32>(stages.size());
    rtInfo.pStages                      = stages.data();
    rtInfo.groupCount                   = static_cast<u32>(vkGroups.size());
    rtInfo.pGroups                      = vkGroups.data();
    rtInfo.maxPipelineRayRecursionDepth = desc.maxRecursionDepth;
    rtInfo.layout                       = pipelineLayout;

    VkPipeline rtPipeline;
    VkResult result = m_RT.createRTPipelines(m_Device, VK_NULL_HANDLE, VK_NULL_HANDLE,
                                              1, &rtInfo, nullptr, &rtPipeline);
    HE_ASSERT(result == VK_SUCCESS, "Ray Tracing Pipeline 创建失败");

    u32 groupCount = static_cast<u32>(vkGroups.size());
    u32 handleSize = m_ShaderGroupHandleSize;
    u32 handleDataSize = groupCount * handleSize;
    std::vector<u8> handles(handleDataSize);
    m_RT.getRTShaderGroupHandles(m_Device, rtPipeline, 0, groupCount, handleDataSize, handles.data());

    for (auto& mod : modules) vkDestroyShaderModule(m_Device, mod, nullptr);

    HE_CORE_INFO("CreateRTPipelineState: {} groups, recursion={}, payload={}B",
                 groupCount, desc.maxRecursionDepth, desc.maxPayloadSize);
    return std::make_unique<VulkanRTPipelineState>(m_Device, rtPipeline, pipelineLayout,
                                                    groupCount, handleSize, std::move(handles));
}

} // namespace he::rhi
