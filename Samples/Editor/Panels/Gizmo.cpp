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
    float2 center = WorldToScreen(position, viewProj, vpPos, vpSize);

    if (center.x < vpPos.x - 50 || center.y < vpPos.y - 50 ||
        center.x > vpPos.x + vpSize.x + 50 || center.y > vpPos.y + vpSize.y + 50)
        return false;

    float3 axX = (space == GizmoSpace::Local) ? rotation * float3(1,0,0) : float3(1,0,0);
    float3 axY = (space == GizmoSpace::Local) ? rotation * float3(0,1,0) : float3(0,1,0);
    float3 axZ = (space == GizmoSpace::Local) ? rotation * float3(0,0,1) : float3(0,0,1);

    ImVec2 mouse = ImGui::GetMousePos();
    float2 mPos(mouse.x, mouse.y);
    bool hovered = false;

    if (mode == GizmoMode::Translate) {
        // === 平移模式 ===
        DrawAxis(center, axX, kColorX, viewProj, vpPos, vpSize, position);
        DrawAxis(center, axY, kColorY, viewProj, vpPos, vpSize, position);
        DrawAxis(center, axZ, kColorZ, viewProj, vpPos, vpSize, position);
        dl->AddCircleFilled(ImVec2(center.x, center.y), 4.0f, 0xFFFFFFFF);

        if (!m_Dragging) {
            float3 axes[] = {axX, axY, axZ};
            for (int i = 0; i < 3; ++i) {
                float2 end2D = WorldToScreen(position + axes[i] * kAxisLength, viewProj, vpPos, vpSize);
                if (HitTest(mPos, center, end2D, 10.0f)) {
                    float2 hEnd = WorldToScreen(position + axes[i] * kAxisLength, viewProj, vpPos, vpSize);
                    dl->AddLine(ImVec2(center.x, center.y), ImVec2(hEnd.x, hEnd.y), kColorSel, 3.0f);
                    hovered = true;
                    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                        m_Dragging = true; m_ActiveAxis = i;
                        m_DragStartPos = position; m_DragStartMouse = mPos;
                    }
                    break;
                }
            }
        } else {
            float3 tax[3] = {axX, axY, axZ};
            float3 moveAxis = tax[m_ActiveAxis];
            float2 mouseDelta = mPos - m_DragStartMouse;
            float2 s0 = WorldToScreen(position, viewProj, vpPos, vpSize);
            float2 s1 = WorldToScreen(position + moveAxis, viewProj, vpPos, vpSize);
            float2 screenAxis = s1 - s0;
            float len = glm::length(screenAxis);
            if (len > 0.001f) {
                float worldDelta = glm::dot(mouseDelta, screenAxis / len) * kAxisLength / std::max(len, 1.0f);
                position = m_DragStartPos + moveAxis * worldDelta;
            }
            if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) { m_Dragging = false; m_ActiveAxis = -1; }
        }
    } else if (mode == GizmoMode::Rotate) {
        // === 旋转模式（单轴环，R 键切换）===
        float ringR = 50.0f;
        u32 colors[3] = {kColorX, kColorY, kColorZ};
        float3 axes[3] = {axX, axY, axZ};
        u32 axCol = colors[rotateAxis];
        float3 rotAx = axes[rotateAxis];

        dl->AddCircleFilled(ImVec2(center.x, center.y), 3.0f, 0xFFFFFFFF);

        // 绘制当前激活的旋转环
        dl->AddCircle(ImVec2(center.x, center.y), ringR, axCol, 48, 2.5f);
        // 轴标签
        const char* labels[3] = {"X", "Y", "Z"};
        float2 labelP = WorldToScreen(position + rotAx * 1.2f, viewProj, vpPos, vpSize);
        dl->AddText(ImVec2(labelP.x - 4, labelP.y - 8), axCol, labels[rotateAxis]);

        // 绘制其他轴的灰色淡环（参考）
        for (int i = 0; i < 3; ++i) {
            if (i == rotateAxis) continue;
            dl->AddCircle(ImVec2(center.x, center.y), ringR, colors[i] & 0x44FFFFFF, 48, 1.0f);
        }

        // 交互
        if (!m_Dragging) {
            float distToRing = std::abs(glm::length(mPos - center) - ringR);
            if (distToRing < 12.0f) {
                dl->AddCircle(ImVec2(center.x, center.y), ringR, kColorSel, 48, 3.5f);
                hovered = true;
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    m_Dragging = true; m_ActiveAxis = rotateAxis;
                    m_DragStartRot = rotation;
                    m_DragStartAngle = std::atan2(mPos.y - center.y, mPos.x - center.x);
                }
            }
        } else {
            float newAngle = std::atan2(mPos.y - center.y, mPos.x - center.x);
            float deltaAngle = newAngle - m_DragStartAngle;
            rotation = glm::angleAxis(deltaAngle, rotAx) * m_DragStartRot;
            dl->AddCircle(ImVec2(center.x, center.y), ringR, kColorSel, 48, 3.5f);
            if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) { m_Dragging = false; }
        }
    }

    return m_Dragging || hovered;
}

} // namespace he::editor
