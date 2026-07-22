#pragma once

#include "Scene/Component.h"
#include "Math/Math.h"

// ============================================================
// LightComponent — 光照组件体系
//
// 基类 + 3 个子类：DirectionalLight / PointLight / SpotLight
//
// 物理模式（向后兼容）:
//   luminousIntensity>0 / illuminance>0 → 物理单位 + 平方反比衰减
//   luminousIntensity=0 / illuminance=0 → 传统 intensity + 多项式衰减
//   colorTemperature>0 → 色温→RGB 叠加到 color 滤镜
// ============================================================

namespace he {

// --- 光源类型 ---
enum class LightType : u8 {
    Directional = 0,  // 平行光（无限远）
    Point       = 1,  // 点光源
    Spot        = 2,  // 聚光灯
};

// --- 光源基类 ---
class LightComponent : public Component {
    HE_COMPONENT()
public:
    LightType type      = LightType::Directional;  // 光源类型
    bool      enabled   = true;                    // 是否启用
    float3    color     = float3(1.0f);            // 光照颜色（线性空间）
    float     intensity = 1.0f;                    // 强度（非物理模式下的无单位乘数）

    // --- 物理光照参数（0 = 使用传统 intensity 模式）---
    float     colorTemperature = 0.0f;               // 色温, K（0=直接使用 color, >0=色温→RGB × color 滤镜）
    float     luminousIntensity = 0.0f;              // 发光强度, cd（点光/聚光物理模式, 0=传统模式）
    float     illuminance = 0.0f;                    // 照度, lux（方向光物理模式, 0=传统模式）

    // --- 阴影参数 ---
    bool      castShadow       = false;            // 是否投射阴影
    u32       shadowMapSize    = kDefaultShadowMapSize;  // 阴影贴图分辨率
    float     shadowBias       = 0.005f;           // 深度偏移（防止阴影痤疮）
    float     shadowNormalBias = 0.02f;            // 法线偏移（防止自阴影）
    float     shadowStrength   = 1.0f;             // 阴影强度 [0,1]
};

// --- 平行光（无限远方向光）---
class DirectionalLight : public LightComponent {
    HE_COMPONENT()
public:
    void OnCreate() override;

    float3 direction = float3(0.5f, -1.0f, 0.5f);  // 世界空间光线方向（归一化）
};

// --- 点光源（全向辐射，随距离衰减）---
class PointLight : public LightComponent {
    HE_COMPONENT()
public:
    void OnCreate() override;

    float range = 1.0f;   // 影响范围
};

// --- 聚光灯（锥形辐射 + 角度衰减）---
class SpotLight : public LightComponent {
    HE_COMPONENT()
public:
    void OnCreate() override;

    float range           = 10.0f;                  // 影响范围
    float3 direction      = float3(0.0f, -1.0f, 0.0f);  // 局部空间锥轴方向
    float innerConeAngle  = 0.3f;                   // 内锥角（弧度，~17°）
    float outerConeAngle  = 0.6f;                   // 外锥角（弧度，~34°）
};

} // namespace he
