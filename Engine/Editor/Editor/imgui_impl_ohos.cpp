// ============================================================

#include "Core/Platform.h"   // HE_PLATFORM_*：必须在使用 #if 之前可见
// imgui_impl_ohos.cpp —— ImGui 的鸿蒙输入后端

//

// 输入来源是平台无关的 he::PollInput(window)（InputOHOS 把触摸事件换算成

// mousePos / mouseDown / mouseDelta）。映射规则：

//   · 单指按下/拖动 → 鼠标左键按下 + 位置移动（ImGui 的点击/拖拽控件都能用）

//   · 触摸抬起        → 左键抬起

// 帧时间用 he::Time::Now 计算（鸿蒙后端没有 GLFW 的 NewFrame 来填 DeltaTime）。

// ============================================================


#include "imgui_impl_ohos.h"

#if HE_PLATFORM_OHOS

#include "imgui.h"

#include "Core/Log.h"
#include "Platform/IInput.h"
#include "Platform/IWindow.h"
#include "Platform/Time.h"

namespace {

he::IWindow* g_Window = nullptr;
he::f64      g_LastTime = 0.0;

} // namespace

bool ImGui_ImplOhos_Init(he::IWindow* window) {
    g_Window = window;
    g_LastTime = he::Time::Now();
    if (window) {
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2(static_cast<float>(window->GetWidth()),
                                static_cast<float>(window->GetHeight()));
        io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
    }
    HE_CORE_INFO("[OHOS] ImGui 输入后端已初始化（触摸 → 鼠标）");
    return true;
}

void ImGui_ImplOhos_Shutdown() {
    g_Window = nullptr;
}

void ImGui_ImplOhos_NewFrame(he::IWindow* window) {
    if (!window) return;
    ImGuiIO& io = ImGui::GetIO();

    io.DisplaySize = ImVec2(static_cast<float>(window->GetWidth()),
                            static_cast<float>(window->GetHeight()));
    io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);

    // 帧时间（ImGui 的动画/输入去抖依赖它）
    const he::f64 now = he::Time::Now();
    const he::f64 dt  = now - g_LastTime;
    g_LastTime = now;
    io.DeltaTime = static_cast<float>(dt > 0.0 ? dt : (1.0 / 60.0));

    // 输入快照（与相机控制共用同一份，不会有"UI 与相机抢事件"的问题）
    const he::InputState& in = he::PollInput(window);
    io.AddMousePosEvent(in.mousePos.x, in.mousePos.y);

    // 单指触摸 → 鼠标左键；桌面语义的右键（相机拖拽）不喂给 ImGui，
    // 否则拖相机时 UI 会同时响应。
    const bool leftDown = (in.touchCount > 0) || in.mouseDown[0];
    if (leftDown != io.MouseDown[0]) {
        io.AddMouseButtonEvent(0, leftDown);
    }
}

#endif // HE_PLATFORM_OHOS
