// Samples/Editor/Panels/ViewportPanel.h
#pragma once

// ============================================================
// ViewportPanel — 3D 场景视口
//
// 在 ImGui 窗口中渲染 3D 场景。
// 独立 Editor Camera（WASD + 鼠标右键旋转）。
// ============================================================

#include "Core/Types.h"
#include "Render/Pipeline/Camera.h"
#include "Math/Math.h"

struct GLFWwindow;

namespace he::rhi {
    class IRHIDevice;
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

    /// 获取编辑器相机引用（供外部读取相机状态）
    const he::render::CameraData& GetCamera() const { return m_Camera; }

private:
    void UpdateCamera(float deltaTime);

    EditorContext*              m_Ctx      = nullptr;
    he::render::ForwardPipeline* m_Pipeline = nullptr;
    GLFWwindow*                  m_Window   = nullptr;

    he::render::CameraData m_Camera;

    // 鼠标状态
    bool   m_RightMouseDown = false;
    double m_LastMouseX     = 0.0;
    double m_LastMouseY     = 0.0;
    float  m_Yaw            = 0.0f;
    float  m_Pitch          = 0.0f;
    float  m_MoveSpeed      = 5.0f;
    float  m_LookSpeed      = 0.003f;
};

} // namespace he::editor
