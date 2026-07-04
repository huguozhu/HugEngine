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
#include "Scene/LightComponent.h"
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

void ViewportPanel::RenderDebugOverlay() {
    // F 键切换
    static bool showDebug = false;
    if (ImGui::IsKeyPressed(ImGuiKey_F)) showDebug = !showDebug;
    if (!showDebug) return;

    auto* dl = ImGui::GetWindowDrawList();
    auto* world = m_Ctx ? m_Ctx->GetWorld() : nullptr;
    if (!world) return;

    const auto& cam = m_CamCtrl.GetCamera();
    float4x4 vp = cam.GetViewProjMatrix();
    float4x4 invVP = glm::inverse(vp);

    // 3D → 2D 投影辅助
    auto project = [&](const float3& wp) -> float2 {
        float4 clip = vp * float4(wp, 1.0f);
        if (std::abs(clip.w) < 0.0001f) return float2(-9999);
        float3 ndc = float3(clip) / clip.w;
        return float2(m_VP_Pos.x + (ndc.x * 0.5f + 0.5f) * m_VP_Size.x,
                      m_VP_Pos.y + (0.5f - ndc.y * 0.5f) * m_VP_Size.y);
    };

    auto drawLine3D = [&](const float3& a, const float3& b, u32 col) {
        float2 sa = project(a), sb = project(b);
        if (sa.x < -100 || sb.x < -100) return;
        dl->AddLine(ImVec2(sa.x, sa.y), ImVec2(sb.x, sb.y), col, 1.5f);
    };

    // === 相机视锥体 ===
    float n = cam.nearPlane, f = cam.farPlane;
    float4 ndcCorners[8] = {
        {-1,-1,0,1}, {1,-1,0,1}, {1,1,0,1}, {-1,1,0,1},  // near
        {-1,-1,1,1}, {1,-1,1,1}, {1,1,1,1}, {-1,1,1,1},  // far
    };
    float3 corners[8];
    for (int i = 0; i < 8; ++i) {
        float4 w = invVP * ndcCorners[i];
        corners[i] = float3(w) / w.w;
    }
    u32 camCol = 0xFFAAFFFF;
    // near rect
    for (int i = 0; i < 4; ++i) drawLine3D(corners[i], corners[(i+1)%4], camCol);
    // far rect
    for (int i = 0; i < 4; ++i) drawLine3D(corners[i+4], corners[4+(i+1)%4], camCol);
    // connecting edges
    for (int i = 0; i < 4; ++i) drawLine3D(corners[i], corners[i+4], camCol);

    // === 编辑器相机视锥体 (Perspective) ===
    {
        float3 camPos = m_CamCtrl.GetCamera().position;
        float3 camFwd = m_CamCtrl.GetCamera().forward;
        float3 camUp  = m_CamCtrl.GetCamera().up;
        float3 camRight = glm::cross(camFwd, camUp);
        float n = m_CamCtrl.GetCamera().nearPlane;
        float f = m_CamCtrl.GetCamera().farPlane;
        float fovRad = glm::radians(m_CamCtrl.GetCamera().fov);
        float aspect = m_CamCtrl.GetCamera().aspectRatio;

        float nearH = std::tan(fovRad * 0.5f) * n;
        float nearW = nearH * aspect;
        float farH  = std::tan(fovRad * 0.5f) * f;
        float farW  = farH * aspect;

        float3 nearCenter = camPos + camFwd * n;
        float3 farCenter  = camPos + camFwd * f;
        float3 nc[4] = { nearCenter + camUp*nearH - camRight*nearW, nearCenter + camUp*nearH + camRight*nearW,
                         nearCenter - camUp*nearH + camRight*nearW, nearCenter - camUp*nearH - camRight*nearW };
        float3 fc[4] = { farCenter  + camUp*farH  - camRight*farW,  farCenter  + camUp*farH  + camRight*farW,
                         farCenter  - camUp*farH  + camRight*farW,  farCenter  - camUp*farH  - camRight*farW };
        u32 cCol = 0xFF6699FF;
        for (int i = 0; i < 4; ++i) { drawLine3D(nc[i], nc[(i+1)%4], cCol); drawLine3D(fc[i], fc[(i+1)%4], cCol); drawLine3D(nc[i], fc[i], cCol); }
        dl->AddText(ImVec2(project(camPos).x, project(camPos).y - 20), cCol, "Camera");
    }

    // === 点光源：球体线框 ===
    world->ForEach<he::PointLight>([&](he::Entity e, he::PointLight& pl) {
        if (!pl.enabled) return;
        auto* tf = world->GetComponent<TransformComponent>(e);
        float3 pos = tf ? tf->position : float3(0);
        float r = pl.range;
        int segs = 24;
        u32 col = 0xFFFFAA33;
        // 3 个正交圆环模拟球体
        for (int axis = 0; axis < 3; ++axis) {
            float3 u, v;
            if (axis == 0) { u = float3(1,0,0); v = float3(0,1,0); }
            else if (axis == 1) { u = float3(1,0,0); v = float3(0,0,1); }
            else { u = float3(0,1,0); v = float3(0,0,1); }
            float2 prev;
            for (int i = 0; i <= segs; ++i) {
                float a = float(i) / segs * glm::radians(360.0f);
                float3 pt = pos + (u * std::cos(a) + v * std::sin(a)) * r;
                float2 sp = project(pt);
                if (i > 0) dl->AddLine(ImVec2(prev.x, prev.y), ImVec2(sp.x, sp.y), col, 1.0f);
                prev = sp;
            }
        }
        dl->AddText(ImVec2(project(pos).x + 10, project(pos).y - 8), col, "PointLight");
    });

    // === 聚光灯：锥体线框 ===
    world->ForEach<he::SpotLight>([&](he::Entity e, he::SpotLight& sl) {
        if (!sl.enabled) return;
        auto* tf = world->GetComponent<TransformComponent>(e);
        float3 pos = tf ? tf->position : float3(0);
        float3 dir = glm::normalize(sl.direction);
        float3 up = std::abs(dir.y) < 0.99f ? float3(0,1,0) : float3(1,0,0);
        float3 right = glm::normalize(glm::cross(dir, up));
        up = glm::cross(right, dir);
        float r = sl.range;
        float outerR = std::tan(sl.outerConeAngle * 0.5f) * r;
        float innerR = std::tan(sl.innerConeAngle * 0.5f) * r;
        float3 baseCenter = pos + dir * r;
        int segs = 20;
        u32 outerCol = 0xFF33FFAA, innerCol = 0xFF66FFCC;
        float2 prevOuter, prevInner;
        for (int i = 0; i <= segs; ++i) {
            float a = float(i) / segs * glm::radians(360.0f);
            float3 ptOuter = baseCenter + (right*std::cos(a) + up*std::sin(a)) * outerR;
            float3 ptInner = baseCenter + (right*std::cos(a) + up*std::sin(a)) * innerR;
            float2 so = project(ptOuter), si = project(ptInner);
            if (i > 0) { dl->AddLine(ImVec2(prevOuter.x,prevOuter.y), ImVec2(so.x,so.y), outerCol, 1.0f);
                         dl->AddLine(ImVec2(prevInner.x,prevInner.y), ImVec2(si.x,si.y), innerCol, 1.0f); }
            dl->AddLine(ImVec2(project(pos).x, project(pos).y), ImVec2(so.x, so.y), outerCol, 1.0f);
            prevOuter = so; prevInner = si;
        }
        dl->AddText(ImVec2(project(pos).x + 10, project(pos).y - 8), 0xFF33FFAA, "SpotLight");
    });

    // === 方向光：从相机视锥体反算正交投影盒 (Orthographic) ===
    // 先获取当前相机视锥体 8 个角点（世界空间）
    float3 camCorners[8];
    {
        float cn = cam.nearPlane, cf = cam.farPlane;
        float fovRad = glm::radians(cam.fov);
        float aspect = cam.aspectRatio;
        float nh = std::tan(fovRad * 0.5f) * cn, nw = nh * aspect;
        float fh = std::tan(fovRad * 0.5f) * cf, fw = fh * aspect;
        float3 cFwd = cam.forward, cUp = cam.up, cRight = glm::cross(cFwd, cUp);
        float3 nc = cam.position + cFwd * cn;
        float3 fc = cam.position + cFwd * cf;
        camCorners[0] = nc + cUp*nh - cRight*nw; camCorners[1] = nc + cUp*nh + cRight*nw;
        camCorners[2] = nc - cUp*nh + cRight*nw; camCorners[3] = nc - cUp*nh - cRight*nw;
        camCorners[4] = fc + cUp*fh - cRight*fw; camCorners[5] = fc + cUp*fh + cRight*fw;
        camCorners[6] = fc - cUp*fh + cRight*fw; camCorners[7] = fc - cUp*fh - cRight*fw;
    }

    world->ForEach<he::DirectionalLight>([&](he::Entity e, he::DirectionalLight& dirLight) {
        if (!dirLight.enabled) return;
        auto* tf = world->GetComponent<TransformComponent>(e);
        float3 lPos = tf ? tf->position : float3(0);
        float3 lDir = glm::normalize(dirLight.direction);
        float3 lUp = std::abs(lDir.y) < 0.99f ? float3(0,1,0) : float3(1,0,0);
        float3 lRight = glm::normalize(glm::cross(lDir, lUp));
        lUp = glm::cross(lRight, lDir);
        // 构建光空间 view 矩阵，将相机视锥角点变换到光空间求 AABB
        float4x4 lightView = glm::lookAtRH(lPos, lPos + lDir, lUp);
        float3 lsMin(FLT_MAX), lsMax(-FLT_MAX);
        for (int i = 0; i < 8; ++i) {
            float3 ls = float3(lightView * float4(camCorners[i], 1.0f));
            lsMin = glm::min(lsMin, ls); lsMax = glm::max(lsMax, ls);
        }
        // 扩展 Z 范围以容纳近处物体
        lsMin.z -= 10.0f; lsMax.z += 10.0f;
        // 逆变换角点回世界空间绘制
        float3 b[8];
        for (int i = 0; i < 8; ++i) {
            float3 ls((i&1)?lsMax.x:lsMin.x, (i&2)?lsMax.y:lsMin.y, (i&4)?lsMax.z:lsMin.z);
            float4 ws = glm::inverse(lightView) * float4(ls, 1.0f);
            b[i] = float3(ws) / ws.w;
        }
        u32 dCol = 0xFFFF6666;
        for (int i = 0; i < 4; ++i) {
            drawLine3D(b[i], b[(i+1)%4], dCol);
            drawLine3D(b[i+4], b[4+(i+1)%4], dCol);
            drawLine3D(b[i], b[i+4], dCol);
        }
        dl->AddText(ImVec2(project(lPos).x + 10, project(lPos).y - 8), dCol, "DirLight");
    });
}

} // namespace he::editor
