#pragma once

// ============================================================
// vulkan/vulkan_ohos.h —— VK_OHOS_surface（stub，仅供 Windows 侧语法自检）
//
// 真实头文件由 HarmonyOS NDK 提供（<vulkan/vulkan_ohos.h>）。这里声明
// VulkanPlatform.cpp 用到的类型/常量/入口，保证在 Windows 上也能编译该分支。
// ============================================================

#include <vulkan/vulkan.h>

struct OHNativeWindow;

#ifndef VK_OHOS_SURFACE_EXTENSION_NAME
    #define VK_OHOS_SURFACE_EXTENSION_NAME "VK_OHOS_surface"
#endif

#ifndef VK_STRUCTURE_TYPE_SURFACE_CREATE_INFO_OHOS
    #define VK_STRUCTURE_TYPE_SURFACE_CREATE_INFO_OHOS static_cast<VkStructureType>(1000009000)
#endif

typedef struct VkSurfaceCreateInfoOHOS {
    VkStructureType     sType;
    const void*         pNext;
    uint32_t            flags;
    struct OHNativeWindow* window;
} VkSurfaceCreateInfoOHOS;

extern "C" VkResult vkCreateSurfaceOHOS(VkInstance instance,
                                        const VkSurfaceCreateInfoOHOS* createInfo,
                                        const VkAllocationCallbacks* allocator,
                                        VkSurfaceKHR* surface);
