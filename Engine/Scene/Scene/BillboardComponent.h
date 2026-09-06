#pragma once

#include "Scene/MeshComponent.h"
#include "Math/Math.h"

// ============================================================
// BillboardComponent — 广告牌（对应 UE5 UBillboardComponent）
//
// 始终面向相机的四边形（粒子替代、UI 指示、调试标记）。
// 继承 MeshComponent：OnCreate 生成 1×1 单位四边形（XY 平面，法线 +Z），
// 默认无光照 + 半透明混合 + 双面渲染。
//
// 渲染接入（SceneRenderer::Prepare / GPUScene::Collect）：
// 每帧用 MakeBillboardMatrix 把四边形旋转对齐相机（相机右/上轴 + 朝向相机）。
// 纹理：baseColorTexture 赋路径 + materialID 指向 bindless 槽（与普通 Mesh 一致）。
// ============================================================

namespace he {

class BillboardComponent : public MeshComponent {
    HE_COMPONENT()
public:
    /// 生成单位四边形网格（顶点 UV 左下角为 (0,0)，+Y 为上）
    void OnCreate() override;

    float2 size = float2(1.0f, 1.0f);   // 广告牌尺寸（世界单位，X=右，Y=上）

    /// 计算对齐相机的 billboard 矩阵（纯函数，可单测）：
    /// 列 0 = 相机右 × size.x，列 1 = 相机上 × size.y，
    /// 列 2 = 朝向相机（−forward），列 3 = position。
    static float4x4 MakeBillboardMatrix(const float3& position,
                                        const float3& cameraForward,
                                        const float3& cameraUp,
                                        const float2& size);
};

} // namespace he
