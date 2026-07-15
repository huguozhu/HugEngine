// VulkanDevice_MeshShader.cpp — Mesh Shader 与 DGC 能力查询、函数加载
// 从 VulkanDevice.cpp 拆分，包含：
//   - QueryMeshCapabilities / LoadMeshFunctions
//   - QueryDGCCapabilities / LoadDGCFunctions

#include "RHI/RHI.h"
#include "Core/Log.h"

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include "VulkanDGC.h"
#include "VulkanDevice.h"
#include "Core/Assert.h"

#include <cstring>

namespace he::rhi {

// ============================================================
// Mesh Shader 能力检测
// ============================================================

void VulkanDevice::QueryMeshCapabilities() {
    // 1. 检查设备扩展是否可用
    u32 extCount = 0;
    vkEnumerateDeviceExtensionProperties(m_Physical, nullptr, &extCount, nullptr);
    std::vector<VkExtensionProperties> extensions(extCount);
    vkEnumerateDeviceExtensionProperties(m_Physical, nullptr, &extCount, extensions.data());

    bool hasMesh = false;
    for (auto& ext : extensions) {
        if (strcmp(ext.extensionName, VK_EXT_MESH_SHADER_EXTENSION_NAME) == 0) {
            hasMesh = true;
            break;
        }
    }

    m_SupportsMesh = hasMesh;
    if (!m_SupportsMesh) {
        HE_CORE_INFO("Mesh Shader: 不支持（缺少 VK_EXT_mesh_shader）");
        return;
    }

    HE_CORE_INFO("Mesh Shader: 硬件支持已检测");

    // 2. 查询 Mesh Shader 属性
    VkPhysicalDeviceMeshShaderPropertiesEXT meshProps{};
    meshProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_PROPERTIES_EXT;

    VkPhysicalDeviceProperties2 props2{};
    props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    props2.pNext = &meshProps;

    vkGetPhysicalDeviceProperties2(m_Physical, &props2);

    m_MaxMeshWorkGroupInvocations = meshProps.maxMeshWorkGroupInvocations;
    m_MaxMeshOutputVertices       = meshProps.maxMeshOutputVertices;
    m_MaxMeshOutputPrimitives     = meshProps.maxMeshOutputPrimitives;
    m_MaxTaskWorkGroupInvocations = meshProps.maxTaskWorkGroupInvocations;
    m_MaxTaskPayloadSize          = meshProps.maxTaskPayloadSize;
    m_MaxMeshWorkGroupCountX      = meshProps.maxMeshWorkGroupCount[0];
    m_MaxMeshWorkGroupCountY      = meshProps.maxMeshWorkGroupCount[1];
    m_MaxMeshWorkGroupCountZ      = meshProps.maxMeshWorkGroupCount[2];

    HE_CORE_INFO("Mesh 属性: meshInvocations={}, meshVertices={}, meshPrimitives={}",
                 m_MaxMeshWorkGroupInvocations, m_MaxMeshOutputVertices, m_MaxMeshOutputPrimitives);
    HE_CORE_INFO("Mesh 属性: taskInvocations={}, taskPayload={}, workGroupCount=({},{},{})",
                 m_MaxTaskWorkGroupInvocations, m_MaxTaskPayloadSize,
                 m_MaxMeshWorkGroupCountX, m_MaxMeshWorkGroupCountY, m_MaxMeshWorkGroupCountZ);
}

// ============================================================
// Mesh Shader 扩展函数加载（设备创建后调用一次）
// ============================================================

void VulkanDevice::LoadMeshFunctions() {
    if (!m_SupportsMesh) return;

    m_CmdDrawMeshTasks = reinterpret_cast<PFN_vkCmdDrawMeshTasksEXT>(
        vkGetDeviceProcAddr(m_Device, "vkCmdDrawMeshTasksEXT"));
    m_CmdDrawMeshTasksIndirect = reinterpret_cast<PFN_vkCmdDrawMeshTasksIndirectEXT>(
        vkGetDeviceProcAddr(m_Device, "vkCmdDrawMeshTasksIndirectEXT"));
    HE_ASSERT(m_CmdDrawMeshTasks, "加载 vkCmdDrawMeshTasksEXT 失败");
    HE_ASSERT(m_CmdDrawMeshTasksIndirect, "加载 vkCmdDrawMeshTasksIndirectEXT 失败");
    HE_CORE_INFO("Mesh Shader 扩展函数加载成功");
}

// ============================================================
// DGC 能力检测 + 函数加载
// ============================================================

void VulkanDevice::QueryDGCCapabilities() {
    m_SupportsDGC = VulkanDGC::IsSupported(m_Physical);
    if (!m_SupportsDGC) {
        HE_CORE_INFO("Device Generated Commands: 不支持（缺少 VK_EXT_device_generated_commands）");
        return;
    }
    HE_CORE_INFO("Device Generated Commands: 硬件支持已检测");

    // 查询 DGC 属性（通过 VkPhysicalDeviceProperties2 pNext 链）
    VkPhysicalDeviceDeviceGeneratedCommandsPropertiesEXT dgcProps{};
    dgcProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_GENERATED_COMMANDS_PROPERTIES_EXT;

    VkPhysicalDeviceProperties2 props2{};
    props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    props2.pNext = &dgcProps;

    vkGetPhysicalDeviceProperties2(m_Physical, &props2);

    HE_CORE_INFO("DGC 属性: maxIndirectPipelineCount={}, maxIndirectSequenceCount={}, "
                 "maxIndirectCommandsTokenCount={}, maxIndirectCommandsTokenOffset={}, "
                 "maxIndirectCommandsIndirectStride={}",
                 dgcProps.maxIndirectPipelineCount,
                 dgcProps.maxIndirectSequenceCount,
                 dgcProps.maxIndirectCommandsTokenCount,
                 dgcProps.maxIndirectCommandsTokenOffset,
                 dgcProps.maxIndirectCommandsIndirectStride);
}

void VulkanDevice::LoadDGCFunctions() {
    if (!m_SupportsDGC) return;

    m_DGCFuncs.vkCreateIndirectCommandsLayoutEXT =
        reinterpret_cast<PFN_vkCreateIndirectCommandsLayoutEXT>(
            vkGetDeviceProcAddr(m_Device, "vkCreateIndirectCommandsLayoutEXT"));
    m_DGCFuncs.vkDestroyIndirectCommandsLayoutEXT =
        reinterpret_cast<PFN_vkDestroyIndirectCommandsLayoutEXT>(
            vkGetDeviceProcAddr(m_Device, "vkDestroyIndirectCommandsLayoutEXT"));
    m_DGCFuncs.vkCreateIndirectExecutionSetEXT =
        reinterpret_cast<PFN_vkCreateIndirectExecutionSetEXT>(
            vkGetDeviceProcAddr(m_Device, "vkCreateIndirectExecutionSetEXT"));
    m_DGCFuncs.vkDestroyIndirectExecutionSetEXT =
        reinterpret_cast<PFN_vkDestroyIndirectExecutionSetEXT>(
            vkGetDeviceProcAddr(m_Device, "vkDestroyIndirectExecutionSetEXT"));
    m_DGCFuncs.vkGetGeneratedCommandsMemoryRequirementsEXT =
        reinterpret_cast<PFN_vkGetGeneratedCommandsMemoryRequirementsEXT>(
            vkGetDeviceProcAddr(m_Device, "vkGetGeneratedCommandsMemoryRequirementsEXT"));
    m_DGCFuncs.vkCmdExecuteGeneratedCommandsEXT =
        reinterpret_cast<PFN_vkCmdExecuteGeneratedCommandsEXT>(
            vkGetDeviceProcAddr(m_Device, "vkCmdExecuteGeneratedCommandsEXT"));
    m_DGCFuncs.vkCmdPreprocessGeneratedCommandsEXT =
        reinterpret_cast<PFN_vkCmdPreprocessGeneratedCommandsEXT>(
            vkGetDeviceProcAddr(m_Device, "vkCmdPreprocessGeneratedCommandsEXT"));

    // 验证所有函数加载成功
    HE_ASSERT(m_DGCFuncs.vkCreateIndirectCommandsLayoutEXT,
              "加载 vkCreateIndirectCommandsLayoutEXT 失败");
    HE_ASSERT(m_DGCFuncs.vkDestroyIndirectCommandsLayoutEXT,
              "加载 vkDestroyIndirectCommandsLayoutEXT 失败");
    HE_ASSERT(m_DGCFuncs.vkCreateIndirectExecutionSetEXT,
              "加载 vkCreateIndirectExecutionSetEXT 失败");
    HE_ASSERT(m_DGCFuncs.vkDestroyIndirectExecutionSetEXT,
              "加载 vkDestroyIndirectExecutionSetEXT 失败");
    HE_ASSERT(m_DGCFuncs.vkGetGeneratedCommandsMemoryRequirementsEXT,
              "加载 vkGetGeneratedCommandsMemoryRequirementsEXT 失败");
    HE_ASSERT(m_DGCFuncs.vkCmdExecuteGeneratedCommandsEXT,
              "加载 vkCmdExecuteGeneratedCommandsEXT 失败");
    HE_ASSERT(m_DGCFuncs.vkCmdPreprocessGeneratedCommandsEXT,
              "加载 vkCmdPreprocessGeneratedCommandsEXT 失败");

    HE_CORE_INFO("DGC 扩展函数全部加载成功");
}

} // namespace he::rhi
