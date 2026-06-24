#include "Platform/Window.h"
#include "Core/Log.h"
#include "Core/Assert.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

namespace he {

Window::Window(const WindowDesc& desc) {
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

void Window::GlfwResizeCallback(GLFWwindow* window, int w, int h) {
    auto* self = static_cast<Window*>(glfwGetWindowUserPointer(window));
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
