#pragma once

// ============================================================
// VulkanPlatform.h —— 唯一决定"用哪种 WSI 表面"的地方
//
// 【为什么单独抽一个头】
// 之前 `#define VK_USE_PLATFORM_WIN32_KHR` 散落在 20 个 Vulkan 源文件里，各自包在
// `#include <vulkan/vulkan.h>` 之前。要支持鸿蒙（VK_OHOS_surface）就必须把这些散点
// 收敛成一处：所有文件只 include 本头，平台差异只在这里出现。
//
// 平台差异只有两件事：
//   1) 实例扩展名（Win32: VK_KHR_win32_surface / OHOS: VK_OHOS_surface）
//   2) 由原生窗口句柄创建 VkSurfaceKHR 的函数
// ============================================================

#include "Core/Platform.h"

#include <vector>

#if HE_PLATFORM_WINDOWS
    #ifndef VK_USE_PLATFORM_WIN32_KHR
        #define VK_USE_PLATFORM_WIN32_KHR
    #endif
#endif

#include <vulkan/vulkan.h>

#if HE_PLATFORM_OHOS
    // 鸿蒙 NDK 的 VK_OHOS_surface：提供 vkCreateSurfaceOHOS / VK_OHOS_SURFACE_EXTENSION_NAME
    #include <vulkan/vulkan_ohos.h>
    #ifndef HE_VULKAN_USE_OHOS_SURFACE
        #define HE_VULKAN_USE_OHOS_SURFACE 1
    #endif
#endif

namespace he::rhi::vkplat {

// 平台是否具备可用的 WSI 表面（两端都应有；留作编译期断言用）
#if HE_PLATFORM_WINDOWS || HE_PLATFORM_OHOS || HE_PLATFORM_ANDROID
constexpr bool kHasPlatformSurface = true;
#else
constexpr bool kHasPlatformSurface = false;
#endif

/// 追加平台相关的**实例**扩展名（调用方已加入 VK_KHR_SURFACE_EXTENSION_NAME）
void AppendSurfaceInstanceExtensions(std::vector<const char*>& extensions);

/// 由原生窗口句柄创建 VkSurfaceKHR。
/// Windows：nativeWindow 是 HWND；鸿蒙：nativeWindow 是 OHNativeWindow*。
/// 失败返回 false（内部已打印错误日志）。
bool CreateSurface(VkInstance instance, void* nativeWindow, VkSurfaceKHR* outSurface);

} // namespace he::rhi::vkplat
