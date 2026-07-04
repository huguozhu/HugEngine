// Samples/Editor/Panels/ViewportPanel.h
#pragma once

// ============================================================
// ViewportPanel — 3D 场景视口
//
// 在 ImGui 窗口中渲染 3D 场景。
// 独立 Editor Camera（WASD + 鼠标右键旋转）。
// ============================================================

#include "Core/Types.h"
#include "Pipeline/CameraController.h"
#include "Math/Math.h"
#include "Panels/Gizmo.h"

struct GLFWwindow;

namespace he::rhi {
    class IRHICommandList;
}
namespace he::render {
    class ForwardPipeline;
}
namespace he {
    class World;
    class SceneGraph;
}
namespace he::editor {
    class EditorContext;
}

namespace he::editor {

class ViewportPanel {
public:
    /// @param window GLFW 窗口句柄（用于捕获输入）
    void Initialize(EditorContext* ctx, he::render::ForwardPipeline* pipeline,
                    GLFWwindow* window);

    /// 渲染视口（每帧调用，在 RenderPass 内部）
    void Render(he::rhi::IRHICommandList* cmdList);

    /// 游戏模式渲染
    void RenderGameView(he::rhi::IRHICommandList* cmdList);

    /// 渲染 Gizmo 叠加层（在 ImGui 帧内调用，场景渲染之后）
    void RenderGizmoOverlay();

    /// 更新视口尺寸
    void SetViewportSize(u32 width, u32 height) {
        m_CamCtrl.SetAspectRatio(static_cast<f32>(width), static_cast<f32>(height));
    }

    const he::render::CameraData& GetCamera() const { return m_CamCtrl.GetCamera(); }

    // 视口区域（由 EditorApp 在 ImGui 帧内设置）
    float2 m_VP_Pos  = float2(0, 0);
    float2 m_VP_Size = float2(1920, 1080);

private:
    void UpdateCamera(float deltaTime);

    EditorContext*              m_Ctx      = nullptr;
    he::render::ForwardPipeline* m_Pipeline = nullptr;
    GLFWwindow*                  m_Window   = nullptr;

    he::render::CameraController m_CamCtrl;
    Gizmo m_Gizmo;

    // 鼠标状态
    bool   m_RightMouseDown = false;
    double m_LastMouseX     = 0.0;
    double m_LastMouseY     = 0.0;
};

} // namespace he::editor
