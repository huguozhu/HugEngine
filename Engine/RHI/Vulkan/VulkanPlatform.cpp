// ============================================================
// VulkanPlatform.cpp —— 平台 WSI 表面创建（Windows / 鸿蒙）
//
// 这个文件是引擎里**唯一**直接调用平台 WSI 函数的地方：
//   Windows: vkCreateWin32SurfaceKHR
//   鸿蒙:    vkCreateSurfaceOHOS（VK_OHOS_surface，窗口句柄为 OHNativeWindow*）
// ============================================================

#include "VulkanPlatform.h"

#include "Core/Log.h"

#if HE_PLATFORM_WINDOWS
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#elif HE_PLATFORM_OHOS
    // 鸿蒙的原生窗口类型（XComponent 的 surface 由 OHNativeWindow 表示）
    #include <native_window/external_window.h>
#endif

namespace he::rhi::vkplat {

void AppendSurfaceInstanceExtensions(std::vector<const char*>& extensions) {
#if HE_PLATFORM_WINDOWS
    extensions.push_back(VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
#elif HE_PLATFORM_OHOS
    extensions.push_back(VK_OHOS_SURFACE_EXTENSION_NAME);
#else
    (void)extensions;
    HE_CORE_ERROR("当前平台没有可用的 Vulkan WSI 表面实现");
#endif
}

bool CreateSurface(VkInstance instance, void* nativeWindow, VkSurfaceKHR* outSurface) {
    if (!instance || !nativeWindow || !outSurface) return false;
    *outSurface = VK_NULL_HANDLE;

#if HE_PLATFORM_WINDOWS
    VkWin32SurfaceCreateInfoKHR info{};
    info.sType     = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    info.hinstance = GetModuleHandleW(nullptr);
    info.hwnd      = static_cast<HWND>(nativeWindow);

    const VkResult r = vkCreateWin32SurfaceKHR(instance, &info, nullptr, outSurface);
    if (r != VK_SUCCESS) {
        HE_CORE_ERROR("vkCreateWin32SurfaceKHR 失败: VkResult={}", static_cast<int>(r));
        return false;
    }
    HE_CORE_INFO("Vulkan 表面已创建（Win32）");
    return true;

#elif HE_PLATFORM_OHOS
    VkSurfaceCreateInfoOHOS info{};
    info.sType  = VK_STRUCTURE_TYPE_SURFACE_CREATE_INFO_OHOS;
    info.window = static_cast<OHNativeWindow*>(nativeWindow);

    const VkResult r = vkCreateSurfaceOHOS(instance, &info, nullptr, outSurface);
    if (r != VK_SUCCESS) {
        HE_CORE_ERROR("vkCreateSurfaceOHOS 失败: VkResult={}", static_cast<int>(r));
        return false;
    }
    HE_CORE_INFO("Vulkan 表面已创建（OHOS）");
    return true;

#else
    (void)nativeWindow;
    HE_CORE_ERROR("当前平台没有可用的 Vulkan WSI 表面实现");
    return false;
#endif
}

} // namespace he::rhi::vkplat
