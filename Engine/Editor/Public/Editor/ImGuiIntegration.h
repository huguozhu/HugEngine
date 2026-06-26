#pragma once

#include "Core/Types.h"

struct GLFWwindow;
struct ImGuiContext;

namespace he::rhi {
    class IRHIDevice;
    class IRHICommandList;
    class IRHISwapChain;
}

namespace he::editor {

// ============================================================
// ImGuiIntegration — 编辑器 ImGui 集成
//
// 管理 ImGui 生命周期、GLFW 输入、Vulkan 渲染。
// 用法:
//   ImGuiIntegration imgui;
//   imgui.Initialize(window, device, swapchain);
//   while (...) {
//       imgui.BeginFrame();
//       ImGui::ShowDemoWindow();
//       imgui.EndFrame(cmdList);
//   }
// ============================================================
class ImGuiIntegration {
public:
    ImGuiIntegration();
    ~ImGuiIntegration();

    void Initialize(GLFWwindow* window, rhi::IRHIDevice* device,
                    rhi::IRHISwapChain* swapchain);
    void Shutdown();

    void BeginFrame();
    void EndFrame(rhi::IRHICommandList* cmd);

    bool IsInitialized() const { return m_Initialized; }

private:
    void CreateFontTexture();
    void CreateVulkanResources(rhi::IRHIDevice* device, rhi::IRHISwapChain* swapchain);
    void DestroyVulkanResources();

    GLFWwindow*   m_Window     = nullptr;
    ImGuiContext* m_Context    = nullptr;
    bool          m_Initialized = false;

    // Vulkan 资源
    void* m_DescPool      = nullptr;  // VkDescriptorPool
    void* m_RenderPass    = nullptr;  // VkRenderPass（ImGui 后端使用，不可提前销毁）
    void* m_FontTexture   = nullptr;  // VkImage
    void* m_FontTexView   = nullptr;  // VkImageView
    void* m_FontTexMem    = nullptr;  // VkDeviceMemory
    void* m_FontSampler   = nullptr;  // VkSampler
    u64   m_FontDescSet   = 0;        // VkDescriptorSet (64-bit handle)
};

} // namespace he::editor
