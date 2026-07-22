#pragma once

#include "Math/Math.h"

// ============================================================
// PhysicalLight.h — 基于物理的光源工具函数
//
// 色温 → RGB：Planck 黑体辐射近似（Kryzysztof 公式）
// 适用于 1000K ~ 40000K 范围
// ============================================================

namespace he::render {

/// 色温 (Kelvin) → RGB 颜色（线性空间，近似归一化）
/// 参考: Tanner Helland / Kryzysztof 近似
/// @param kelvin 色温, 单位开尔文 (K), 范围 [1000, 40000]
/// @return 归一化 RGB（线性空间）
inline float3 KelvinToRGB(float kelvin) {
    float t = kelvin / 100.0f;  // 缩放避免大数值精度问题
    float3 rgb;

    // 红色分量
    if (t <= 66.0f) {
        rgb.r = 1.0f;
    } else {
        float r = t - 60.0f;
        r = -0.0004f * r * r * r + 1.0f;
        rgb.r = glm::clamp(r, 0.0f, 1.0f);
    }

    // 绿色分量
    if (t <= 66.0f) {
        float g = t;
        g = -0.0087f * g * g + 1.9284f * g - 42.545f;
        rgb.g = glm::clamp(g / 100.0f, 0.0f, 1.0f);
    } else {
        float g = t - 60.0f;
        g = -0.00328f * g * g + 1.0f;
        rgb.g = glm::clamp(g, 0.0f, 1.0f);
    }

    // 蓝色分量
    if (t <= 20.0f) {
        rgb.b = 1.0f;
    } else if (t <= 66.0f) {
        float b = t - 10.0f;
        b = -0.0213f * b * b + 1.0f;
        rgb.b = glm::clamp(b, 0.0f, 1.0f);
    } else {
        rgb.b = 1.0f;
    }

    return rgb;
}

} // namespace he::render
