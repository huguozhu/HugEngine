#pragma once

#include "Core/Platform.h"   // HE_PLATFORM_*：必须在使用 #if 之前可见

// ============================================================
// imgui_impl_mobile.h —— ImGui 的移动端输入后端（触摸）
//
// ImGui 的 Vulkan 渲染后端（imgui_impl_vulkan）两端通用；差的是**输入**：
//   Windows: imgui_impl_glfw（GLFW 回调）
//   鸿蒙:    本文件（读平台无关的 InputState：单指 = 左键按下 + 位置）
// 渲染路径不受影响。
// ============================================================

#if HE_PLATFORM_OHOS || HE_PLATFORM_ANDROID

namespace he {
class IWindow;
}

bool ImGui_ImplMobile_Init(he::IWindow* window);
void ImGui_ImplMobile_Shutdown();
void ImGui_ImplMobile_NewFrame(he::IWindow* window);

#endif // HE_PLATFORM_OHOS || HE_PLATFORM_ANDROID
