// Samples/Editor/Panels/ViewportPanel.cpp

#include "ViewportPanel.h"
#include "Editor/EditorContext.h"
#include "Render/Pipeline/ForwardPipeline.h"
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

    // ��ʼ���༭�����
    m_Camera.position = float3(0.0f, 2.0f, 8.0f);
    m_Camera.forward  = float3(0.0f, -0.2f, -1.0f);
    m_Camera.up       = float3(0.0f, 1.0f, 0.0f);
    m_Camera.SetAspectRatio(16.0f, 9.0f);

    // �ӳ�ʼ������ yaw/pitch
    m_Yaw   = std::atan2(m_Camera.forward.x, -m_Camera.forward.z);
    m_Pitch = std::asin(m_Camera.forward.y);
}

void ViewportPanel::Render(rhi::IRHICommandList* cmdList) {
    if (!m_Ctx || !m_Pipeline) return;

    // ����֡ʱ��
    static f64 lastTime = glfwGetTime();
    f64 now = glfwGetTime();
    float dt = static_cast<float>(now - lastTime);
    lastTime = now;


    // ���±༭�����
    UpdateCamera(dt);

    // ��Ⱦ�������ӿ�����
    m_Pipeline->RenderScene(cmdList,
        *m_Ctx->GetWorld(),
        *m_Ctx->GetSceneGraph(),
        m_Camera);

    // MVP �׶Σ�ֱ��ʹ�� ForwardPipeline ��Ⱦ�� BackBuffer
    // Phase 3-2: ��Ϊ��Ⱦ�� off-screen texture �� ImGui::Image
}

void ViewportPanel::UpdateCamera(float deltaTime) {
    if (!m_Window) return;

    // --- �����飺�����ӿ����� hovered/focused ʱ�Ų������� ---
    // MVP �򻯣�ʼ�ղ��񣨺�����Ϊ�������У�

    // --- �����ת���Ҽ���ק��---
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

        m_Yaw   += dx * m_LookSpeed;
        m_Pitch -= dy * m_LookSpeed;
        m_Pitch  = glm::clamp(m_Pitch, -1.5f, 1.5f);
    }

    // --- WASD �ƶ� ---
    float speed = m_MoveSpeed * deltaTime;
    if (glfwGetKey(m_Window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS)
        speed *= 3.0f;

    float3 right = glm::normalize(glm::cross(m_Camera.forward, m_Camera.up));
    float3 move(0.0f);

    if (glfwGetKey(m_Window, GLFW_KEY_W) == GLFW_PRESS) move += m_Camera.forward;
    if (glfwGetKey(m_Window, GLFW_KEY_S) == GLFW_PRESS) move -= m_Camera.forward;
    if (glfwGetKey(m_Window, GLFW_KEY_A) == GLFW_PRESS) move -= right;
    if (glfwGetKey(m_Window, GLFW_KEY_D) == GLFW_PRESS) move += right;
    if (glfwGetKey(m_Window, GLFW_KEY_E) == GLFW_PRESS) move += m_Camera.up;
    if (glfwGetKey(m_Window, GLFW_KEY_Q) == GLFW_PRESS) move -= m_Camera.up;

    if (glm::dot(move, move) > 0.0001f)
        m_Camera.position += glm::normalize(move) * speed;

    // ���³���
    float3 forward;
    forward.x = cos(m_Pitch) * sin(m_Yaw);
    forward.y = sin(m_Pitch);
    forward.z = -cos(m_Pitch) * cos(m_Yaw);
    m_Camera.forward = glm::normalize(forward);
}

} // namespace he::editor
