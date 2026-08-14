#pragma once

#include "Scene/Component.h"
#include "Math/Math.h"

// ============================================================
// PhysicalSkyComponent — 物理天空组件（Preetham 大气散射模型）
//
// 提供太阳方向、浑浊度、地面反照率等参数，由 SkyboxPass 渲染解析天空。
// 用法: world.AddComponent<PhysicalSkyComponent>(entity);
// ============================================================

namespace he {

class PhysicalSkyComponent : public Component {
    HE_COMPONENT()
public:
    void OnCreate() override;

    float3 sunDirection = float3(0.0f, 0.6f, 0.4f);  // 太阳方向（世界空间，OnCreate 归一化）
    float  turbidity    = 4.0f;   // 大气浑浊度（1=极清，2=清，5=霾，10=浓霾）
    float  groundAlbedo = 0.1f;   // 地面反照率（0~1，影响天空亮度）
    float  intensity    = 1.0f;   // 天空整体亮度倍率
    float  sunIntensity = 1.0f;   // 太阳盘亮度倍率
    bool   enabled      = true;   // 是否渲染
};

} // namespace he
