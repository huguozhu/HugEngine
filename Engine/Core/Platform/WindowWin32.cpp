// ============================================================
// WindowWin32.cpp —— Windows 窗口实现（GLFW）
// ============================================================

#include "Platform/WindowWin32.h"

#include "Core/Log.h"
#include "Core/Assert.h"

#define GLFW_INCLUDE_NONE
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>  // glfwGetWin32Window

#if HE_PLATFORM_WINDOWS
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#endif

namespace he {

WindowWin32::WindowWin32(const WindowDesc& desc) : m_Desc(desc) {
    HE_CORE_INFO("Creating window: {} {}x{}", desc.title, desc.width, desc.height);

    if (!glfwInit()) {
        HE_CORE_CRITICAL("Failed to initialize GLFW");
        HE_ASSERT(false);
        return;
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);   // 不用 OpenGL：渲染由 RHI 负责
    glfwWindowHint(GLFW_RESIZABLE, desc.resizable ? GLFW_TRUE : GLFW_FALSE);

    m_Handle = glfwCreateWindow(static_cast<int>(desc.width), static_cast<int>(desc.height),
                                desc.title.c_str(), nullptr, nullptr);
    if (!m_Handle) {
        HE_CORE_CRITICAL("Failed to create GLFW window");
        glfwTerminate();
        HE_ASSERT(false);
        return;
    }

    m_Width  = desc.width;
    m_Height = desc.height;

    glfwSetWindowUserPointer(m_Handle, this);
    glfwSetFramebufferSizeCallback(m_Handle, GlfwResizeCallback);

    HE_CORE_INFO("Window created successfully");
}

WindowWin32::~WindowWin32() {
    if (m_Handle) {
        glfwDestroyWindow(m_Handle);
        m_Handle = nullptr;
    }
    glfwTerminate();
    HE_CORE_INFO("Window destroyed");
}

bool WindowWin32::ShouldClose() const {
    return glfwWindowShouldClose(m_Handle) != 0;
}

void WindowWin32::RequestClose() {
    if (m_Handle) glfwSetWindowShouldClose(m_Handle, GLFW_TRUE);
}

void WindowWin32::PollEvents() {
    glfwPollEvents();
}

void* WindowWin32::GetNativeWindow() const {
#if HE_PLATFORM_WINDOWS
    return glfwGetWin32Window(m_Handle);
#else
    return nullptr;
#endif
}

void* WindowWin32::GetDisplayHandle() const {
#if HE_PLATFORM_WINDOWS
    return static_cast<void*>(GetModuleHandleW(nullptr));
#else
    return nullptr;
#endif
}

void WindowWin32::SetTitle(const char* title) {
    if (m_Handle && title) glfwSetWindowTitle(m_Handle, title);
}

void WindowWin32::SetCursorVisible(bool visible) {
    if (!m_Handle) return;
    glfwSetInputMode(m_Handle, GLFW_CURSOR, visible ? GLFW_CURSOR_NORMAL : GLFW_CURSOR_HIDDEN);
}

bool WindowWin32::Recreate() {
    if (!m_Handle) return false;

    // 记住当前位置/尺寸，重建后尽量保持原地（用户看到的只是闪一下）
    int x = 0, y = 0;
    glfwGetWindowPos(m_Handle, &x, &y);
    const u32 w = m_Width  ? m_Width  : m_Desc.width;
    const u32 h = m_Height ? m_Height : m_Desc.height;

    glfwDestroyWindow(m_Handle);
    m_Handle = nullptr;

    // 与构造函数相同的 hint（GLFW 已初始化，不要再 glfwInit）
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, m_Desc.resizable ? GLFW_TRUE : GLFW_FALSE);
    m_Handle = glfwCreateWindow(static_cast<int>(w), static_cast<int>(h), m_Desc.title.c_str(),
                                nullptr, nullptr);
    if (!m_Handle) {
        HE_CORE_CRITICAL("重建窗口失败");
        HE_ASSERT(false);
        return false;
    }
    glfwSetWindowPos(m_Handle, x, y);
    m_Width  = w;
    m_Height = h;
    // 重新挂上回调与 user pointer（回调本身存在本对象里，重建后继续有效）
    glfwSetWindowUserPointer(m_Handle, this);
    glfwSetFramebufferSizeCallback(m_Handle, GlfwResizeCallback);
    HE_CORE_INFO("窗口已重建: {}x{}（hwnd={}）", m_Width, m_Height,
                 static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(GetNativeWindow())));
    return true;
}

void WindowWin32::GlfwResizeCallback(GLFWwindow* window, int w, int h) {
    auto* self = static_cast<WindowWin32*>(glfwGetWindowUserPointer(window));
    if (!self) return;
    self->m_Width  = static_cast<u32>(w);
    self->m_Height = static_cast<u32>(h);
    HE_CORE_INFO("Window resized: {}x{}", w, h);
    if (self->m_OnResize) {
        self->m_OnResize(self->m_Width, self->m_Height);
    }
}

GLFWwindow* GetGlfwWindow(IWindow* window) {
    if (auto* win32 = dynamic_cast<WindowWin32*>(window)) return win32->GetGLFWWindow();
    return nullptr;
}

std::unique_ptr<IWindow> CreatePlatformWindow(const WindowDesc& desc) {
    return std::make_unique<WindowWin32>(desc);
}

} // namespace he
