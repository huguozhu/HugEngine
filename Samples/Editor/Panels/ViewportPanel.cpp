// Samples/Editor/Panels/ViewportPanel.cpp

#include "ViewportPanel.h"
#include "Editor/EditorContext.h"
#include "Pipeline/ForwardPipeline.h"
#include "Scene/World.h"
#include "Scene/SceneGraph.h"
#include "Scene/Transform.h"
#include "imgui.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

namespace he::editor {

void ViewportPanel::Initialize(EditorContext* ctx,
                                render::ForwardPipeline* pipeline,
                                GLFWwindow* window) {
    m_Ctx      = ctx;
    m_Pipeline = pipeline;
    m_Window   = window;

    // 初始化编辑器相机（编辑器使用 Ground 模式：WASD 水平面移动）
    m_CamCtrl.SetMoveMode(render::CameraController::MoveMode::Ground);
    m_CamCtrl.SetAspectRatio(16.0f, 9.0f);
}

void ViewportPanel::Render(rhi::IRHICommandList* cmdList) {
    if (!m_Ctx || !m_Pipeline) return;

    static f64 lastTime = glfwGetTime();
    f64 now = glfwGetTime();
    float dt = static_cast<float>(now - lastTime);
    lastTime = now;

    UpdateCamera(dt);

    // 绑定 PBR PSO（Editor 直接渲染到 backbuffer，不走 BeginHDRPass）
    cmdList->SetPipeline(m_Pipeline->GetPipelineState());
    m_Pipeline->RenderScene(cmdList,
        *m_Ctx->GetWorld(),
        *m_Ctx->GetSceneGraph(),
        m_CamCtrl.GetCamera());
}

void ViewportPanel::FocusOn(const float3& worldPos) {
    // 将相机放在目标后方 5 单位处，看向目标
    float3 camPos = worldPos + float3(0, 2, 5);
    m_CamCtrl.SetPosition(camPos);
    float3 forward = glm::normalize(worldPos - camPos);
    m_CamCtrl.SetOrientationFromForward(forward);
}

void ViewportPanel::RenderGameView(rhi::IRHICommandList* cmdList) {
    if (!m_Ctx || !m_Pipeline) return;
    m_Pipeline->RenderScene(cmdList,
        *m_Ctx->GetWorld(),
        *m_Ctx->GetSceneGraph(),
        m_CamCtrl.GetCamera());
}

void ViewportPanel::RenderGizmoOverlay() {
    if (!m_Ctx) return;
    auto& sel = m_Ctx->GetSelection();
    if (sel.empty()) return;
    he::Entity selEnt = sel[0];
    if (!selEnt.IsValid()) return;
    auto* world = m_Ctx->GetWorld();
    if (!world) return;

    auto* tf = world->GetComponent<TransformComponent>(selEnt);
    if (!tf) return;

    // 模式切换键
    auto& io = ImGui::GetIO();
    if (!io.WantCaptureKeyboard) {
        if (ImGui::IsKeyPressed(ImGuiKey_W)) m_Gizmo.mode = GizmoMode::Translate;
        if (ImGui::IsKeyPressed(ImGuiKey_E)) m_Gizmo.mode = GizmoMode::Rotate;
        if (ImGui::IsKeyPressed(ImGuiKey_R)) m_Gizmo.mode = GizmoMode::Scale;
        if (ImGui::IsKeyPressed(ImGuiKey_X)) m_Gizmo.space = (m_Gizmo.space == GizmoSpace::Local)
            ? GizmoSpace::World : GizmoSpace::Local;
    }

    // 渲染 gizmo
    float4x4 vp = m_CamCtrl.GetCamera().GetViewProjMatrix();
    float3 pos = tf->position;
    quat rot = tf->rotation;
    float3 scl = tf->scale;

    bool dragging = m_Gizmo.Render(m_CamCtrl.GetCamera(), pos, rot, scl, vp, m_VP_Pos, m_VP_Size);

    // 应用变换 + 标记场景图为脏
    if (dragging) {
        tf->position = pos;
        tf->rotation = rot;
        tf->scale    = scl;
        m_Ctx->GetSceneGraph()->MarkDirty(selEnt);
    }
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
