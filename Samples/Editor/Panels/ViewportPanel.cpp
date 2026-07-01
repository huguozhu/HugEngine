// Samples/Editor/Panels/ViewportPanel.cpp

#include "ViewportPanel.h"
#include "Editor/EditorContext.h"
#include "Pipeline/ForwardPipeline.h"
#include "Scene/World.h"
#include "Scene/SceneGraph.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

namespace he::editor {

void ViewportPanel::Initialize(EditorContext* ctx,
                                render::ForwardPipeline* pipeline,
                                GLFWwindow* window) {
    m_Ctx      = ctx;
    m_Pipeline = pipeline;
    m_Window   = window;

    // 初始化编辑器相机（CameraController 默认位置/朝向与旧代码一致）
    m_CamCtrl.SetAspectRatio(16.0f, 9.0f);
}

void ViewportPanel::Render(rhi::IRHICommandList* cmdList) {
    if (!m_Ctx || !m_Pipeline) return;

    // 计算帧时间
    static f64 lastTime = glfwGetTime();
    f64 now = glfwGetTime();
    float dt = static_cast<float>(now - lastTime);
    lastTime = now;

    // 更新编辑器相机
    UpdateCamera(dt);

    // 渲染 3D 场景到当前 BackBuffer
    m_Pipeline->RenderScene(cmdList,
        *m_Ctx->GetWorld(),
        *m_Ctx->GetSceneGraph(),
        m_CamCtrl.GetCamera());
}

void ViewportPanel::RenderGameView(rhi::IRHICommandList* cmdList) {
    if (!m_Ctx || !m_Pipeline) return;
    m_Pipeline->RenderScene(cmdList,
        *m_Ctx->GetWorld(),
        *m_Ctx->GetSceneGraph(),
        m_CamCtrl.GetCamera());
}

void ViewportPanel::UpdateCamera(float deltaTime) {
    if (!m_Window) return;

    // --- 鼠标旋转（右键拖拽）---
    bool mouseDown = glfwGetMouseButton(m_Window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;

    if (mouseDown && !m_RightMouseDown) {
        m_RightMouseDown = true;
        glfwGetCursorPos(m_Window, &m_LastMouseX, &m_LastMouseY);
        glfwSetInputMode(m_Window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    } else if (!mouseDown && m_RightMouseDown) {
        m_RightMouseDown = false;
        glfwSetInputMode(m_Window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
    } else if (mouseDown && m_RightMouseDown) {
        double cx, cy;
        glfwGetCursorPos(m_Window, &cx, &cy);
        float dx = static_cast<float>(cx - m_LastMouseX);
        float dy = static_cast<float>(cy - m_LastMouseY);
        m_LastMouseX = cx;
        m_LastMouseY = cy;

        m_CamCtrl.Rotate(dx * 0.003f, -dy * 0.003f);
    }

    // --- 键盘移动 ---
    render::CameraController::MoveInput moveIn;
    moveIn.forward  = glfwGetKey(m_Window, GLFW_KEY_W) == GLFW_PRESS;
    moveIn.backward = glfwGetKey(m_Window, GLFW_KEY_S) == GLFW_PRESS;
    moveIn.left     = glfwGetKey(m_Window, GLFW_KEY_A) == GLFW_PRESS;
    moveIn.right    = glfwGetKey(m_Window, GLFW_KEY_D) == GLFW_PRESS;
    moveIn.up       = glfwGetKey(m_Window, GLFW_KEY_E) == GLFW_PRESS;
    moveIn.down     = glfwGetKey(m_Window, GLFW_KEY_Q) == GLFW_PRESS;
    moveIn.sprint   = glfwGetKey(m_Window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS;

    m_CamCtrl.Update(deltaTime, moveIn);
}

} // namespace he::editor
