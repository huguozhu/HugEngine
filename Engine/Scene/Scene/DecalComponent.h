#pragma once

#include "Scene/MeshComponent.h"
#include "Math/Math.h"

// ============================================================
// DecalComponent — 贴花（对应 UE5 UDecalComponent）
//
// 用途：弹孔/污渍/路面标线等贴附在场景表面的纹理片。
// MVP 实现 = 半透明投射片（计划中的简化版）：四边形网格 +
// Blend 混合，靠 Transform 摆放/贴合表面，不做深度投影 Pass
// （真正的 GBuffer 投影贴花归 Deferred 管线后续扩展）。
//
// 纹理：decalTexture 赋路径（与 Mesh 一致的 bindless 流程，
// materialID 由调用方注册后填入）；空路径 = 纯色片。
// ============================================================

namespace he {

class DecalComponent : public MeshComponent {
    HE_COMPONENT()
public:
    /// 生成四边形网格（XY 平面，法线 +Z；按 rotation/size 构建）
    void OnCreate() override;

    // --- 参数 ---
    float2 size     = float2(1.0f, 1.0f);   // 贴花尺寸（世界单位，X=宽，Y=高）
    float  rotation = 0.0f;                 // 绕法线（+Z）旋转（弧度）
    float  opacity  = 1.0f;                 // 不透明度 [0,1]（写入 baseColorFactor.w）
    u8     blendMode = 2;                   // AlphaMode：0=Opaque 1=Mask 2=Blend（默认混合）
    String decalTexture;                    // 贴花纹理路径（空 = 纯色片）
};

} // namespace he
