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
// Vulkan 特定资源（RenderPass / DescriptorPool）通过 RHI 接口创建，
// 不直接调用 Vulkan API。
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
    void CreateImGuiResources(rhi::IRHIDevice* device, rhi::IRHISwapChain* swapchain);
    void DestroyImGuiResources(rhi::IRHIDevice* device);

    /// 【A2】Vulkan 叠加记录器：由设备在"回放侧/直接录制"时调用，
    /// 把 ImGui 的 draw data 编码进给定原生命令缓冲（VkCommandBuffer）。
    static void RecordVulkanOverlay(void* nativeCmdBuffer);

    GLFWwindow*   m_Window     = nullptr;
    ImGuiContext* m_Context    = nullptr;
    bool          m_Initialized = false;
    bool          m_IsD3D12     = false;     // true = 用 imgui_impl_dx12 渲染，否则 imgui_impl_vulkan
    bool          m_IsD3D11     = false;     // true = 用 imgui_impl_dx11 渲染
    rhi::IRHIDevice* m_Device  = nullptr;    // 持有设备指针用于资源销毁

    // 后端资源（void* 存储，由 RHI 管理生命周期）
    void* m_DescPool   = nullptr;  // VkDescriptorPool / ID3D12DescriptorHeap
    void* m_RenderPass = nullptr;  // VkRenderPass
};

} // namespace he::editor
