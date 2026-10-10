// RT_HitCommon.slang — RT 反射/GI 共享的命中评估逻辑
//
// 提供：
//   1. 场景材质纹理（2×N RGBA32F，ClosestHit 用 InstanceID() 查询）
//   2. 紧凑光源 cbuffer（ClosestHit 直接光照）
//   3. LoadRTHitMaterial / EvaluateHitRadiance 辅助函数
//
// 数据布局（与 C++ RTPass::BuildSceneMaterialTexture / 各 Pass 的 LightUB 对应）：
//   g_SceneMaterials(row0=albedo.rgb+metallic, row1=roughness+ao)
//   g_HitLights[16]（48B/个：colorIntensity + positionRange + directionType）
//
// 注意：材质用 Texture2D::Load（SampledImage）而非 SSBO——
//       已知 slangc 在 ClosestHitKHR 中访问 StructuredBuffer 会 GPU fault。

#include "HitCommon.slang"   // LoadHitNormal / SampleMaterialColor（零绑定，资源作参数）
#include "ShaderTypes.slang"  // GPULight 等共享结构
#include "pbr_common.slang"   // PBR_BRDF / F_Schlick（**零绑定**，故可安全引入本文件）

// === 场景材质纹理（binding 4, set0, ClosestHit 阶段）===
// 3×N RGBA32F：row0=albedo.rgb+metallic, row1=roughness+ao,
// row2=(法线线性起始=tri*3, 三角形数, 法线纹理宽度, 0)
[[vk::binding(4, 0)]] Texture2D<float4> g_SceneMaterials;

// === 三角形顶点法线纹理（binding 6, set0, ClosestHit 阶段）===
// 注：LoadHitNormal 已移入 HitCommon.slang（资源作参数）；本声明供各 rchit 调用点传入。
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
// LoadRTHitMaterial — 按 TLAS 实例 ID 查询命中材质
// 4×N 纹理：row0=albedo+metallic, row1=roughness+ao, row3=emissive
// ============================================================
void LoadRTHitMaterial(uint instanceID,
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
// EvaluateHitRadiance — 命中点**出射辐射度** L_o（环境项 + 直接光的一次弹射）
//
// 【量纲：必须是辐射度，不是辐照度】（§9.2-AC / 任务 33）
//   朗伯面的出射辐射度是 `albedo/π · E`（E 为入射辐照度）。此前本函数把环境项与直接光
//   直接相加后乘 albedo 就返回，即返回的是 `albedo · E` —— **比辐射度大 π 倍**。当时三处
//   调用点里只有 `DDGI_Trace.rgen` 在调用后除了 π，`RT_GI.rchit` 与 `RT_Reflection.rchit`
//   没有 ⇒ RTGI 与 RT 反射的绝对量级各大约 3.14 倍。现在函数自己返回辐射度，调用点一律
//   不再做换算（当前调用点就是这两个 rchit）。
//
// 【白炉（furnace）分支】白炉条件 = 全白环境（辐射度 1）+ albedo 1 + 关直接光，其解析真值
//   是"每个方向的入射辐射度都是 1" ⇒ E = π ⇒ L_o = albedo/π · π = albedo。
//   故白炉下环境项取 E = π（而不是那个艺术常数），直接光跳过 —— 这样**命中与未命中两条路径
//   都返回理想值**，白炉读数与场景几何无关，成为一条能看见绝对量级的判据。
//   **该分支是判据：任何时候都不得改动，改它等于把判据一起改掉。**
//
// 【2026-10：直接光改用共享 PBR_BRDF（行为变更）】
//   此前直接光只做朗伯（`albedo/π · Σ light·NdotL`），材质的 `metallic` / `roughness` 被读
//   出来却没用 ⇒ **同一材质在光栅路径（PBR_BRDF）与光追命中点下外观不同**（金属与低粗糙度
//   表面差异最明显）。现在逐光源调用 `pbr_common.slang` 的 `PBR_BRDF`（含 D/F/G、金属度、
//   clearcoat、各向异性等），环境项仍走朗伯但按 `kD = (1-F)(1-metallic)` 缩放 —— 因此
//   **金属不再获得漫反射**（此前金属也拿满 albedo/π·E，属物理错误）。
//   ⚠ 影响：RT 反射与 RTGI 的绝对亮度/外观会变化（金属、低粗糙度尤其明显）。
//   ⚠ 刻意保留、未标定的部分（要动需单独评估）：环境项是历史常数辐照度、点光衰减是
//     `(1-d/range)²` 艺术衰减、命中点没有可用的预滤波环境图（故没有镜面环境项）。
// ============================================================
// 本文件不包含 common.slang（RT 各阶段对包含顺序敏感），故 π 用局部常量
static const float kRT_PI = 3.14159265359;

// 未标定的历史环境辐照度（量纲是 E；由本函数统一乘 1/π 转成辐射度）
static const float3 kRT_AmbientIrradiance = float3(0.22, 0.23, 0.26);

float3 EvaluateHitRadiance(float3 P, float3 N, float3 V,
                           float3 albedo, float metallic, float roughness,
                           uint lightCount, bool furnace) {
    if (furnace) {
        // 全白环境：E_ambient = π（辐射度 1 的均匀环境），无直接光
        return albedo * (1.0 / kRT_PI) * kRT_PI;   // = albedo（写出换算过程，避免"魔法 1"）
    }

    // ── 环境项：恒定辐照度 → 朗伯漫反射 ──
    // kD 与 PBR_BRDF 的漫反射项同构（(1-F)(1-metallic)），金属因此不再有漫反射。
    float  NdotV = max(dot(N, V), 0.0);
    float3 f0    = lerp(float3(0.04), albedo, metallic);
    float3 kD    = (1.0 - F_Schlick(f0, NdotV)) * (1.0 - metallic);
    float3 Lo    = kD * albedo * (1.0 / kRT_PI) * kRT_AmbientIrradiance;

    // ── 直接光：共享 PBR_BRDF（返回值已含 cosθ_l，见 pbr_common 末尾 `return baseBRDF * NdotL`）──
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
        Lo += PBR_BRDF(albedo, metallic, roughness, N, V, L)
            * l.colorIntensity.rgb * l.colorIntensity.w * atten;
    }
    return Lo;
}
