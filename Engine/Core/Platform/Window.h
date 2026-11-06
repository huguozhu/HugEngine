#pragma once

#include "Core/Types.h"
#include "Math/Math.h"

#include <functional>

// ============================================================
// Window abstraction
// ============================================================

struct GLFWwindow; // Forward declaration

namespace he {

struct WindowDesc {
    String  title   = "HugEngine";
    u32     width   = kDefaultWindowWidth;
    u32     height  = kDefaultWindowHeight;
    bool    resizable = true;
    bool    vsync    = true;
};

class Window {
    HE_DECLARE_NON_COPYABLE(Window);
    HE_DECLARE_NON_MOVABLE(Window);

public:
    Window(const WindowDesc& desc);
    ~Window();

    bool ShouldClose() const;
    void PollEvents() const;
    void SwapBuffers() const;

    // Dimensions
    u32  GetWidth()  const { return m_Width; }
    u32  GetHeight() const { return m_Height; }
    float2 GetSize() const { return float2(static_cast<float>(m_Width), static_cast<float>(m_Height)); }
    float GetAspectRatio() const { return static_cast<float>(m_Width) / static_cast<float>(m_Height); }

    // Native handle
    GLFWwindow* GetNativeHandle() const { return m_Handle; }
    void* GetNativeHandleRaw() const; // HWND on Windows

    // 重建原生窗口（同位置、同尺寸、同标题），返回 false 表示重建失败。
    //
    // 【为什么需要】Windows 上：先在某个 HWND 上创建过 DXGI 翻转模型交换链（D3D11/D3D12），
    // 之后再让 Vulkan 交换链往**同一个** HWND 呈现时，窗口画面会一直停在 D3D 的最后一帧
    // （vkQueuePresentKHR 返回成功、渲染循环照常推进，但桌面合成不再更新；隐藏/显示、缩放、
    // DWM cloak 都无效）。换一个没有 DXGI 历史的全新 HWND 可以彻底避开这段残留状态。
    // 只在切换渲染后端时使用，代价是窗口重建一次（视觉上闪一下）。
    bool Recreate();

    // Callbacks
    using ResizeCallback = std::function<void(u32 width, u32 height)>;
    void SetResizeCallback(ResizeCallback cb) { m_OnResize = std::move(cb); }

private:
    GLFWwindow*     m_Handle = nullptr;
    u32             m_Width  = 0;
    u32             m_Height = 0;
    WindowDesc      m_Desc{};            // 重建窗口时要用回原来的标题/尺寸/可缩放设置
    ResizeCallback  m_OnResize;

    static void GlfwResizeCallback(GLFWwindow* window, int w, int h);
};

} // namespace he
