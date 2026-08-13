// VulkanDevice_GPL.cpp — Graphics Pipeline Library 能力检测
// 照 VulkanDevice_MeshShader.cpp 的 QueryMeshCapabilities 范式，
// 查询 VK_EXT_graphics_pipeline_library 的 feature 与 properties。

#include "RHI/RHI.h"
#include "Core/Log.h"

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include "VulkanDevice.h"

#include <cstring>

namespace he::rhi {

void VulkanDevice::QueryGPLCapabilities() {
    m_SupportsGPL = false;
    m_SupportsGPLFastLinking = false;

    // 1. 检查设备扩展是否可用
    u32 extCount = 0;
    vkEnumerateDeviceExtensionProperties(m_Physical, nullptr, &extCount, nullptr);
    std::vector<VkExtensionProperties> extensions(extCount);
    vkEnumerateDeviceExtensionProperties(m_Physical, nullptr, &extCount, extensions.data());

    bool hasGPL = false;
    for (auto& ext : extensions) {
        if (strcmp(ext.extensionName, VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME) == 0) {
            hasGPL = true;
            break;
        }
    }
    if (!hasGPL) {
        HE_CORE_INFO("Graphics Pipeline Library: 不支持（缺少 VK_EXT_graphics_pipeline_library）");
        return;
    }

    // 2. 查询 feature（graphicsPipelineLibrary 在 Features 结构）
    VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT gplFeatures{};
    gplFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT;
    VkPhysicalDeviceFeatures2 feat2{};
    feat2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    feat2.pNext = &gplFeatures;
    vkGetPhysicalDeviceFeatures2(m_Physical, &feat2);
    if (!gplFeatures.graphicsPipelineLibrary) {
        HE_CORE_INFO("Graphics Pipeline Library: feature 不可用");
        return;
    }
    m_SupportsGPL = true;

    // 3. 查询 properties（graphicsPipelineLibraryFastLinking 在 Properties 结构）
    VkPhysicalDeviceGraphicsPipelineLibraryPropertiesEXT gplProps{};
    gplProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_PROPERTIES_EXT;
    VkPhysicalDeviceProperties2 props2{};
    props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    props2.pNext = &gplProps;
    vkGetPhysicalDeviceProperties2(m_Physical, &props2);
    m_SupportsGPLFastLinking = gplProps.graphicsPipelineLibraryFastLinking;

    HE_CORE_INFO("Graphics Pipeline Library: 硬件支持已检测 (fastLinking={})",
                 m_SupportsGPLFastLinking ? 1 : 0);
}

} // namespace he::rhi
