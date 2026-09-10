#pragma once

#include "Scene/MeshComponent.h"

// ============================================================
// SplineMeshComponent — 沿样条生成条带网格（对应 UE5 USplineMeshComponent）
//
// 用途：道路/管线/轨道/赛道等沿路径铺开的几何体。
// 原理：采样关联 SplineComponent 的弧长参数，在每个采样点按切线方向
//       向两侧各偏移 width/2 生成一对顶点，相邻采样点连成四边形条带。
//
// 生成参数（反射 + AI 注解）：width / segments / uvTiling / enabled
// 关联样条：splineEntity（实体 ID，不入反射/词表——与 homingTarget 等实体引用同策略）
//
// 网格由 SplineMeshSystem 在样条版本变化时重建（见 SplineComponent::GetVersion）。
// ============================================================

namespace he {

class SplineComponent;   // fwd

class SplineMeshComponent : public MeshComponent {
    HE_COMPONENT()
public:
    void OnCreate() override;

    // --- 生成参数 ---
    u64   splineEntity = 0;      // 关联 SplineComponent 所在实体 ID（0 = 未设置）
    float width        = 2.0f;   // 条带全宽（米）
    int   segments     = 32;     // 沿样条采样段数（≥1）
    float uvTiling     = 1.0f;   // V 方向 UV 平铺（沿路径每米重复次数）
    bool  enabled      = true;   // 是否生成网格

    /// 从样条重建条带网格。
    /// 样条无效（控制点 < 2）时清空网格，不崩溃。
    void RebuildFrom(SplineComponent& spline);

    /// 当前网格对应的样条数据版本（供系统脏检测；0 = 尚未构建）
    u32 GetBuiltVersion() const { return m_BuiltVersion; }

    /// 强制下次系统更新时重建（生成参数变更后可调用）
    void MarkDirty() { m_BuiltVersion = 0; }

private:
    u32 m_BuiltVersion = 0;      // 已构建网格对应的样条版本
};

} // namespace he
