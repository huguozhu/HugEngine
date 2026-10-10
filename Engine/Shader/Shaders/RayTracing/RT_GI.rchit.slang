// RT_GI.rchit.slang — GI 射线 ClosestHit 着色器
//
// 评估命中表面的出射辐射度（一次反弹的间接漫反射来源）：
//   position_fetch 获取顶点位置 → 面法线
//   → 场景材质纹理查询 albedo
//   → EvaluateHitRadiance（环境光漫反射 + 各直接光源按 PBR_BRDF 求值）
//
// 与 RT_Reflection.rchit 逻辑一致，仅 Payload 类型不同。

#include "RT_HitCommon.slang"  // 材质纹理 + 光源 cbuffer + EvaluateHitRadiance

#include "RTPayloads.slang"   // ShadowPayload / RayPayload / GIPayload / ReflectionPayload（唯一出处）

[[vk::push_constant]] RTRayEffectPushConstant g_PC;

[shader("closesthit")]
void main(inout GIPayload payload, in BuiltInTriangleIntersectionAttributes attr) {
    uint instanceID = NonUniformResourceIndex(InstanceID());
    uint primId     = NonUniformResourceIndex(PrimitiveIndex());
    float3 P = WorldRayOrigin() + WorldRayDirection() * RayTCurrent();

    // ── 平滑法线（纹理查询 + 重心插值，不依赖 position_fetch）──
    float3 N = LoadHitNormal(g_SceneMaterials, g_TriangleNormals, instanceID, primId, attr.barycentrics);

    // ── 命中材质查询（emissive 供后续扩展，间接光近似暂不使用）──
    float3 albedo; float metallic; float roughness; float3 emissive;
    LoadRTHitMaterial(instanceID, albedo, metallic, roughness, emissive);

    // ── 命中点出射辐射度（作为间接漫反射来源）──
    // V = 表面 → 视点（入射光线反向），供 PBR_BRDF 的菲涅耳/几何项使用
    float3 V = -normalize(WorldRayDirection());
    float3 radiance = EvaluateHitRadiance(P, N, V, albedo, metallic, roughness,
                                          g_PC.lightCount, (g_PC.flags & 4u) != 0u);

    payload.radianceT = float4(radiance, RayTCurrent());
}
