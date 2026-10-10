// RT_Reflection.rchit.slang — 反射射线 ClosestHit 着色器
//
// 评估命中表面的出射辐射度（diffuse 近似）：
//   position_fetch 获取顶点位置 → 面法线
//   → 场景材质纹理查询 albedo/metallic/roughness
//   → EvaluateHitRadiance（环境光漫反射 + 各直接光源按 PBR_BRDF 求值）
//
// 注意：材质用 Texture2D（SampledImage）而非 SSBO——ClosestHit 中
// 访问 StructuredBuffer 已知 slangc GPU fault（见文档 P0 节）。

#include "RT_HitCommon.slang"  // 材质纹理 + 光源 cbuffer + EvaluateHitRadiance

#include "RTPayloads.slang"   // ShadowPayload / RayPayload / GIPayload / ReflectionPayload（唯一出处）

[[vk::push_constant]] RTRayEffectPushConstant g_PC;

[shader("closesthit")]
void main(inout ReflectionPayload payload, in BuiltInTriangleIntersectionAttributes attr) {
    uint instanceID = NonUniformResourceIndex(InstanceID());
    uint primId     = NonUniformResourceIndex(PrimitiveIndex());
    float3 P = WorldRayOrigin() + WorldRayDirection() * RayTCurrent();

    // ── 平滑法线：材质纹理行2 查线性起始 → 三角形法线纹理读 3 顶点法线 → 重心插值 ──
    // 不依赖 position_fetch（GTX 1070 等设备不支持 VK_KHR_ray_tracing_position_fetch）
    float3 N = LoadHitNormal(g_SceneMaterials, g_TriangleNormals, instanceID, primId, attr.barycentrics);

    // ── 命中材质查询（emissive 供后续扩展，反射近似暂不使用）──
    float3 albedo; float metallic; float roughness; float3 emissive;
    LoadRTHitMaterial(instanceID, albedo, metallic, roughness, emissive);

    // ── 命中点出射辐射度（反射 = 命中表面被照亮的颜色）──
    // V = 表面 → 视点（入射光线反向），供 PBR_BRDF 的菲涅耳/几何项使用
    float3 V = -normalize(WorldRayDirection());
    float3 radiance = EvaluateHitRadiance(P, N, V, albedo, metallic, roughness,
                                          g_PC.lightCount, (g_PC.flags & 4u) != 0u);

    payload.colorT = float4(radiance, RayTCurrent());
}
