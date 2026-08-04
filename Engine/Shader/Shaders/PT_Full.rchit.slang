// PT_Full.rchit.slang — 全路径追踪 ClosestHit 着色器
//
// 填充 PathPayload（48B）：albedo+metallic / normal+roughness / emissive+hitT。
// 材质用 Texture2D（SampledImage）而非 SSBO——ClosestHitKHR 中访问
// StructuredBuffer 已知 slangc GPU fault；法线用三角形法线纹理重心插值，
// 不依赖 position_fetch（GTX 1070 等设备不支持）。
//
// 注意：本着色器自带绑定声明与材质查询函数副本（RT_HitCommon.slang 的
// 绑定为 4/5/6，而 PTPass set0 布局为 b5=光源 SSBO(RayGen)/b6=材质/b7=法线，
// 无法直接 include）。

#include "PT_Common.slang"  // PathPayload + 随机数（SampleSky 等）

// === 材质资源（set=0, ClosestHit 阶段）===
// 4×N RGBA32F：row0=albedo.rgb+metallic, row1=roughness+ao,
// row2=(法线线性起始=tri*3, 三角形数, 法线纹理宽度, 0), row3=emissive.rgb+0
[[vk::binding(6, 0)]] Texture2D<float4> g_SceneMaterials;

// 三角形顶点法线纹理（每三角形 3 条，按 TLAS 实例顺序扁平排列）
[[vk::binding(7, 0)]] Texture2D<float4> g_TriangleNormals;

// === Push Constant（与 RayGen 共享，176B）===
[[vk::push_constant]] PTPushConstant g_PC;

// ============================================================
// LoadHitMaterial — 按 TLAS 实例 ID 查询命中材质（4×N 纹理）
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
// 重心坐标来自 closesthit 的 BuiltInTriangleIntersectionAttributes
// ============================================================
float3 LoadHitNormal(uint instanceID, uint primId, float2 bary) {
    float4 row2 = g_SceneMaterials.Load(int3(instanceID, 2, 0));
    uint linBase = uint(row2.x);      // 本实例法线数组线性起始
    uint texW    = uint(row2.z);      // 法线纹理宽度（lin%W → 列）

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
// 主入口：ClosestHit
// ============================================================
[shader("closesthit")]
void main(inout PathPayload payload, in BuiltInTriangleIntersectionAttributes attr) {
    uint instanceID = NonUniformResourceIndex(InstanceID());
    uint primId     = NonUniformResourceIndex(PrimitiveIndex());

    float3 albedo; float metallic; float roughness; float3 emissive;
    LoadHitMaterial(instanceID, albedo, metallic, roughness, emissive);
    float3 N = LoadHitNormal(instanceID, primId, attr.barycentrics);

    payload.albedoMetallic = float4(albedo, metallic);
    payload.normalRough    = float4(N, roughness);
    payload.emissiveT      = float4(emissive, RayTCurrent());
}
