#pragma once

// ============================================================
// WindowWin32.h —— Windows 窗口实现（GLFW）
//
// 只实现 IWindow；GLFW 只出现在本文件与 WindowWin32.cpp 里。
// 鸿蒙对应实现见 Platforms/OHOS 的 WindowOHOS.cpp（同一套 IWindow 接口）。
// ============================================================

#include "Platform/IWindow.h"

struct GLFWwindow;

namespace he {

class WindowWin32 final : public IWindow {
public:
    explicit WindowWin32(const WindowDesc& desc);
    ~WindowWin32() override;

    bool ShouldClose() const override;
    void RequestClose() override;
    void PollEvents() override;

    u32    GetWidth()  const override { return m_Width; }
    u32    GetHeight() const override { return m_Height; }
    float2 GetSize()   const override {
        return float2(static_cast<float>(m_Width), static_cast<float>(m_Height));
    }
    float  GetAspectRatio() const override {
        return m_Width ? static_cast<float>(m_Width) / static_cast<float>(m_Height) : 1.0f;
    }

    void* GetNativeWindow()  const override;   // HWND
    void* GetDisplayHandle() const override;   // HINSTANCE

    bool Recreate() override;
    void SetResizeCallback(ResizeCallback cb) override { m_OnResize = std::move(cb); }
    void SetCursorVisible(bool visible) override;
    void SetTitle(const char* title) override;

    /// 仅 Windows / 过渡期使用：ImGui 的 GLFW 后端与样例的 CameraInput 需要 GLFWwindow*
    GLFWwindow* GetGLFWWindow() const { return m_Handle; }

private:
    static void GlfwResizeCallback(GLFWwindow* window, int w, int h);

    GLFWwindow*    m_Handle = nullptr;
    u32            m_Width  = 0;
    u32            m_Height = 0;
    WindowDesc     m_Desc{};
    ResizeCallback m_OnResize;
};

/// 由 IWindow 取 GLFW 窗口指针（Windows 专用；非 GLFW 窗口返回 nullptr）。
/// 【过渡期】P0-3 引入 IInput 后，样例不再需要它；目前只给 ImGui 的 GLFW 后端用。
GLFWwindow* GetGlfwWindow(IWindow* window);

} // namespace he
