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

    // Callbacks
    using ResizeCallback = std::function<void(u32 width, u32 height)>;
    void SetResizeCallback(ResizeCallback cb) { m_OnResize = std::move(cb); }

private:
    GLFWwindow*     m_Handle = nullptr;
    u32             m_Width  = 0;
    u32             m_Height = 0;
    ResizeCallback  m_OnResize;

    static void GlfwResizeCallback(GLFWwindow* window, int w, int h);
};

} // namespace he
