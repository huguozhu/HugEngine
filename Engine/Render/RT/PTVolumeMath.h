#pragma once

#include "Core/Types.h"
#include "Math/Math.h"

#include <cmath>

namespace he::render {

// ============================================================
// PTVolumeMath — 电介质界面 / 参与介质的 CPU 侧数学（PT 任务 4）
//
// 与 Engine/Shader/Shaders/PT_Common.slang 里的
//   FresnelDielectric / RefractDirection / SampleDistanceInMedium / BeerTransmittance
// **逐式同构**：改装 shader 侧公式时必须同步这里，否则单元测试就失去意义
// （本文件的存在意义正是"用解析解把这个公式钉住"）。
// ============================================================

/// 电介质菲涅尔反射率（Schlick 近似）
inline float FresnelDielectricCPU(float cosTheta, float f0) {
    const float c = glm::clamp(cosTheta, 0.0f, 1.0f);
    return f0 + (1.0f - f0) * std::pow(1.0f - c, 5.0f);
}

/// Snell 折射：I 为入射方向（指向表面，已归一化），N 为朝向入射侧的法线，
/// eta = n1/n2。返回 false = 全内反射。
inline bool RefractDirectionCPU(const float3& I, const float3& N, float eta, float3& T) {
    const float cosI = glm::dot(N, I);
    const float sin2T = eta * eta * (1.0f - cosI * cosI);
    if (sin2T > 1.0f) { T = float3(0.0f); return false; }
    const float cosT = std::sqrt(std::max(0.0f, 1.0f - sin2T));
    T = glm::normalize(eta * I - (eta * cosI + cosT) * N);
    return true;
}

/// 均匀介质的自由程采样（指数分布）：P(d > D) = exp(-σ_t·D)
inline float SampleDistanceInMediumCPU(float u, float sigmaT) {
    return -std::log(std::max(1.0f - u, 1e-6f)) / std::max(sigmaT, 1e-6f);
}

/// Beer-Lambert 透射率（解析解）：T = exp(-σ_t · d)
inline float3 BeerTransmittanceCPU(const float3& sigmaT, float d) {
    return glm::exp(-sigmaT * std::max(d, 0.0f));
}

/// 由 glTF 的 attenuationColor / attenuationDistance 推导吸收系数（与 RTPass 同式）
inline float3 ComputeSigmaT(const float3& attenuationColor, float attenuationDistance) {
    if (attenuationDistance <= 0.0f) return float3(0.0f);
    const float inv = 1.0f / attenuationDistance;
    return float3(-std::log(std::max(attenuationColor.r, 1e-6f)) * inv,
                  -std::log(std::max(attenuationColor.g, 1e-6f)) * inv,
                  -std::log(std::max(attenuationColor.b, 1e-6f)) * inv);
}

} // namespace he::render
