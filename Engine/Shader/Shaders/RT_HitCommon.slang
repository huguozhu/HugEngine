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
// EvaluateHitRadiance — 命中点**出射辐射度** L_o（环境项 + 直接光的一次反弹）
// 用于反射 / GI 的第一次反弹。
//
// 【量纲：必须是辐射度，不是辐照度】（§9.2-AC / 任务 33）
//   朗伯面的出射辐射度是 `albedo/π · E`（E 为入射辐照度）。此前本函数把环境项与直接光
//   直接相加后乘 albedo 就返回，即返回的是 `albedo · E` —— **比辐射度大 π 倍**。
//   三处调用点里只有 `DDGI_Trace.rgen` 在调用后除了 π（任务 17 修探针过亮时加的），
//   `RT_GI.rchit` 与 `RT_Reflection.rchit` 没有 ⇒ RTGI 与 RT 反射的绝对量级各大约 3.14 倍。
//   现在函数自己返回辐射度，调用点一律不再做换算。
//
// 【白炉（furnace）分支】白炉条件 = 全白环境（辐射度 1）+ albedo 1 + 关直接光，其解析真值
//   是"每个方向的入射辐射度都是 1" ⇒ E = π ⇒ L_o = albedo/π · π = albedo。
//   故白炉下环境项取 E = π（而不是那个艺术常数），直接光跳过 —— 这样**命中与未命中两条路径
//   都返回理想值**，白炉读数与场景几何无关，成为一条能看见绝对量级的判据
//   （改前经此路径会读到 albedo·π）。
// ============================================================
// 本文件不包含 common.slang（RT 各阶段对包含顺序敏感），故 π 用局部常量
static const float kRT_PI = 3.14159265359;

float3 EvaluateHitRadiance(float3 P, float3 N, float3 albedo, uint lightCount, bool furnace) {
    if (furnace) {
        // 全白环境：E_ambient = π（辐射度 1 的均匀环境），无直接光
        return albedo * (1.0 / kRT_PI) * kRT_PI;   // = albedo（写出换算过程，避免"魔法 1"）
    }

    // 简单环境光（与 RT_PBR.rchit 一致的半球近似）。**量纲是辐照度 E**（历史常数，
    // 只保证"看起来对"，没有物理标定），故下面统一乘 1/π 转成辐射度。
    float3 totalE = float3(0.22, 0.23, 0.26);

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
        totalE += l.colorIntensity.rgb * l.colorIntensity.w * ndl * atten;
    }
    // 辐照度 → 朗伯面出射辐射度
    return albedo * (1.0 / kRT_PI) * totalE;
}
