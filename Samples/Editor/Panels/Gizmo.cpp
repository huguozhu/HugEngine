// Panels/Gizmo.cpp — 变换操控器实现
#include "Gizmo.h"
#include "imgui.h"
#include <algorithm>
#include <cmath>

namespace he::editor {

float2 Gizmo::WorldToScreen(const float3& wp, const float4x4& vp,
                             const float2& vpPos, const float2& vpSize) const {
    float4 clip = vp * float4(wp, 1.0f);
    if (std::abs(clip.w) < 0.0001f) return float2(-9999);
    float3 ndc = float3(clip) / clip.w;
    return float2(
        vpPos.x + (ndc.x * 0.5f + 0.5f) * vpSize.x,
        vpPos.y + (0.5f - ndc.y * 0.5f) * vpSize.y);  // Vulkan Y flip
}

bool Gizmo::HitTest(const float2& mouse, const float2& p1, const float2& p2, float threshold) {
    // 点到线段的最短距离
    float2 ab = p2 - p1;
    float len2 = glm::dot(ab, ab);
    if (len2 < 0.0001f) return glm::length(mouse - p1) < threshold;
    float t = glm::clamp(glm::dot(mouse - p1, ab) / len2, 0.0f, 1.0f);
    float2 closest = p1 + ab * t;
    return glm::length(mouse - closest) < threshold;
}

void Gizmo::DrawAxis(const float2& origin, const float3& axisDir, u32 color,
                      const float4x4& vp, const float2& vpPos, const float2& vpSize,
                      const float3& worldOrigin) {
    auto* dl = ImGui::GetWindowDrawList();
    float3 endWorld = worldOrigin + axisDir * kAxisLength;
    float2 end = WorldToScreen(endWorld, vp, vpPos, vpSize);

    // 超出屏幕范围不画
    if (end.x < -100 || end.y < -100 || end.x > vpPos.x + vpSize.x + 100 ||
        end.y > vpPos.y + vpSize.y + 100)
        return;

    // 选中的轴用黄色
    u32 col = (m_Dragging && m_ActiveAxis >= 0) ? kColorSel : color;

    // 画线 + 箭头
    dl->AddLine(ImVec2(origin.x, origin.y), ImVec2(end.x, end.y), col, 2.0f);

    // 简易箭头（小三角形）
    float2 dir = glm::normalize(end - origin);
    float2 perp(dir.y, -dir.x);
    float arrowSize = 10.0f;
    float2 tip = end;
    float2 l = end - dir * arrowSize * 2.0f + perp * arrowSize * 0.5f;
    float2 r = end - dir * arrowSize * 2.0f - perp * arrowSize * 0.5f;
    dl->AddTriangleFilled(ImVec2(tip.x, tip.y), ImVec2(l.x, l.y), ImVec2(r.x, r.y), col);
}

bool Gizmo::Render(const render::CameraData& camera, float3& position, quat& rotation, float3& scale,
                    const float4x4& viewProj, const float2& vpPos, const float2& vpSize) {
    auto* dl = ImGui::GetWindowDrawList();
    float2 origin2D = WorldToScreen(position, viewProj, vpPos, vpSize);

    // 原点不在视口内或太远
    if (origin2D.x < vpPos.x - 50 || origin2D.y < vpPos.y - 50 ||
        origin2D.x > vpPos.x + vpSize.x + 50 ||
        origin2D.y > vpPos.y + vpSize.y + 50)
        return false;

    // 根据模式选择轴向
    float3 axisX = (space == GizmoSpace::Local) ? rotation * float3(1, 0, 0) : float3(1, 0, 0);
    float3 axisY = (space == GizmoSpace::Local) ? rotation * float3(0, 1, 0) : float3(0, 1, 0);
    float3 axisZ = (space == GizmoSpace::Local) ? rotation * float3(0, 0, 1) : float3(0, 0, 1);

    // 绘制轴
    DrawAxis(origin2D, axisX, kColorX, viewProj, vpPos, vpSize, position);
    DrawAxis(origin2D, axisY, kColorY, viewProj, vpPos, vpSize, position);
    DrawAxis(origin2D, axisZ, kColorZ, viewProj, vpPos, vpSize, position);

    // 原点圆点
    dl->AddCircleFilled(ImVec2(origin2D.x, origin2D.y), 4.0f, 0xFFFFFFFF);

    // --- 鼠标交互（平移模式）---
    ImVec2 mouse = ImGui::GetMousePos();
    float2 mPos(mouse.x, mouse.y);
    bool hovered = false;

    if (!m_Dragging) {
        // 检测悬停
        float3 axes[] = {axisX, axisY, axisZ};
        for (int i = 0; i < 3; ++i) {
            float3 endWorld = position + axes[i] * kAxisLength;
            float2 end2D = WorldToScreen(endWorld, viewProj, vpPos, vpSize);
            if (HitTest(mPos, origin2D, end2D, 10.0f)) {
                // 高亮选中轴
                float3 highlightEnd = position + axes[i] * kAxisLength;
                float2 hEnd = WorldToScreen(highlightEnd, viewProj, vpPos, vpSize);
                dl->AddLine(ImVec2(origin2D.x, origin2D.y), ImVec2(hEnd.x, hEnd.y), kColorSel, 3.0f);
                hovered = true;

                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    m_Dragging = true;
                    m_ActiveAxis = i;
                    m_DragStartPos = position;
                    m_DragStartMouse = mPos;
                }
                break;
            }
        }
    } else {
        // 拖拽中：沿轴移动
        float3 axes[] = {axisX, axisY, axisZ};
        float3 moveAxis = axes[m_ActiveAxis];

        // 计算屏幕空间移动量 → 世界空间
        float2 mouseDelta = mPos - m_DragStartMouse;
        // 投影轴到屏幕：用轴方向的 3D 点投影到屏幕，计算屏幕空间的轴向量
        float3 axisPoint = position + moveAxis;
        float2 axisScreen0 = WorldToScreen(position, viewProj, vpPos, vpSize);
        float2 axisScreen1 = WorldToScreen(axisPoint, viewProj, vpPos, vpSize);
        float2 screenAxis = axisScreen1 - axisScreen0;
        float screenAxisLen = glm::length(screenAxis);
        if (screenAxisLen > 0.001f) {
            screenAxis /= screenAxisLen;
            float projDelta = glm::dot(mouseDelta, screenAxis);
            // 屏幕像素 → 世界单位（粗略近似）
            float worldDelta = projDelta * kAxisLength / std::max(screenAxisLen, 1.0f);
            position = m_DragStartPos + moveAxis * worldDelta;
        }

        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            m_Dragging = false;
            m_ActiveAxis = -1;
        }
    }

    return m_Dragging || hovered;
}

} // namespace he::editor
