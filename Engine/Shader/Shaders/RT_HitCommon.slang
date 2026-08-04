// RT_HitCommon.slang — RT 反射/GI 共享的命中评估逻辑
//
// 提供：
//   1. 场景材质纹理（2×N RGBA32F，ClosestHit 用 InstanceID() 查询）
//   2. 紧凑光源 cbuffer（ClosestHit 直接光照）
//   3. LoadHitMaterial / EvaluateHitRadiance 辅助函数
//
// 数据布局（与 C++ RTPass::BuildSceneMaterialTexture / 各 Pass 的 LightUB 对应）：
//   g_SceneMaterials(row0=albedo.rgb+metallic, row1=roughness+ao)
//   g_HitLights[16]（48B/个：colorIntensity + positionRange + directionType）
//
// 注意：材质用 Texture2D::Load（SampledImage）而非 SSBO——
//       已知 slangc 在 ClosestHitKHR 中访问 StructuredBuffer 会 GPU fault。

#include "ShaderTypes.slang"  // GPULight 等共享结构

// === 场景材质纹理（binding 4, set0, ClosestHit 阶段）===
// 3×N RGBA32F：row0=albedo.rgb+metallic, row1=roughness+ao,
// row2=(法线线性起始=tri*3, 三角形数, 法线纹理宽度, 0)
[[vk::binding(4, 0)]] Texture2D<float4> g_SceneMaterials;

// === 三角形顶点法线纹理（binding 6, set0, ClosestHit 阶段）===
// 每三角形 3 条顶点法线，按 TLAS 实例顺序扁平排列；
// 线性索引 lin = row2.x + primId*3 + v → (col=lin%row2.z, row=lin/row2.z)
[[vk::binding(6, 0)]] Texture2D<float4> g_TriangleNormals;

// === 命中点光源 cbuffer（binding 5, set0, ClosestHit 阶段）===
// 48B/个，由 C++ 每帧从 GPULight[] SSBO 显式抽取（避免 ClosestHit 访问 SSBO）
struct RTHitLight {
    float4 colorIntensity;   // rgb=颜色, w=强度
    float4 positionRange;    // xyz=位置, w=范围
    float4 directionType;    // xyz=方向, w=类型 (0=Dir,1=Point,2=Spot)
};
[[vk::binding(5, 0)]] cbuffer RTHitLightCB {
    RTHitLight g_HitLights[16];
};

// ============================================================
// LoadHitMaterial — 按 TLAS 实例 ID 查询命中材质
// 4×N 纹理：row0=albedo+metallic, row1=roughness+ao, row3=emissive
// ============================================================
void LoadHitMaterial(uint instanceID,
                     out float3 albedo,
                     out float metallic,
                     out float roughness,
                     out float3 emissive) {
    float4 row0 = g_SceneMaterials.Load(int3(instanceID, 0, 0));
    albedo    = row0.rgb;
    metallic  = row0.a;
    roughness = g_SceneMaterials.Load(int3(instanceID, 1, 0)).r;
    emissive  = g_SceneMaterials.Load(int3(instanceID, 3, 0)).rgb;
}

// ============================================================
// LoadHitNormal — 按实例 + 三角形 + 重心坐标查询平滑法线
// 不依赖 position_fetch（GTX 1070 等设备不支持该扩展），
// 重心坐标来自 closesthit 的 BuiltInTriangleIntersectionAttributes
// ============================================================
float3 LoadHitNormal(uint instanceID, uint primId, float2 bary) {
    float4 row2 = g_SceneMaterials.Load(int3(instanceID, 2, 0));
    uint linBase = uint(row2.x);      // 本实例法线数组线性起始
    uint texW    = uint(row2.z);      // 法线纹理宽度（lin%W → 列）

    // 读取三角形 3 个顶点法线
    float3 n0 = g_TriangleNormals.Load(int3((linBase + primId * 3 + 0) % texW,
                                             (linBase + primId * 3 + 0) / texW, 0)).xyz;
    float3 n1 = g_TriangleNormals.Load(int3((linBase + primId * 3 + 1) % texW,
                                             (linBase + primId * 3 + 1) / texW, 0)).xyz;
    float3 n2 = g_TriangleNormals.Load(int3((linBase + primId * 3 + 2) % texW,
                                             (linBase + primId * 3 + 2) / texW, 0)).xyz;

    // 重心插值（顶点 0 权重 = 1 - x - y）
    float3 N = normalize(n0 * (1.0 - bary.x - bary.y) + n1 * bary.x + n2 * bary.y);

    // 确保法线朝向光线入射方向（背面命中 → 翻转）
    if (dot(N, -WorldRayDirection()) < 0.0) N = -N;
    return N;
}

// ============================================================
// EvaluateHitRadiance — 评估命中点直接光照（diffuse 近似 + 环境光）
// 用于反射/ GI 的第一次反弹：命中表面的出射辐射度
// ============================================================
float3 EvaluateHitRadiance(float3 P, float3 N, float3 albedo, uint lightCount) {
    // 简单环境光（与 RT_PBR.rchit 一致的半球近似）
    float3 total = float3(0.22, 0.23, 0.26) * albedo;

    for (uint i = 0; i < min(lightCount, 16u); i++) {
        RTHitLight l = g_HitLights[i];
        if (l.colorIntensity.w <= 0.0) continue;  // 跳过无效光源

        float3 L;
        float atten = 1.0;
        if (l.directionType.w == 0.0) {
            // 方向光
            L = normalize(l.directionType.xyz);
        } else {
            // 点光源 / 聚光灯：方向指向光源，距离衰减
            float3 toLight = l.positionRange.xyz - P;
            float dist = length(toLight);
            L = toLight / max(dist, 1e-5);
            float range = abs(l.positionRange.w);
            if (range > 0.0) {
                float d = saturate(1.0 - dist / range);
                atten = d * d;
            }
        }

        float ndl = max(dot(N, L), 0.0);
        total += l.colorIntensity.rgb * l.colorIntensity.w * ndl * atten;
    }
    return total;
}
