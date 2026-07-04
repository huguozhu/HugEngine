// Panels/Gizmo.h — 变换操控器
#pragma once

#include "Core/Types.h"
#include "Math/Math.h"
#include "Pipeline/Camera.h"

// ============================================================
// Gizmo — 3D 变换操控器（平移/旋转/缩放）
//
// 在选中物体位置绘制彩色坐标轴，支持鼠标拖拽操作。
// ImGui DrawList 实现，无需额外着色器或几何体。
// ============================================================

namespace he::editor {

enum class GizmoMode : u8 { Translate, Rotate, Scale };
enum class GizmoSpace : u8 { Local, World };

class Gizmo {
public:
    GizmoMode mode  = GizmoMode::Translate;
    GizmoSpace space = GizmoSpace::World;
    i32 rotateAxis  = 0;  // 0=X, 1=Y, 2=Z（旋转模式下当前激活的轴）

    void cycleRotateAxis() { rotateAxis = (rotateAxis + 1) % 3; }

    /// 渲染 gizmo（在 ImGui 帧内调用，使用 ImDrawList）
    /// 返回 true 表示 gizmo 正在被拖拽（此时隐藏鼠标输入）
    bool Render(const render::CameraData& camera, float3& position, quat& rotation, float3& scale,
                const float4x4& viewProj, const float2& viewportPos, const float2& viewportSize);

    /// 获取当前拖拽状态
    bool IsDragging() const { return m_Dragging; }

private:
    // 3D → 2D 投影
    float2 WorldToScreen(const float3& worldPos, const float4x4& vp,
                         const float2& vpPos, const float2& vpSize) const;

    // 绘制箭头轴
    void DrawAxis(const float2& origin, const float3& axisDir, u32 color,
                  const float4x4& vp, const float2& vpPos, const float2& vpSize,
                  const float3& worldOrigin);

    // 命中检测
    bool HitTest(const float2& mouse, const float2& p1, const float2& p2, float threshold = 8.0f);

    // 模式切换
    static constexpr u32 kColorX = 0xFF3333FF;  // Red (ABGR)
    static constexpr u32 kColorY = 0xFF33FF33;  // Green
    static constexpr u32 kColorZ = 0xFFFF3333;  // Blue
    static constexpr u32 kColorSel = 0xFFFFFF33; // Yellow (selected axis)
    static constexpr float kAxisLength = 2.0f;   // World-space axis length

    bool  m_Dragging     = false;
    i32   m_ActiveAxis   = -1;  // 0=X, 1=Y, 2=Z, -1=none
    float3 m_DragStartPos;
    float2 m_DragStartMouse;
    quat   m_DragStartRot;       // 旋转拖拽起始值
    float  m_DragStartAngle = 0; // 旋转拖拽起始角度
};

} // namespace he::editor
