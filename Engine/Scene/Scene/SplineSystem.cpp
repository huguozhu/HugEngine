// ============================================================
// SplineSystem.cpp — 样条求值静态包装
// ============================================================

#include "Scene/SplineSystem.h"

#include "Scene/SplineComponent.h"

namespace he {

float3 SplineSystem::EvaluateAtDistance(SplineComponent& spline, float distance) {
    return spline.EvaluateAtDistance(distance);
}

float3 SplineSystem::GetTangent(SplineComponent& spline, float distance) {
    return spline.GetTangent(distance);
}

} // namespace he
