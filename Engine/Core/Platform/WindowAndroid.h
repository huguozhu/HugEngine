#pragma once

#include "Core/Platform.h"   // HE_PLATFORM_*：必须在使用 #if 之前可见

// ============================================================
// WindowAndroid.h —— Android 原生窗口挂接（SurfaceView/ANativeWindow）
//
// 与鸿蒙的 WindowOHOS 完全同构：
//   · Java 侧 SurfaceHolder.Callback → JNI 调 AttachNativeWindow(ANativeWindow*, w, h)
//   · 旋转/分屏 → 同一个函数（window 传 nullptr 表示只更新尺寸）
//   · surfaceDestroyed → DetachNativeWindow()
// 引擎通过 IWindow 读取尺寸与原生句柄，Vulkan 用它建 VkSurfaceKHR。
// ============================================================

#include "Core/Types.h"

#if HE_PLATFORM_ANDROID

#include <android/native_window.h>

namespace he {

/// surfaceCreated / surfaceChanged 时调用（窗口句柄 + 像素尺寸）
void AttachNativeWindow(ANativeWindow* window, u32 width, u32 height);

/// surfaceDestroyed 时调用：置空窗口句柄，渲染循环会自行暂停
void DetachNativeWindow();

} // namespace he

#endif // HE_PLATFORM_ANDROID
