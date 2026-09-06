#pragma once

#include "Scene/Component.h"
#include "Math/Math.h"

#include <vector>

// ============================================================
// SplineComponent — 样条路径（对应 UE5 USplineComponent）
//
// 用途：道路/管线/摄像机轨道/Agent 巡逻路线。
// 曲线：Hermite 分段三次样条；每控制点含位置 + 切向，
//       切向传零向量时自动生成 Catmull-Rom 切线（连续光滑）。
//
// 求值 API（弧长参数化）：
//   EvaluateAtDistance(d)：沿样条走 d 米的位置（闭环自动回绕）
//   EvaluateAtParam(t)   ：参数 t ∈ [0, 段数]（每段 = 1，闭环循环）
//   GetTangent(d)        ：d 处单位切向
// 弧长按每段 16 步自适应采样缓存，加点后惰性重建。
// SplineMeshComponent（沿样条生成网格）后续扩展；调试路径绘制（bShowPath）预留。
// ============================================================

namespace he {

/// 样条控制点（tangent 为零向量时自动 Catmull-Rom）
struct SplinePoint {
    float3 position = float3(0.0f);
    float3 tangent  = float3(0.0f);
};

class SplineComponent : public Component {
    HE_COMPONENT()
public:
    std::vector<SplinePoint> points;    // 控制点（≥2 个才能求值）
    bool bClosedLoop = false;           // 闭合路径（末点回连首点）
    bool bShowPath   = true;            // 调试路径绘制开关（预留，MVP 未接渲染）

    /// 追加控制点（tangent=0 → 自动 Catmull-Rom 切线）
    void AddPoint(const float3& position, const float3& tangent = float3(0.0f));
    void Clear();

    /// 段数：开环 = n-1；闭环 = n
    int GetSegmentCount() const;

    /// 总弧长（米，惰性缓存）
    float GetTotalLength();

    /// 沿样条走 distance 米处的位置（闭环回绕；开环钳制到 [0, 总长]）
    float3 EvaluateAtDistance(float distance);

    /// 参数 t ∈ [0, 段数] 处的位置（闭环循环回绕）
    float3 EvaluateAtParam(float t);

    /// distance 处单位切向
    float3 GetTangent(float distance);

private:
    /// 重建缓存：自动切线 + 弧长表（AddPoint/Clear 置脏后惰性触发）
    void Rebuild();

    bool m_Dirty = true;
    float m_TotalLength = 0.0f;
    std::vector<float> m_CumLen;   // 每段累计弧长（n+1 项，末项 = 总长）
};

} // namespace he
