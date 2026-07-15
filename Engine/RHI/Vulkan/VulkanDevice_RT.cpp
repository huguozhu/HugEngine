// VulkanDevice_RT.cpp — Ray Tracing 能力查询、函数加载与资源创建
// 从 VulkanDevice.cpp 拆分，包含：
//   - QueryRTCapabilities / LoadRTFunctions
//   - CreateBLAS / CreateTLAS / GetBLASBuildSizes / GetTLASBuildSizes
//   - CreateRTPipelineState

#include "RHI/RHI.h"
#include "RHI/Shader.h"
#include "Core/Log.h"

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include "VulkanDevice.h"
#include "Core/Assert.h"

#include <vector>
#include <cstring>

namespace he::rhi {

// ============================================================
// RT 扩展函数加载（设备创建后调用一次）
// ============================================================
void VulkanDevice::LoadRTFunctions() {
    if (!m_SupportsRT) return;

    // 加载所有 RT 扩展函数指针（vkGetDeviceProcAddr）
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

    // 验证所有函数加载成功
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
// RT / Mesh Shader 能力检测
// ============================================================

void VulkanDevice::QueryRTCapabilities() {
    // 1. 检查设备扩展是否可用
    u32 extCount = 0;
    vkEnumerateDeviceExtensionProperties(m_Physical, nullptr, &extCount, nullptr);
    std::vector<VkExtensionProperties> extensions(extCount);
    vkEnumerateDeviceExtensionProperties(m_Physical, nullptr, &extCount, extensions.data());

    bool hasAS  = false;  // VK_KHR_acceleration_structure
    bool hasRTP = false;  // VK_KHR_ray_tracing_pipeline
    bool hasDHO = false;  // VK_KHR_deferred_host_operations
    bool hasPosFetch = false;  // VK_KHR_ray_tracing_position_fetch（可选）

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

    // 2. 查询 RT Pipeline 属性（通过 pNext 链）
    VkPhysicalDeviceRayTracingPipelinePropertiesKHR rtProps{};
    rtProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR;

    VkPhysicalDeviceAccelerationStructurePropertiesKHR asProps{};
    asProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR;
    asProps.pNext = nullptr;

    // pNext 链: base → rtProps → asProps
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
// RT 资源创建
// ============================================================

std::unique_ptr<IRHIAccelerationStructure>
VulkanDevice::CreateBLAS(const BLASBuildDesc& desc) {
    if (!m_SupportsRT) {
        HE_CORE_ERROR("CreateBLAS: 设备不支持 Ray Tracing");
        return nullptr;
    }

    // 1. 查询构建所需内存大小
    ASBuildSizes sizes = GetBLASBuildSizes(desc);
    u64 asSize = sizes.accelerationStructureSize;

    // 2. 创建 AS 对象 + 底层缓冲
    auto blas = std::make_unique<VulkanAccelerationStructure>(
        m_Device, m_VmaAllocator,
        AccelerationStructureType::BottomLevel, m_RT, asSize);

    // 3. 存储 BLAS 构建描述（供后续 BuildBLAS 使用）
    blas->SetBLASDesc(desc);

    HE_CORE_INFO("CreateBLAS: {} geometries, AS size={}MB, scratch={}MB",
                 desc.geometries.size(), asSize / (1024 * 1024),
                 sizes.buildScratchSize / (1024 * 1024));
    return blas;
}

std::unique_ptr<IRHIAccelerationStructure>
VulkanDevice::CreateTLAS(const TLASBuildDesc& desc) {
    if (!m_SupportsRT) {
        HE_CORE_ERROR("CreateTLAS: 设备不支持 Ray Tracing");
        return nullptr;
    }

    // 1. 查询构建所需内存大小
    ASBuildSizes sizes = GetTLASBuildSizes(desc.maxInstanceCount);
    u64 asSize = sizes.accelerationStructureSize;

    // 2. 创建 AS 对象 + 底层缓冲
    auto tlas = std::make_unique<VulkanAccelerationStructure>(
        m_Device, m_VmaAllocator,
        AccelerationStructureType::TopLevel, m_RT, asSize);

    HE_CORE_INFO("CreateTLAS: maxInstances={}, AS size={}MB, scratch={}MB",
                 desc.maxInstanceCount, asSize / (1024 * 1024),
                 sizes.buildScratchSize / (1024 * 1024));
    return tlas;
}

ASBuildSizes VulkanDevice::GetBLASBuildSizes(const BLASBuildDesc& desc) {
    if (!m_SupportsRT || !m_RT.getASBuildSizes) return ASBuildSizes{};

    // 构建 VkAccelerationStructureGeometryKHR 数组（仅用于查询大小，不需要实际缓冲地址）
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
        vkGeo.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;  // 默认不透明（后续可扩展 alpha-test 支持）
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

ASBuildSizes VulkanDevice::GetTLASBuildSizes(u32 maxInstanceCount) {
    if (!m_SupportsRT || !m_RT.getASBuildSizes) return ASBuildSizes{};

    // TLAS 需要一个 dummy geometry（类型=INSTANCES）以满足 Vulkan VUID：
    // geometryCount != 0 时，pGeometries 或 ppGeometries 必须非空
    VkAccelerationStructureGeometryKHR tlasGeo{};
    tlasGeo.sType        = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    tlasGeo.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;

    VkAccelerationStructureBuildGeometryInfoKHR buildInfo{};
    buildInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    buildInfo.type          = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    buildInfo.flags         = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    buildInfo.geometryCount = 1;  // TLAS 只有一个"几何"（实例数组）
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
// RT Pipeline State 创建
// ============================================================

std::unique_ptr<IRHIRayTracingPipelineState>
VulkanDevice::CreateRTPipelineState(const RTPipelineStateDesc& desc) {
    if (!m_SupportsRT || !m_RT.createRTPipelines) {
        HE_CORE_ERROR("CreateRTPipelineState: 设备不支持 Ray Tracing");
        return nullptr;
    }

    // 1. 从 SPIRV 创建 ShaderModule（每个 shader 一个 module）
    struct ShaderInfo {
        VkShaderModule module;
        u32 groupIndex;  // 所属 group
    };
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

        // 根据 ShaderStage 映射到 VkShaderStageFlagBits
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

    // 2. 构建 VkRayTracingShaderGroupCreateInfoKHR 数组
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

    // 3. 创建 PipelineLayout
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

    // 4. 创建 RT Pipeline
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

    // 5. 查询着色器组句柄（SBT 用）
    u32 groupCount = static_cast<u32>(vkGroups.size());
    u32 handleSize = m_ShaderGroupHandleSize;
    u32 handleDataSize = groupCount * handleSize;
    std::vector<u8> handles(handleDataSize);
    m_RT.getRTShaderGroupHandles(m_Device, rtPipeline, 0, groupCount, handleDataSize, handles.data());

    // 6. 清理 ShaderModule
    for (auto& mod : modules) vkDestroyShaderModule(m_Device, mod, nullptr);

    HE_CORE_INFO("CreateRTPipelineState: {} groups, recursion={}, payload={}B",
                 groupCount, desc.maxRecursionDepth, desc.maxPayloadSize);
    return std::make_unique<VulkanRTPipelineState>(m_Device, rtPipeline, pipelineLayout,
                                                    groupCount, handleSize, std::move(handles));
}

} // namespace he::rhi
