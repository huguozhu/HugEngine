// ============================================================
// VulkanDevice_DebugUtils.cpp — VK_EXT_debug_utils 资源命名 + 调试标签
//
// 加载 vkSetDebugUtilsObjectNameEXT（为 Buffer/Texture/Pipeline 设置调试名）
// 加载 vkCmdBeginDebugUtilsLabelEXT/vkCmdEndDebugUtilsLabelEXT（GPU 标签区域）
// 加载 vkCmdInsertDebugUtilsLabelEXT（瞬时标签）
//
// 遵循 VulkanDevice 子模块拆分模式：独立编译单元，namespace he::rhi
// ============================================================

#include "RHI/RHI.h"
#include "RHI/Buffer.h"
#include "RHI/Shader.h"
#include "RHI/RayTracing.h"
#include "Core/Log.h"

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

// Vulkan 类型的完整定义（供 static_cast 使用）
#include "VulkanDevice.h"
#include "Core/Assert.h"

namespace he::rhi {

// ============================================================
// VulkanDevice::LoadDebugUtilsFunctions — 加载调试工具函数指针
// ============================================================
void VulkanDevice::LoadDebugUtilsFunctions() {
    m_SetDebugUtilsObjectName = reinterpret_cast<PFN_vkSetDebugUtilsObjectNameEXT>(
        vkGetDeviceProcAddr(m_Device, "vkSetDebugUtilsObjectNameEXT"));
    m_CmdBeginDebugUtilsLabelEXT = reinterpret_cast<PFN_vkCmdBeginDebugUtilsLabelEXT>(
        vkGetDeviceProcAddr(m_Device, "vkCmdBeginDebugUtilsLabelEXT"));
    m_CmdEndDebugUtilsLabelEXT = reinterpret_cast<PFN_vkCmdEndDebugUtilsLabelEXT>(
        vkGetDeviceProcAddr(m_Device, "vkCmdEndDebugUtilsLabelEXT"));
    m_CmdInsertDebugUtilsLabelEXT = reinterpret_cast<PFN_vkCmdInsertDebugUtilsLabelEXT>(
        vkGetDeviceProcAddr(m_Device, "vkCmdInsertDebugUtilsLabelEXT"));

    if (m_SetDebugUtilsObjectName) {
        HE_CORE_INFO("VK_EXT_debug_utils 扩展函数加载成功（对象命名 + 调试标签）");
    } else {
        HE_CORE_WARN("VK_EXT_debug_utils 扩展不可用：调试标签与对象命名功能将跳过");
    }
}

// ============================================================
// VulkanDevice::SetResourceDebugName — 资源命名实现
//
// 使用 VkDebugUtilsObjectNameInfoEXT 为各种 GPU 资源设置调试名称，
// 在 RenderDoc / NSight / GPU Trace 中可见。
// ============================================================

// 内部辅助：通用对象命名
static void SetVkObjectName(VkDevice device,
                            PFN_vkSetDebugUtilsObjectNameEXT fn,
                            VkObjectType type, u64 handle, const char* name) {
    if (!fn || !handle || !name) return;
    VkDebugUtilsObjectNameInfoEXT info{};
    info.sType        = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
    info.objectType   = type;
    info.objectHandle = handle;
    info.pObjectName  = name;
    fn(device, &info);
}

void VulkanDevice::SetResourceDebugName(IRHIBuffer* resource, const char* name) {
    if (!resource) return;
    auto* vkBuf = static_cast<VulkanBuffer*>(resource);
    SetVkObjectName(m_Device, m_SetDebugUtilsObjectName,
                    VK_OBJECT_TYPE_BUFFER, reinterpret_cast<u64>(vkBuf->GetHandle()), name);
}

void VulkanDevice::SetResourceDebugName(IRHITexture* resource, const char* name) {
    if (!resource) return;
    auto* vkTex = static_cast<VulkanTexture*>(resource);
    SetVkObjectName(m_Device, m_SetDebugUtilsObjectName,
                    VK_OBJECT_TYPE_IMAGE, reinterpret_cast<u64>(vkTex->GetImage()), name);
}

void VulkanDevice::SetResourceDebugName(IRHIPipelineState* resource, const char* name) {
    if (!resource) return;
    auto* vkPSO = static_cast<VulkanPipelineState*>(resource);
    void* native = vkPSO->GetNativeHandle();
    SetVkObjectName(m_Device, m_SetDebugUtilsObjectName,
                    VK_OBJECT_TYPE_PIPELINE, reinterpret_cast<u64>(native), name);
}

void VulkanDevice::SetResourceDebugName(IRHISampler* resource, const char* name) {
    if (!resource) return;
    auto* vkSamp = static_cast<VulkanSampler*>(resource);
    SetVkObjectName(m_Device, m_SetDebugUtilsObjectName,
                    VK_OBJECT_TYPE_SAMPLER, reinterpret_cast<u64>(vkSamp->GetHandle()), name);
}

void VulkanDevice::SetResourceDebugName(IRHIAccelerationStructure* resource, const char* name) {
    if (!resource) return;
    auto* vkAS = static_cast<VulkanAccelerationStructure*>(resource);
    SetVkObjectName(m_Device, m_SetDebugUtilsObjectName,
                    VK_OBJECT_TYPE_ACCELERATION_STRUCTURE_KHR,
                    reinterpret_cast<u64>(vkAS->GetHandle()), name);
}

} // namespace he::rhi
