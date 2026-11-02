#include "Platform/Window.h"
#include "Core/Log.h"
#include "Core/Assert.h"

#define GLFW_INCLUDE_NONE
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>  // glfwGetWin32Window

namespace he {

Window::Window(const WindowDesc& desc) : m_Desc(desc) {
    HE_CORE_INFO("Creating window: {} {}x{}", desc.title, desc.width, desc.height);

    if (!glfwInit()) {
        HE_CORE_CRITICAL("Failed to initialize GLFW");
        HE_ASSERT(false);
        return;
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API); // No OpenGL — we use Vulkan/D3D12
    glfwWindowHint(GLFW_RESIZABLE, desc.resizable ? GLFW_TRUE : GLFW_FALSE);

    m_Handle = glfwCreateWindow(
        static_cast<int>(desc.width),
        static_cast<int>(desc.height),
        desc.title.c_str(),
        nullptr, nullptr
    );

    if (!m_Handle) {
        HE_CORE_CRITICAL("Failed to create GLFW window");
        glfwTerminate();
        HE_ASSERT(false);
        return;
    }

    m_Width  = desc.width;
    m_Height = desc.height;

    // Register resize callback
    glfwSetWindowUserPointer(m_Handle, this);
    glfwSetFramebufferSizeCallback(m_Handle, GlfwResizeCallback);

    HE_CORE_INFO("Window created successfully");
}

Window::~Window() {
    if (m_Handle) {
        glfwDestroyWindow(m_Handle);
        m_Handle = nullptr;
    }
    glfwTerminate();
    HE_CORE_INFO("Window destroyed");
}

bool Window::ShouldClose() const {
    return glfwWindowShouldClose(m_Handle);
}

void Window::PollEvents() const {
    glfwPollEvents();
}

void Window::SwapBuffers() const {
    // No OpenGL swap — presentation handled by RHI
}

void* Window::GetNativeHandleRaw() const {
#if HE_PLATFORM_WINDOWS
    return glfwGetWin32Window(m_Handle);
#else
    return nullptr;
#endif
}

bool Window::Recreate() {
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
    m_Handle = glfwCreateWindow(static_cast<int>(w), static_cast<int>(h), m_Desc.title.c_str(), nullptr, nullptr);
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
                 static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(GetNativeHandleRaw())));
    return true;
}

void Window::GlfwResizeCallback(GLFWwindow* window, int w, int h) {    auto* self = static_cast<Window*>(glfwGetWindowUserPointer(window));
    if (self) {
        self->m_Width  = static_cast<u32>(w);
        self->m_Height = static_cast<u32>(h);
        HE_CORE_INFO("Window resized: {}x{}", w, h);
        if (self->m_OnResize) {
            self->m_OnResize(self->m_Width, self->m_Height);
        }
    }
}

} // namespace he
