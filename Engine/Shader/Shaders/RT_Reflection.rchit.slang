// RT_Reflection.rchit.slang — 反射射线 ClosestHit 着色器
//
// 评估命中表面的出射辐射度（diffuse 近似）：
//   position_fetch 获取顶点位置 → 面法线
//   → 场景材质纹理查询 albedo/metallic/roughness
//   → EvaluateHitRadiance（环境光 + 各直接光源漫反射）
//
// 注意：材质用 Texture2D（SampledImage）而非 SSBO——ClosestHit 中
// 访问 StructuredBuffer 已知 slangc GPU fault（见文档 P0 节）。

#include "RT_HitCommon.slang"  // 材质纹理 + 光源 cbuffer + EvaluateHitRadiance

struct ReflectionPayload {
    float4 colorT;  // rgb=反射颜色, a=命中距离
};

[[vk::push_constant]] RTRayEffectPushConstant g_PC;

[shader("closesthit")]
void main(inout ReflectionPayload payload, in BuiltInTriangleIntersectionAttributes attr) {
    uint instanceID = NonUniformResourceIndex(InstanceID());
    uint primId     = NonUniformResourceIndex(PrimitiveIndex());
    float3 P = WorldRayOrigin() + WorldRayDirection() * RayTCurrent();

    // ── 平滑法线：材质纹理行2 查线性起始 → 三角形法线纹理读 3 顶点法线 → 重心插值 ──
    // 不依赖 position_fetch（GTX 1070 等设备不支持 VK_KHR_ray_tracing_position_fetch）
    float3 N = LoadHitNormal(instanceID, primId, attr.barycentrics);

    // ── 命中材质查询 ──
    float3 albedo; float metallic; float roughness;
    LoadHitMaterial(instanceID, albedo, metallic, roughness);

    // ── 命中点出射辐射度（反射 = 命中表面被照亮的颜色）──
    float3 radiance = EvaluateHitRadiance(P, N, albedo, g_PC.lightCount);

    payload.colorT = float4(radiance, RayTCurrent());
}
