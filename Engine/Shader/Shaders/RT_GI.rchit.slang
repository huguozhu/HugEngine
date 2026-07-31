// RT_GI.rchit.slang — GI 射线 ClosestHit 着色器
//
// 评估命中表面的出射辐射度（一次反弹的间接漫反射来源）：
//   position_fetch 获取顶点位置 → 面法线
//   → 场景材质纹理查询 albedo
//   → EvaluateHitRadiance（环境光 + 各直接光源漫反射）
//
// 与 RT_Reflection.rchit 逻辑一致，仅 Payload 类型不同。

#include "RT_HitCommon.slang"  // 材质纹理 + 光源 cbuffer + EvaluateHitRadiance

struct GIPayload {
    float4 radianceT;  // rgb=辐射度, a=命中距离
};

[[vk::push_constant]] RTRayEffectPushConstant g_PC;

[shader("closesthit")]
void main(inout GIPayload payload, in BuiltInTriangleIntersectionAttributes attr) {
    uint instanceID = NonUniformResourceIndex(InstanceID());
    uint primId     = NonUniformResourceIndex(PrimitiveIndex());
    float3 P = WorldRayOrigin() + WorldRayDirection() * RayTCurrent();

    // ── 平滑法线（纹理查询 + 重心插值，不依赖 position_fetch）──
    float3 N = LoadHitNormal(instanceID, primId, attr.barycentrics);

    // ── 命中材质查询 ──
    float3 albedo; float metallic; float roughness;
    LoadHitMaterial(instanceID, albedo, metallic, roughness);

    // ── 命中点出射辐射度（作为间接漫反射来源）──
    float3 radiance = EvaluateHitRadiance(P, N, albedo, g_PC.lightCount);

    payload.radianceT = float4(radiance, RayTCurrent());
}
