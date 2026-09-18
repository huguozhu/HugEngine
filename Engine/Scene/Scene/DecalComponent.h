#pragma once

#include "Scene/MeshComponent.h"
#include "Math/Math.h"

// ============================================================
// DecalComponent — 贴花（对应 UE5 UDecalComponent）
//
// 用途：弹孔/污渍/路面标线等贴附在场景表面的纹理片。
//
// 两条渲染路径：
//   · **Deferred（任务 24）：GBuffer 投影贴花**（DecalPass）—— 贴花当投影体积投到 GBuffer 上，
//     片段着色器按 GBuffer 世界坐标把盒子裁剪到真实表面，曲面/台阶也能贴合；
//     `projectionDepth` 决定投影体积沿贴花轴的厚度。
//   · Forward：半透明投射片（原 MVP）—— 四边形网格 + Blend 混合，靠 Transform 摆放贴合表面，
//     平的地方可用，曲面/斜坡会穿模（Forward 没有 GBuffer 可投影，见文档"已知边界"）。
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
    /// 投影体积厚度（世界单位，沿贴花 +Z 轴，任务 24 的 Deferred 投影路径使用）。
    /// 【怎么选】贴花盒必须**包住**要贴的表面：放在地面上的贴花，中心离地面 z 偏移多少，
    /// 厚度就要 ≥ 2×偏移（默认 0.5m 足够覆盖常见的 0.0~0.25m 抬高）。
    float  projectionDepth = 0.5f;
    u8     blendMode = 2;                   // AlphaMode：0=Opaque 1=Mask 2=Blend（默认混合）
    String decalTexture;                    // 贴花纹理路径（空 = 纯色片）
};

} // namespace he
