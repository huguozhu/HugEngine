// Samples/Editor/Panels/ViewportPanel.cpp

#include "ViewportPanel.h"
#include "Editor/EditorContext.h"
#include "Pipeline/ForwardPipeline.h"
#include "Scene/World.h"
#include "Scene/SceneGraph.h"
#include "Scene/Transform.h"
#include "Scene/MeshComponent.h"
#include "Scene/CubeComponent.h"
#include "Scene/SphereComponent.h"
#include "Core/Log.h"
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

// 射线-AABB 相交检测（slab 方法）
static bool RayAABB(const float3& origin, const float3& dir,
                    const float3& aabbMin, const float3& aabbMax, float& outT) {
    float tMin = 0.0f, tMax = FLT_MAX;
    for (int i = 0; i < 3; ++i) {
        if (std::abs(dir[i]) < 0.0001f) {
            if (origin[i] < aabbMin[i] || origin[i] > aabbMax[i]) return false;
        } else {
            float invD = 1.0f / dir[i];
            float t1 = (aabbMin[i] - origin[i]) * invD;
            float t2 = (aabbMax[i] - origin[i]) * invD;
            if (t1 > t2) std::swap(t1, t2);
            tMin = std::max(tMin, t1);
            tMax = std::min(tMax, t2);
            if (tMin > tMax) return false;
        }
    }
    outT = tMin;
    return true;
}

void ViewportPanel::HandleClickSelect() {
    if (!m_Ctx || !m_Window) return;

    // 用 GLFW 检测左键（避免 ImGui Child 窗口吞事件）
    static bool wasDown = false;
    bool isDown = glfwGetMouseButton(m_Window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
    bool clicked = !wasDown && isDown;
    wasDown = isDown;
    if (!clicked) return;

    // 右键按住时不处理（正在旋转视角）
    if (glfwGetMouseButton(m_Window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS) return;

    // 调试
    static int clickCount = 0;

    // 检查鼠标是否在视口 child 区域内
    double mx, my;
    glfwGetCursorPos(m_Window, &mx, &my);
    if (mx < m_VP_ChildMin.x || my < m_VP_ChildMin.y ||
        mx > m_VP_ChildMax.x || my > m_VP_ChildMax.y) return;
    HE_CORE_INFO("Click #{}: mouse=({:.0f},{:.0f}) child=({:.0f},{:.0f})-({:.0f},{:.0f})",
        ++clickCount, mx, my, m_VP_ChildMin.x, m_VP_ChildMin.y, m_VP_ChildMax.x, m_VP_ChildMax.y);

    auto* world = m_Ctx->GetWorld();
    if (!world) return;

    // 屏幕坐标 → NDC → 世界射线
    float nx = (float(mx) - m_VP_Pos.x) / m_VP_Size.x * 2.0f - 1.0f;
    float ny = 1.0f - (float(my) - m_VP_Pos.y) / m_VP_Size.y * 2.0f;
    float4x4 invVP = glm::inverse(m_CamCtrl.GetCamera().GetViewProjMatrix());
    float4 nearP = invVP * float4(nx, ny, 0.0f, 1.0f);
    float4 farP  = invVP * float4(nx, ny, 1.0f, 1.0f);
    float3 origin = float3(nearP) / nearP.w;
    float3 farPoint = float3(farP) / farP.w;
    float3 dir = glm::normalize(farPoint - origin);
    HE_CORE_INFO("  Ray: origin=({:.1f},{:.1f},{:.1f}) dir=({:.2f},{:.2f},{:.2f})",
        origin.x, origin.y, origin.z, dir.x, dir.y, dir.z);

    // 遍历所有 MeshComponent 做射线-AABB 测试
    Entity bestHit{ kInvalidEntity };
    float bestT = FLT_MAX;
    int testedCount = 0;
    // 收集所有可点击的几何体（Mesh + Cube + Sphere）
    struct HitTarget { AABB bounds; Entity entity; };
    std::vector<HitTarget> targets;
    auto* sg = m_Ctx->GetSceneGraph();
    world->ForEach<he::MeshComponent>([&](he::Entity e, he::MeshComponent& mc) {
        if (mc.GetIndexCount() == 0) return;
        float4x4 wm = sg ? sg->GetWorldMatrix(e) : float4x4(1.0f);
        targets.push_back({mc.GetBounds().Transform(wm), e});
    });
    world->ForEach<he::CubeComponent>([&](he::Entity e, he::CubeComponent& cc) {
        if (cc.GetIndexCount() == 0) return;
        float4x4 wm = sg ? sg->GetWorldMatrix(e) : float4x4(1.0f);
        targets.push_back({cc.GetBounds().Transform(wm), e});
    });
    world->ForEach<he::SphereComponent>([&](he::Entity e, he::SphereComponent& sc) {
        if (sc.GetIndexCount() == 0) return;
        float4x4 wm = sg ? sg->GetWorldMatrix(e) : float4x4(1.0f);
        targets.push_back({sc.GetBounds().Transform(wm), e});
    });

    for (auto& t : targets) {
        float tHit;
        if (RayAABB(origin, dir, t.bounds.min, t.bounds.max, tHit) && tHit < bestT) {
            bestT = tHit;
            bestHit = t.entity;
        }
        testedCount++;
    }

    HE_CORE_INFO("  Tested {} targets, bestHit={}, t={:.1f}", testedCount, bestHit.id, bestT);
    if (bestHit.IsValid()) {
        m_Ctx->SelectEntity(bestHit);
        HE_CORE_INFO("  Selected entity #{}", bestHit.id);
    }
}

} // namespace he::editor
