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
#include "Scene/CameraComponent.h"
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
        if (ImGui::IsKeyPressed(ImGuiKey_R)) {
            if (m_Gizmo.mode != GizmoMode::Rotate) m_Gizmo.mode = GizmoMode::Rotate;
            else m_Gizmo.cycleRotateAxis();
        }
        if (ImGui::IsKeyPressed(ImGuiKey_S)) m_Gizmo.mode = GizmoMode::Scale;
        if (ImGui::IsKeyPressed(ImGuiKey_X)) m_Gizmo.space = (m_Gizmo.space == GizmoSpace::Local)
            ? GizmoSpace::World : GizmoSpace::Local;
    }

    // Ctrl 键临时禁用吸附（按住 Ctrl 时自由变换）
    m_Gizmo.snapEnabled = !ImGui::GetIO().KeyCtrl;

    // 渲染 gizmo（记录是否 hover，点击选中时跳过）
    float4x4 vp = m_CamCtrl.GetCamera().GetViewProjMatrix();
    float3 pos = tf->position;
    quat rot = tf->rotation;
    float3 scl = tf->scale;

    m_GizmoHovered = m_Gizmo.Render(m_CamCtrl.GetCamera(), pos, rot, scl, vp, m_VP_Pos, m_VP_Size);

    // 应用变换 + 标记脏（Render 内已修改 pos/rot/scl）
    tf->position = pos;
    tf->rotation = rot;
    tf->scale    = scl;
    if (m_Gizmo.IsDragging() || m_GizmoHovered)
        m_Ctx->GetSceneGraph()->MarkDirty(selEnt);
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

    // --- 键盘移动（Gizmo 交互时禁用，避免按键冲突）---
    render::CameraController::MoveInput moveIn;
    bool gizmoActive = m_Gizmo.IsDragging() || m_GizmoHovered;
    moveIn.forward  = !gizmoActive && glfwGetKey(m_Window, GLFW_KEY_W) == GLFW_PRESS;
    moveIn.backward = glfwGetKey(m_Window, GLFW_KEY_S) == GLFW_PRESS;
    moveIn.left     = glfwGetKey(m_Window, GLFW_KEY_A) == GLFW_PRESS;
    moveIn.right    = glfwGetKey(m_Window, GLFW_KEY_D) == GLFW_PRESS;
    moveIn.up       = !gizmoActive && glfwGetKey(m_Window, GLFW_KEY_E) == GLFW_PRESS;
    moveIn.down     = !gizmoActive && glfwGetKey(m_Window, GLFW_KEY_Q) == GLFW_PRESS;
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
    // Gizmo 交互中不处理点击选中
    if (m_Gizmo.IsDragging() || m_GizmoHovered) return;

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

    // 仅对选中实体显示
    he::Entity selEnt{ kInvalidEntity };
    { auto& s = m_Ctx->GetSelection(); if (!s.empty()) selEnt = s[0]; }

    // === 相机视锥体（选中 CameraComponent 实体）===
    if (selEnt.IsValid() && world->HasComponent<CameraComponent>(selEnt)) {
        auto* cc = world->GetComponent<CameraComponent>(selEnt);
        auto* tf = world->GetComponent<TransformComponent>(selEnt);
        if (cc && tf) {
            float3 pos = tf->position, fwd = tf->GetForward(), up = tf->GetUp();
            float3 right = glm::cross(fwd, up);
            float f = cc->farPlane, fovRad = glm::radians(cc->fov), aspect = cc->aspectRatio;
            float fh = std::tan(fovRad * 0.5f) * f, fw = fh * aspect;
            float3 fc = pos + fwd * f;
            float3 fC[4] = { fc + up*fh - right*fw, fc + up*fh + right*fw,
                             fc - up*fh + right*fw, fc - up*fh - right*fw };
            u32 col = 0xFF6699FF;
            for (int i = 0; i < 4; ++i) {
                drawLine3D(fC[i], fC[(i+1)%4], col);
                drawLine3D(pos, fC[i], col);
            }
            dl->AddText(ImVec2(project(pos).x, project(pos).y - 20), col, "Camera");
        }
    }

    // === 点光源：6 面透视视锥体（选中时）===
    world->ForEach<he::PointLight>([&](he::Entity e, he::PointLight& pl) {
        if (!pl.enabled || e != selEnt) return;
        float3 pos = world->GetComponent<TransformComponent>(e)->position;
        float r = pl.range; u32 col = 0xFFFFAA33;
        struct { float3 dir; float3 up; } faces[6] = {
            {{ 1,0,0},{0,1,0}}, {{-1,0,0},{0,1,0}}, {{0,1,0},{0,0,1}},
            {{0,-1,0},{0,0,-1}}, {{0,0,1},{0,1,0}}, {{0,0,-1},{0,1,0}},
        };
        for (int fi = 0; fi < 6; ++fi) {
            float3 fwd = faces[fi].dir, up = faces[fi].up;
            float3 right = glm::normalize(glm::cross(fwd, up));
            up = glm::normalize(glm::cross(right, fwd));
            float fp = r;
            float fh = fp, fw = fp;
            float3 fc = pos + fwd * fp;
            float3 c[4] = { fc + up*fh - right*fw, fc + up*fh + right*fw,
                            fc - up*fh + right*fw, fc - up*fh - right*fw };
            for (int i = 0; i < 4; ++i) {
                drawLine3D(c[i], c[(i+1)%4], col);
                drawLine3D(pos, c[i], col);
            }
        }
        dl->AddText(ImVec2(project(pos).x + 10, project(pos).y - 8), col, "PointLight");
    });

    // === 聚光灯（选中时）===
    world->ForEach<he::SpotLight>([&](he::Entity e, he::SpotLight& sl) {
        if (!sl.enabled || e != selEnt) return;
        auto* tf = world->GetComponent<TransformComponent>(e);
        float3 pos = tf ? tf->position : float3(0);
        float3 dir = glm::normalize(sl.direction);
        float3 up  = tf ? tf->GetUp() : float3(0,1,0);
        float3 right = tf ? tf->GetRight() : glm::normalize(glm::cross(dir, up));
        float r = sl.range, outerR = std::tan(sl.outerConeAngle*0.5f)*r, innerR = std::tan(sl.innerConeAngle*0.5f)*r;
        float3 base = pos + dir * r;
        int segs = 20; float2 prevO, prevI;
        for (int i = 0; i <= segs; ++i) {
            float a = float(i)/segs * glm::radians(360.0f);
            float3 po = base + (right*std::cos(a)+up*std::sin(a))*outerR;
            float3 pi = base + (right*std::cos(a)+up*std::sin(a))*innerR;
            float2 so = project(po), si = project(pi);
            if (i > 0) {
                dl->AddLine(ImVec2(prevO.x,prevO.y), ImVec2(so.x,so.y), 0xFF33FFAA, 1.0f);
                dl->AddLine(ImVec2(prevI.x,prevI.y), ImVec2(si.x,si.y), 0xFF66FFCC, 1.0f);
            }
            dl->AddLine(ImVec2(project(pos).x,project(pos).y), ImVec2(so.x,so.y), 0xFF33FFAA, 1.0f);
            prevO = so; prevI = si;
        }
        dl->AddText(ImVec2(project(pos).x + 10, project(pos).y - 8), 0xFF33FFAA, "SpotLight");
    });

    // === 方向光：正交视锥体（从光源位置出发）===
    world->ForEach<he::DirectionalLight>([&](he::Entity e, he::DirectionalLight& dirLight) {
        if (!dirLight.enabled || e != selEnt) return;
        auto* tf = world->GetComponent<TransformComponent>(e);
        float3 lPos = tf ? tf->position : float3(0);
        // 使用 Transform 的旋转 + 光源 direction 计算朝向
        float3 lDir = glm::normalize(dirLight.direction);
        float3 lUp = tf ? tf->GetUp() : float3(0,1,0);
        float3 lRight = tf ? tf->GetRight() : glm::normalize(glm::cross(lDir, lUp));
        float nearD = 0.1f, farD = 10.0f, halfSz = 3.0f;
        float3 nearC = lPos + lDir * nearD, farC = lPos + lDir * farD;
        float3 b[8];
        b[0]=nearC+lUp*halfSz-lRight*halfSz; b[1]=nearC+lUp*halfSz+lRight*halfSz;
        b[2]=nearC-lUp*halfSz+lRight*halfSz; b[3]=nearC-lUp*halfSz-lRight*halfSz;
        b[4]=farC+lUp*halfSz-lRight*halfSz;  b[5]=farC+lUp*halfSz+lRight*halfSz;
        b[6]=farC-lUp*halfSz+lRight*halfSz;  b[7]=farC-lUp*halfSz-lRight*halfSz;
        u32 dCol = 0xFFFF6666;
        for (int i = 0; i < 4; ++i) {
            drawLine3D(b[i], b[(i+1)%4], dCol); drawLine3D(b[i+4], b[4+(i+1)%4], dCol);
            drawLine3D(b[i], b[i+4], dCol);
        }
        dl->AddText(ImVec2(project(lPos).x + 10, project(lPos).y - 8), dCol, "DirLight");
    });
}

} // namespace he::editor
