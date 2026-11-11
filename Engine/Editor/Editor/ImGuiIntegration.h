#pragma once

#include "Core/Types.h"

struct ImGuiContext;

namespace he {
    class IWindow;
}
namespace he::rhi {
    class IRHIDevice;
    class IRHICommandList;
    class IRHISwapChain;
}

namespace he::editor {

// ============================================================
// ImGuiIntegration — 编辑器 ImGui 集成
//
// 管理 ImGui 生命周期、GLFW / 移动端输入、渲染后端。
// ImGui 的渲染后端按图形 API 分文件（imgui_impl_vulkan / dx12 / dx11），
// 这里用 `RenderBackend` 枚举把"选哪个后端"收敛成一次解析：
// 后续所有分支只看这个枚举，不再各自判断 RHI 的 Backend。
// ============================================================
class ImGuiIntegration {
public:
    ImGuiIntegration();
    ~ImGuiIntegration();

    void Initialize(IWindow* window, rhi::IRHIDevice* device,
                    rhi::IRHISwapChain* swapchain);
    void Shutdown();

    void BeginFrame();
    void EndFrame(rhi::IRHICommandList* cmd);

    bool IsInitialized() const { return m_Initialized; }

private:
    /// ImGui 实际使用的渲染后端。
    /// 【为什么不直接用 rhi::Backend】头文件不 include RHI；而且这里表达的是
    /// "**编译期存在**且**运行期可用**的 ImGui 后端"——没编进来的后端（HE_RHI_* 未定义）
    /// 会在 ResolveRenderBackend 里落成 None，与"设备不支持"是同一种处理。
    enum class RenderBackend { None, Vulkan, D3D12, D3D11 };

    /// 把 RHI 后端解析成 ImGui 后端；None 表示初始化时直接跳过 UI。
    static RenderBackend ResolveRenderBackend(const rhi::IRHIDevice* device);

    void CreateImGuiResources(rhi::IRHIDevice* device, rhi::IRHISwapChain* swapchain);
    void DestroyImGuiResources(rhi::IRHIDevice* device);

    /// 【A2】Vulkan 叠加记录器：由设备在"回放侧/直接录制"时调用，
    /// 把 ImGui 的 draw data 编码进给定原生命令缓冲（VkCommandBuffer）。
    static void RecordVulkanOverlay(void* nativeCmdBuffer);

    IWindow*      m_Window     = nullptr;
    ImGuiContext* m_Context    = nullptr;
    bool          m_Initialized = false;
    RenderBackend m_RenderBackend = RenderBackend::None;   // 初始化时解析一次，之后只读
    rhi::IRHIDevice* m_Device  = nullptr;    // 持有设备指针用于资源销毁

    // 后端资源（void* 存储，由 RHI 管理生命周期）
    void* m_DescPool   = nullptr;  // VkDescriptorPool / ID3D12DescriptorHeap
    void* m_RenderPass = nullptr;  // VkRenderPass
};

} // namespace he::editor
