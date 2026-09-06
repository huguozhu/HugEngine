#pragma once

#include "Math/Math.h"

// ============================================================
// SplineSystem — 样条求值接口（静态系统包装，统一"系统接入"契约）
//
// 与 SplineComponent 的成员方法等价，供 AgentSystem 等
// 外部系统以静态调用形式消费（巡逻路线 / 摄像机轨道）。
// ============================================================

namespace he {

class SplineComponent;

class SplineSystem {
public:
    /// 沿样条走 distance 米处的位置（闭环回绕 / 开环钳制）
    static float3 EvaluateAtDistance(SplineComponent& spline, float distance);

    /// distance 处单位切向
    static float3 GetTangent(SplineComponent& spline, float distance);
};

} // namespace he
