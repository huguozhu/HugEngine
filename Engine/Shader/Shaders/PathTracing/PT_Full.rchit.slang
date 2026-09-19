// PT_Full.rchit.slang — 全路径追踪 ClosestHit 着色器
//
// 填充 PathPayload（112B）：albedo+metallic / normal+roughness / emissive+hitT，
// Disney 材质参数（disneyA / disneyB / surfaceParams）与介质吸收系数（volumeParams），
// 供 RayGen 的 BRDF 求值、BSDF 采样与 Beer-Lambert 衰减使用。
// 材质用 Texture2D（SampledImage）而非 SSBO——ClosestHitKHR 中访问
// StructuredBuffer 已知 slangc GPU fault；法线用三角形法线纹理重心插值，
// 不依赖 position_fetch（GTX 1070 等设备不支持）。
//
// 注意：本着色器自带绑定声明与材质查询函数副本（RT_HitCommon.slang 的
// 绑定为 4/5/6，而 PTPass set0 布局为 b5=光源 SSBO(RayGen)/b6=材质/b7=法线，
// 无法直接 include）。

#include "PT_Common.slang"  // PathPayload + 随机数（SampleSky 等）

// === 材质资源（set=0, ClosestHit 阶段）===
// 11×N RGBA32F（与 RTPass::BuildSceneMaterialTexture 的行布局一致）：
//   row0=albedo.rgb+metallic, row1=roughness+ao,
//   row2=(法线线性起始=tri*3, 三角形数, 法线纹理宽度, 0), row3=emissive.rgb+0,
//   row4=disneyA(aniso, subsurface, specular, sheen),
//   row5=disneyB(clearcoat, clearcoatGloss, specularTint.rg),
//   row6=(disneyC, dielectricF0, ior, transmission),
//   row7=(σ_t.rgb, 0) 介质吸收系数,
//   row8=(materialID, textureMask, 0, 0),
//   row9=(baseColorFactor.rgb, metallicFactor), row10=(roughnessFactor, 0, 0, 0)
[[vk::binding(6, 0)]] Texture2D<float4> g_SceneMaterials;

// 三角形顶点法线纹理（每三角形 3 条，按 TLAS 实例顺序扁平排列）
[[vk::binding(7, 0)]] Texture2D<float4> g_TriangleNormals;

// 三角形顶点 UV 纹理（RG32F，布局与法线纹理一致）：命中点 UV 由重心坐标插值得到
[[vk::binding(11, 0)]] Texture2D<float2> g_TriangleUVs;

// bindless 材质贴图：texBase = row8.x（materialID），槽位 = texBase + kGPUMaterialTexSlot_*
// （与光栅化路径 PBR.frag/GBuffer.frag 完全同一套槽位约定与 textureMask 判据）
// 命名与 common.slang 的 u_Textures/u_Samplers 区分：本文件经 PT_Common.slang 间接 include 了
// common.slang，那里的同名数组声明在 set0 的 5/6（PT 这边 5/6 是光源 SSBO 与材质纹理），
// 同名会在 Slang 里报 conflicting declaration / ambiguous reference。
[[vk::binding(12, 0)]] Texture2D<float4> g_MaterialTextures[];
[[vk::binding(13, 0)]] SamplerState      g_MaterialSamplers[];

// STBN 时空蓝噪声（PT_Common 随机函数体引用，本 stage 未用但需声明以通过编译）
[[vk::binding(10, 0)]] Texture3D<float4> g_BlueNoise;

// === Push Constant（与 RayGen 共享，176B）===
[[vk::push_constant]] PTPushConstant g_PC;

// PT flags（与 PathTracingPipeline.cpp 的 kPTFlag_* 一致）
#define PT_FLAG_TEXTURES (1u << 4)   // 采样材质贴图（r.PT.Textures；关闭 = 只用均值/因子）

// ============================================================
// LoadHitMaterial — 按 TLAS 实例 ID 查询命中材质（11×N 纹理）
// 同时取出 bindless 贴图基索引与纹理存在位掩码（row8）
// ============================================================
void LoadHitMaterial(uint instanceID,
                     out float3 albedo,
                     out float metallic,
                     out float roughness,
                     out float3 emissive,
                     out float4 disneyA,
                     out float4 disneyB,
                     out float4 surfaceParams,
                     out float4 volumeParams,
                     out uint   texBase,
                     out uint   texMask) {
    float4 row0 = g_SceneMaterials.Load(int3(instanceID, 0, 0));
    albedo    = row0.rgb;
    metallic  = row0.a;
    roughness = g_SceneMaterials.Load(int3(instanceID, 1, 0)).r;
    emissive  = g_SceneMaterials.Load(int3(instanceID, 3, 0)).rgb;
    // Disney 参数（row4/row5）与表面参数（row6：disneyC / F0 / IOR / 透射）
    disneyA       = g_SceneMaterials.Load(int3(instanceID, 4, 0));
    disneyB       = g_SceneMaterials.Load(int3(instanceID, 5, 0));
    surfaceParams = g_SceneMaterials.Load(int3(instanceID, 6, 0));
    volumeParams  = g_SceneMaterials.Load(int3(instanceID, 7, 0));
    float4 row8   = g_SceneMaterials.Load(int3(instanceID, 8, 0));
    texBase = uint(row8.x + 0.5);   // 整数按 float 存，四舍五入还原
    texMask = uint(row8.y + 0.5);
}

// ============================================================
// LoadHitUV — 按实例 + 三角形 + 重心坐标插值出命中点 UV
// （与法线用同一份「每三角形 3 条顶点属性」布局，uv 纹理宽度同样取自 row2.z）
// ============================================================
float2 LoadHitUV(uint instanceID, uint primId, float2 bary) {
    float4 row2 = g_SceneMaterials.Load(int3(instanceID, 2, 0));
    uint linBase = uint(row2.x);
    uint texW    = uint(row2.z);

    float2 uv0 = g_TriangleUVs.Load(int3((linBase + primId * 3 + 0) % texW,
                                         (linBase + primId * 3 + 0) / texW, 0));
    float2 uv1 = g_TriangleUVs.Load(int3((linBase + primId * 3 + 1) % texW,
                                         (linBase + primId * 3 + 1) / texW, 0));
    float2 uv2 = g_TriangleUVs.Load(int3((linBase + primId * 3 + 2) % texW,
                                         (linBase + primId * 3 + 2) / texW, 0));
    return uv0 * (1.0 - bary.x - bary.y) + uv1 * bary.x + uv2 * bary.y;
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
    float4 disneyA; float4 disneyB; float4 surfaceParams; float4 volumeParams;
    uint   texBase, texMask;
    LoadHitMaterial(instanceID, albedo, metallic, roughness, emissive,
                    disneyA, disneyB, surfaceParams, volumeParams, texBase, texMask);
    float3 N = LoadHitNormal(instanceID, primId, attr.barycentrics);

    // ── 贴图采样（PT 贴图落地）──
    // 与光栅化路径同一套约定：slot = texBase + kGPUMaterialTexSlot_*，按 textureMask 判据；
    // 有贴图时用「因子 × 贴图」（row0/row1 存的是贴图均值 × 因子，只在无贴图时回落，
    // 直接乘贴图会把贴图算两遍）。r.PT.Textures 关闭时整段跳过，退回平色着色便于对照。
    if (texMask != 0u && (g_PC.flags & PT_FLAG_TEXTURES) != 0u) {
        float2 uv = LoadHitUV(instanceID, primId, attr.barycentrics);

        if ((texMask & kGPUMaterialTexMask_BaseColor) != 0u) {
            uint idx = NonUniformResourceIndex(texBase + kGPUMaterialTexSlot_BaseColor);
            float4 factor = g_SceneMaterials.Load(int3(instanceID, 9, 0));
            albedo = factor.rgb * g_MaterialTextures[idx].SampleLevel(g_MaterialSamplers[idx], uv, 0.0).rgb;
        }
        if ((texMask & kGPUMaterialTexMask_MetallicRough) != 0u) {
            uint idx = NonUniformResourceIndex(texBase + kGPUMaterialTexSlot_MetallicRough);
            float4 factor = g_SceneMaterials.Load(int3(instanceID, 9, 0));   // a = metallicFactor
            float  roughF = g_SceneMaterials.Load(int3(instanceID, 10, 0)).x;
            float4 mr = g_MaterialTextures[idx].SampleLevel(g_MaterialSamplers[idx], uv, 0.0);
            metallic  = factor.a * mr.b;
            roughness = clamp(roughF * mr.g, 0.04, 1.0);
        }
        // 法线贴图：与光栅化路径同一份近似（切线空间法线叠加到插值法线上）。
        // PT 没有逐顶点切线（顶点拉取缓冲未绑定到本 stage），故不构造完整 TBN。
        if ((texMask & kGPUMaterialTexMask_Normal) != 0u) {
            uint idx = NonUniformResourceIndex(texBase + kGPUMaterialTexSlot_Normal);
            float3 tn = g_MaterialTextures[idx].SampleLevel(g_MaterialSamplers[idx], uv, 0.0).rgb * 2.0 - 1.0;
            N = normalize(N + tn * 0.5);
        }
        // 遮蔽贴图（occlusion）：路径追踪里 AO 是求解量而非贴图项，
        // 与 PBR.frag 不同、这里不参与着色（row1.y 的 aoFactor 保留未用）。
    }

    payload.albedoMetallic = float4(albedo, metallic);
    payload.normalRough    = float4(N, roughness);
    payload.emissiveT      = float4(emissive, RayTCurrent());
    // Disney 材质参数随载荷带回 RayGen（PT 任务 1：为共享 BRDF 求值/采样准备数据；
    // 任务 2/3 才真正在采样端使用，故此刻读数与扩展前一致）
    payload.disneyA        = disneyA;
    payload.disneyB        = disneyB;
    payload.surfaceParams  = surfaceParams;
    payload.volumeParams   = volumeParams;   // σ_t（PT 任务 4：Beer-Lambert 吸收）
}
