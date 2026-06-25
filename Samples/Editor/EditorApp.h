// Samples/Editor/EditorApp.h
#pragma once

// ============================================================
// EditorApp — HugEditor 应用主类
//
// 管理编辑器生命周期：初始化引擎 → 创建面板 → 主循环 → 清理
// ============================================================

#include "Core/Core.h"
#include <memory>

struct GLFWwindow;

namespace he {
    class World;
    class SceneGraph;
    class CommandHistory;
    class Engine;
namespace rhi {
    class IRHIDevice;
    class IRHISwapChain;
    class IRHICommandList;
}
namespace render {
    class ForwardPipeline;
}
namespace editor {
    class ImGuiIntegration;
    class EditorContext;
}
}

// 前向声明面板类
namespace he::editor {
    class ViewportPanel;
    class OutlinerPanel;
    class DetailsPanel;
    class ContentBrowserPanel;
}

class EditorApp {
public:
    EditorApp();
    ~EditorApp();

    int Run();

private:
    void InitEngine();      // GLFW 窗口 + RHI 设备 + SwapChain
    void InitScene();       // 默认场景（地面 + 光源）
    void InitEditor();      // ImGui + EditorContext + 面板
    void InitPipeline();    // ForwardPipeline
    void MainLoop();        // 帧循环
    void Shutdown();

    // --- 底层 ---
    // 声明顺序即析构顺序（逆序）：Engine 最后销毁，Logger 在所有子系统之后释放
    std::unique_ptr<he::Engine>         m_Engine;
    GLFWwindow*                         m_Window    = nullptr;
    std::unique_ptr<he::rhi::IRHIDevice>       m_Device;
    std::unique_ptr<he::rhi::IRHISwapChain>    m_SwapChain;
    std::unique_ptr<he::rhi::IRHICommandList>  m_CmdList;

    // --- 引擎系统 ---
    std::unique_ptr<he::World>                  m_World;
    std::unique_ptr<he::SceneGraph>             m_SceneGraph;
    std::unique_ptr<he::render::ForwardPipeline> m_Pipeline;
    std::unique_ptr<he::CommandHistory>          m_CmdHistory;
    std::unique_ptr<he::editor::ImGuiIntegration> m_ImGui;
    std::unique_ptr<he::editor::EditorContext>    m_EditorCtx;

    // --- 面板 ---
    std::unique_ptr<he::editor::ViewportPanel> m_Viewport;
    std::unique_ptr<he::editor::OutlinerPanel> m_Outliner;
    std::unique_ptr<he::editor::DetailsPanel>  m_Details;
    std::unique_ptr<he::editor::ContentBrowserPanel> m_ContentBrowser;

    he::f64  m_LastTime  = 0.0;
};
