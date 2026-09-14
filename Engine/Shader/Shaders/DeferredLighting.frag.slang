// DeferredLighting.frag — 延迟光照全屏片段着色器
// 读取 GBuffer MRT + Shadow/IBL/RSM 纹理，逐光源 PBR + 间接光
#include "pbr_common.slang"
// DDGI 探针网格参数（由 GI_DDGI 每帧上传，替代 RT_DDGI.slang 硬编码常量；
// 布局与 GI_DDGI.h 的 ProbeGridUniform 前两个 float4 一致）
[[vk::binding(kGPUBinding_DDGIGridParams, kGPUDescSet_PerFrame)]] cbuffer DDGIGridParams {
    float4 u_DDGIGridOrigin;  // xyz=世界空间原点
    float4 u_DDGIGridSize;    // xyz=探针数量(gridX/gridY/gridZ), w=cellSize
};
#include "RT_DDGI.slang"  // DDGI 探针查询共享库（EvalDDGI_SH + SampleDDGI）
#include "Atmosphere.slang"  // 空中透视（大气消光 + 入射散射）

struct VSOutput {
    float4 position          : SV_Position;
    [[vk::location(0)]] float2 uv;
};

// GBuffer 纹理
[[vk::binding(kGPUBinding_GBufferA, kGPUDescSet_PerFrame)]] Texture2D<float4> u_GBufferA;  // albedo.rgb + metallic
[[vk::binding(kGPUBinding_GBufferA, kGPUDescSet_PerFrame)]] SamplerState      u_GBufferSampler;
[[vk::binding(kGPUBinding_GBufferB, kGPUDescSet_PerFrame)]] Texture2D<float4> u_GBufferB;  // normal*0.5+0.5 + roughness
[[vk::binding(kGPUBinding_GBufferC, kGPUDescSet_PerFrame)]] Texture2D<float4> u_GBufferC;  // emissive.rgb + ao
[[vk::binding(kGPUBinding_Depth, kGPUDescSet_PerFrame)]] Texture2D<float>  u_Depth;
[[vk::binding(kGPUBinding_GBufferE, kGPUDescSet_PerFrame)]] Texture2D<float4> u_GBufferE;  // worldPos.xyz（GBuffer MRT4 直接读取）
[[vk::binding(kGPUBinding_GBufferF, kGPUDescSet_PerFrame)]] Texture2D<float4> u_GBufferF;  // disneyA：anisotropic, subsurface, specular, sheen（MRT5）
[[vk::binding(kGPUBinding_GBufferG, kGPUDescSet_PerFrame)]] Texture2D<float4> u_GBufferG;  // disneyB：clearcoat, clearcoatGloss, specularTint.r, specularTint.g（MRT6）

// Shadow + IBL + RSM (复用 Forward PBR 绑定)
[[vk::binding(kGPUBinding_ShadowMap0, kGPUDescSet_PerFrame)]] Texture2D<float>  u_ShadowMap0;
[[vk::binding(kGPUBinding_ShadowMap0, kGPUDescSet_PerFrame)]] SamplerState      u_ShadowSampler;
[[vk::binding(kGPUBinding_SpotShadow_DL, kGPUDescSet_PerFrame)]] Texture2D<float>  u_SpotShadowMap;  // 聚光灯阴影贴图（Deferred 专用 binding 9）
[[vk::binding(kGPUBinding_ShadowMap1, kGPUDescSet_PerFrame)]] Texture2D<float> u_ShadowMap1;
[[vk::binding(kGPUBinding_ShadowMap2, kGPUDescSet_PerFrame)]] Texture2D<float> u_ShadowMap2;
[[vk::binding(kGPUBinding_IrradianceMap, kGPUDescSet_PerFrame)]] TextureCube<float4> u_IrradianceMap;
[[vk::binding(kGPUBinding_IrradianceMap, kGPUDescSet_PerFrame)]] SamplerState        u_IBLSampler;
[[vk::binding(kGPUBinding_PrefilterMap, kGPUDescSet_PerFrame)]] TextureCube<float4> u_PrefilterMap;
[[vk::binding(kGPUBinding_BRDF_LUT, kGPUDescSet_PerFrame)]] Texture2D<float2>   u_BRDF_LUT;
[[vk::binding(kGPUBinding_RSMPosition, kGPUDescSet_PerFrame)]] Texture2D<float4>   u_RSMPositionMap;
[[vk::binding(kGPUBinding_RSMFlux, kGPUDescSet_PerFrame)]] Texture2D<float4>   u_RSMFluxMap;

// GPULight / GPUShadowData 定义来自 ShaderTypes.slang（C++/Slang 共享）

[[vk::binding(kGPUBinding_Lights_DL, kGPUDescSet_PerFrame)]] StructuredBuffer<GPULight>      u_Lights;
[[vk::binding(kGPUBinding_ShadowData_DL, kGPUDescSet_PerFrame)]] StructuredBuffer<GPUShadowData> u_ShadowData;
[[vk::binding(kGPUBinding_SSGI, kGPUDescSet_PerFrame)]] Texture2D<float4> u_SSGI;  // SSGI 间接光照
[[vk::binding(kGPUBinding_SSAO_DL, kGPUDescSet_PerFrame)]] Texture2D<float4> u_SSAO;  // SSAO 环境光遮蔽
[[vk::binding(kGPUBinding_SSR, kGPUDescSet_PerFrame)]] Texture2D<float4> u_SSR;   // SSR 屏幕空间反射
[[vk::binding(kGPUBinding_DDGIProbes, kGPUDescSet_PerFrame)]] StructuredBuffer<float4> u_DDGIProbes;  // DDGI 探针数据（每探针 16 float4）

// Hybrid RT 效果输出纹理（由 HybridRTPipeline 通过 LightingPass 绑定）
// 通过 push constant 的 rt*Source 字段在 shader 内选择 RT 或屏幕空间源
[[vk::binding(kGPUBinding_RT_ShadowMask, kGPUDescSet_PerFrame)]] Texture2D<float>  u_RT_ShadowMask;  // RT 阴影遮罩（0=全影, 1=无影）
[[vk::binding(kGPUBinding_RT_Reflection, kGPUDescSet_PerFrame)]] Texture2D<float4> u_RT_Reflection;  // RT 反射（rgb=反射色, a=有效标记）
[[vk::binding(kGPUBinding_RT_AO, kGPUDescSet_PerFrame)]] Texture2D<float>  u_RT_AO;          // RT 环境光遮蔽（0=全遮蔽, 1=无遮蔽）
[[vk::binding(kGPUBinding_RT_GI, kGPUDescSet_PerFrame)]] Texture2D<float4> u_RT_GI;          // RT 间接漫反射（rgb=间接光, a=有效标记）

// Clustered Shading: LightGrid + LightIndexList（每 cluster 只迭代影响它的光源）
struct LightGridCell { uint offset; uint count; };
[[vk::binding(kGPUBinding_LightGrid, kGPUDescSet_PerFrame)]]  StructuredBuffer<LightGridCell> u_LightGrid;       // 每 cluster 的光源列表头
[[vk::binding(kGPUBinding_LightIndexList, kGPUDescSet_PerFrame)]]  StructuredBuffer<uint>          u_LightIndexList;  // 扁平光源索引数组

// Cluster 网格参数（push constant 传入）
// worldPos 由 GBuffer MRT4 直接读取，无需 Camera invViewProj
[[vk::push_constant]] cbuffer LightingPC {
    float4   cameraPosition;
    uint     lightCount;
    float    iblIntensity;
    uint     clusterTilesX;     // screen 宽度方向 tile 数（0=禁用 clustered）
    uint     clusterTilesY;     // screen 高度方向 tile 数
    float    clusterNear;       // 近裁剪面
    float    clusterFar;        // 远裁剪面
    float    clusterLogFactor;  // log(far/near) 用于计算 Z slice
    uint     useClustered;      // 是否启用 clustered shading
    uint     rtShadowSource;    // 0=CSM, 1=RT 阴影（配合 u_RT_ShadowMask）
    uint     rtAOSource;        // 0=SSAO, 1=RT AO（配合 u_RT_AO）
    uint     rtSpecularSource;  // 0=SSR, 1=RT 反射（配合 u_RT_Reflection）
    uint     rtDiffuseSource;   // 0=SSGI, 1=RT GI（配合 u_RT_GI）
    float4   atmosphere;        // sunDir.xyz（指向太阳）+ turbidity（空中透视）
    float    ddgiScale;         // DDGI 贡献缩放（由 debugScale 驱动）
    float    giIntensity;       // 间接漫反射 GI 总强度（SSGI/RT GI/DDGI/RSM 统一驱动）
    float    aoIntensity;       // AO 强度（0=无 AO，1=完整 AO）
    // GI 分层合成参数经 GIBlendParams UBO 传递（见 kGPUBinding_GIBlendParams）
};

// GI 各通道分层合成参数（UBO：3 通道 × 32B，避免 push constant 超限且便于层栈扩展）
[[vk::binding(kGPUBinding_GIBlendParams, kGPUDescSet_PerFrame)]] cbuffer GIBlendParamsCB {
    GIBlendParams u_GIBlendParams;
};

// kGPUPrefilterMips / kGPUClusterSlices 统一在 ShaderTypes.slang（kGPU*）

// ============================================================
// GI 分层合成（Wave 1）：源采样与权重
//
// 合成按「源 id」分派而不是固定语义槽 —— 新增一种 GI 算法只需：
//   ① ShaderTypes.slang 加 GISOURCE_XXX 常量（与 C++ GISourceId 同步）
//   ② 在下面的 SampleXxxSource 加一个 case
//   ③ 实现算法本体（Provider）
// 无需改动 UBO 结构、帧图映射或合成循环。
// ============================================================

/// 源权重 = 用户权重 × 置信度 × 可选「距离让位」
/// 屏幕空间源（SSGI/SSR/SSAO/RTGI/RT 反射/RTAO）在屏幕边缘降权（屏幕外无数据）；
/// 世界空间源（IBL/DDGI/RSM）不依赖屏幕覆盖，不受 confEdge 影响。
float SourceWeight(GISourceSlot s, float confEdge, float camDist) {
    const bool screenSpace =
        (s.id == GISOURCE_SSGI || s.id == GISOURCE_SSR || s.id == GISOURCE_SSAO ||
         s.id == GISOURCE_RTGI || s.id == GISOURCE_RT_REFLECTION || s.id == GISOURCE_RTAO);
    float w = s.weight;
    if (screenSpace) w *= confEdge;
    if (s.falloffDistance > 0.0) w *= saturate(1.0 - camDist / s.falloffDistance);
    return w;
}

/// RSM 间接光（反射阴影贴图，单次反弹 VPL 求和）
///
/// 仅当存在方向光阴影时才采样：shadowParams.w 为光源类型（0=方向光/1=点光/2=聚光，
/// w<0.5 判定方向光），shadowParams.z 为阴影强度 >0 才有效——避免无方向阴影时
/// u_ShadowData[0] 全零（w==0 恰好匹配方向光编码）导致零矩阵投影注入 NaN。
/// 0.03 为 RSM 25-VPL 的离散化能量归一常数（VPL 数量有限，需按数量缩放总能量）。
float3 SampleRSMIndirect(float3 worldPos, float3 N, uint lightCount) {
    if (iblIntensity <= 0.0 || lightCount == 0u) return float3(0.0);
    if (u_ShadowData[0].shadowParams.w >= 0.5f) return float3(0.0);
    if (u_ShadowData[0].shadowParams.z <= 0.0) return float3(0.0);

    float4 lsp = mul(u_ShadowData[0].lightViewProj[0], float4(worldPos, 1.0));
    float2 rsmUV = saturate(lsp.xy / lsp.w * 0.5 + 0.5);
    float3 rsmIndirect = float3(0.0);
    for (int dx = -2; dx <= 2; dx++) {
        for (int dy = -2; dy <= 2; dy++) {
            float2 suv = saturate(rsmUV + float2(dx, dy) * 0.005);
            float4 ps = u_RSMPositionMap.Sample(u_IBLSampler, suv);
            float4 fs = u_RSMFluxMap.Sample(u_IBLSampler, suv);
            float3 sN  = normalize(fs.rgb * 2.0 - 1.0);
            float3 dir = ps.xyz - worldPos;
            float  d2  = max(dot(dir, dir), 0.01);
            rsmIndirect += fs.a * max(dot(N, normalize(dir)), 0.0)
                         * max(dot(sN, -normalize(dir)), 0.0) / d2;
        }
    }
    return rsmIndirect * 0.03 * iblIntensity;
}

/// 漫反射源采样（按 GISourceId 分派）
/// 注意：各源的输出都已是「已乘 albedo/BRDF 的间接漫反射贡献」，彼此可直接加权平均。
float3 SampleDiffuseSource(uint id, float3 worldPos, float3 N, float2 uv,
                           float3 albedo, float3 kD, bool furnace) {
    if (furnace) return float3(1.0, 1.0, 1.0);   // 白炉：各源入射辐射度真值 = 1
    switch (id) {
    case GISOURCE_IBL:   // 低频：环境辐照度（含 kD 与 albedo）
        return kD * u_IrradianceMap.Sample(u_IBLSampler, N).rgb * albedo * iblIntensity;
    case GISOURCE_DDGI:  // 低频：动态漫反射探针（世界空间）
        // 合成路径恒用真实辐照度——ddgiScale（GI_DDGI::debugScale）只用于调试可视化，
        // 若在此相乘会破坏能量守恒（归一化之内引入非物理缩放，Wave 0.6）
        return SampleDDGI(worldPos, N);
    case GISOURCE_SSGI:  // 中频：屏幕空间
        return u_SSGI.Sample(u_GBufferSampler, uv).rgb;
    case GISOURCE_RSM:   // 中频：反射阴影贴图
        return SampleRSMIndirect(worldPos, N, lightCount);
    case GISOURCE_RTGI:  // 高频：硬件光追
        return u_RT_GI.Sample(u_GBufferSampler, uv).rgb;
    default:
        return float3(0.0);
    }
}

/// 镜面源采样（按 GISourceId 分派）
float3 SampleSpecularSource(uint id, float2 uv, float3 R, float roughness,
                            float3 F, float2 envBRDF) {
    switch (id) {
    case GISOURCE_IBL:   // 低频：环境预滤波（含菲涅耳与环境 BRDF）
        return u_PrefilterMap.SampleLevel(u_IBLSampler, R,
                   roughness * (kGPUPrefilterMips - 1.0)).rgb
             * (F * envBRDF.r + envBRDF.g) * iblIntensity;
    case GISOURCE_SSR:           return u_SSR.Sample(u_GBufferSampler, uv).rgb;
    case GISOURCE_RT_REFLECTION: return u_RT_Reflection.Sample(u_GBufferSampler, uv).rgb;
    default:                     return float3(0.0);
    }
}

float4 fragmentMain(VSOutput input) : SV_Target0 {
    float2 uv = input.uv;

    // 1. 反算世界坐标
    // 1. 世界坐标直接从 GBuffer MRT4 读取（无需 Camera 矩阵，避免视角耦合）
    float4 gbE = u_GBufferE.Sample(u_GBufferSampler, uv);
    float3 worldPos = gbE.xyz;
    // 电介质 F0（由 IOR 预计算，存于 GBuffer worldPos.a）
    float  dielectricF0 = gbE.a;
    // 深度仅用于 Clustered Shading 的 Z-slice 计算（非世界坐标重建）
    float  depth    = u_Depth.Sample(u_GBufferSampler, uv).r;

    // 2. 读取 GBuffer
    float4 gbA = u_GBufferA.Sample(u_GBufferSampler, uv);
    float4 gbB = u_GBufferB.Sample(u_GBufferSampler, uv);
    float4 gbC = u_GBufferC.Sample(u_GBufferSampler, uv);
    float4 gbF = u_GBufferF.Sample(u_GBufferSampler, uv);  // disneyA（MRT5）
    float4 gbG = u_GBufferG.Sample(u_GBufferSampler, uv);  // disneyB（MRT6）

    float3 albedo    = gbA.rgb;
    float  metallic  = gbA.a;
    float3 N         = normalize(gbB.rgb * 2.0 - 1.0);
    float  roughness = gbB.a;
    float  ao        = gbC.a;

    // Disney BSDF 参数（GBuffer 读取，透传给 PBR_BRDF）
    float4 disneyA = gbF;  // anisotropic, subsurface, specular, sheen
    float4 disneyB = gbG;  // clearcoat, clearcoatGloss, specularTint.r, specularTint.g
    // specularTint.b 未随 GBuffer 携带（通道不足舍弃），Deferred 端固定为 1.0（默认中性色调）
    float  disneyC = 1.0;

    float3 V = normalize(cameraPosition.xyz - worldPos);

    // BRDF LUT：用于直接光照的多重散射能量补偿，IBL 镜面项复用同一采样
    float2 envBRDF = u_BRDF_LUT.Sample(u_IBLSampler, float2(max(dot(N,V),0.0), roughness)).rg;

    float3 color = float3(0.0);

    // ============================================================
    // 白炉数值测试（Wave 0.2）——能量守恒的硬判据
    //
    // 白炉条件：全白环境（各方向入射辐射度 = 1）+ 表面 albedo = 1 + 关闭直接光 + 无自发光。
    // 此时**每个间接光源的真值都是 1**，因此正确的分层合成（权重归一化、权重和 = 1）
    // 必须恰好得到 1.0。任何 > 1 的结果都说明有源被加在归一化之外（双重计数）。
    //
    // 实现方式：不是绕过真实路径另写一份，而是把"白炉下的源真值"代入**同一条**真实合成
    // 代码，这样测的就是真正的合成数学（也能暴露 IBL 环境项、RSM 之类在归一化之外累加的项）。
    // ============================================================
    const bool furnace = (u_GIBlendParams.diffuse.furnaceMode != 0u);
    if (furnace) {
        albedo = float3(1.0, 1.0, 1.0);   // 全白反照率
    }

    // RT 阴影因子：RT 阴影遮罩覆盖所有阴影光源（全屏像素级乘积），
    // 在直接光照结算后整体乘一次；rtShadowSource=0 时使用传统逐光源 CSM
    float rtShadowFactor = (rtShadowSource != 0u)
        ? u_RT_ShadowMask.Sample(u_GBufferSampler, uv).r : 1.0;

    // 3. 直接光照（Clustered 或线性回退）
    if (useClustered != 0u && clusterTilesX > 0u) {
        // --- Clustered Shading ---
        uint tileX = uint(input.position.x) / kGPUClusterTileSize;
        uint tileY = uint(input.position.y) / kGPUClusterTileSize;
        float linearDepth = clusterNear * clusterFar / (clusterFar - depth * (clusterFar - clusterNear));
        float sliceF = float(kGPUClusterSlices) * log(linearDepth / clusterNear) / clusterLogFactor;
        uint  slice  = uint(clamp(sliceF, 0.0f, float(kGPUClusterSlices - 1)));
        uint clusterIdx = (slice * clusterTilesY + tileY) * clusterTilesX + tileX;

        LightGridCell cell = u_LightGrid[clusterIdx];
        uint endIdx = cell.offset + min(cell.count, 64u);
        for (uint li = cell.offset; li < endIdx; li++) {
            uint i = u_LightIndexList[li];
            GPULight light = u_Lights[i];
        uint ltype = uint(light.directionType.w);
        float3 L; float attenuation = 1.0;
        if (ltype == 0u) {
            L = normalize(-light.directionType.xyz);
        } else {
            float3 toLight = light.positionRange.xyz - worldPos;
            float dist = length(toLight);
            L = toLight / max(dist, 0.001);
            float d = dist / max(abs(light.positionRange.w), 0.001);
            // 物理模式：positionRange.w < 0 → 平方反比衰减 + 范围软截止
            // 传统模式：positionRange.w > 0 → 多项式衰减
            if (light.positionRange.w < 0.0) {
                attenuation = 1.0 / max(d * d, 0.001);
                float fade = smoothstep(1.0, 0.8, d);
                attenuation *= fade * fade;  // 范围边缘软过渡
            } else {
                attenuation = 1.0 / (1.0 + 2.0 * d + d * d);
            }
            if (ltype == 2u) {
                float spotDot = dot(-L, normalize(light.directionType.xyz));
                attenuation *= smoothstep(light.coneAngles.y, light.coneAngles.x, spotDot);
            }
        }
        float3 radiance = light.colorIntensity.rgb * light.colorIntensity.w * attenuation;

        float shadow = 1.0;
        if (light.shadowIndex >= 0 && rtShadowSource == 0u) {
            GPUShadowData sm = u_ShadowData[light.shadowIndex];
            if (sm.shadowParams.w < 0.5f) {
                // 方向光 CSM
                shadow = lerp(1.0, SampleShadowPCF(worldPos, N, L, sm, cameraPosition.xyz,
                    u_ShadowMap0, u_ShadowMap1, u_ShadowMap2, u_ShadowSampler), sm.shadowParams.z);
            } else if (sm.shadowParams.w > 1.5f) {
                // 聚光灯透视阴影（单采样 + PCF 3×3）
                float4 spotClip = mul(sm.lightViewProj[0], float4(worldPos + N * sm.shadowParams.y, 1.0));
                float3 spotNdc = spotClip.xyz / spotClip.w;
                float2 spotUv = float2(spotNdc.x * 0.5 + 0.5, 0.5 - spotNdc.y * 0.5);
                float  spotZ  = spotNdc.z - sm.shadowParams.x;
                float2 ts = 1.0 / 1024.0;
                float spotShadow = 0.0;
                if (spotUv.x > 0.0 && spotUv.x < 1.0 && spotUv.y > 0.0 && spotUv.y < 1.0) {
                    for (int sx = -1; sx <= 1; sx++)
                        for (int sy = -1; sy <= 1; sy++)
                            spotShadow += (spotZ <= u_SpotShadowMap.Sample(u_ShadowSampler, spotUv + float2(sx, sy) * ts)) ? 1.0 : 0.0;
                    shadow = spotShadow / 9.0;
                }
            }
        }
        color += PBR_BRDF(albedo, metallic, roughness, N, V, L, dielectricF0, envBRDF, disneyA, disneyB, disneyC) * radiance * shadow;
        }  // end clustered for
    } else {
        // --- 线性回退（Clustered 禁用时）---
        uint count = min(lightCount, 8u);
        for (uint i = 0u; i < count; i++) {
            GPULight light = u_Lights[i];
        uint ltype = uint(light.directionType.w);
        float3 L; float attenuation = 1.0;
        if (ltype == 0u) { L = normalize(-light.directionType.xyz); }
        else {
            float3 toLight = light.positionRange.xyz - worldPos;
            float dist = length(toLight); L = toLight / max(dist, 0.001);
            float d = dist / max(abs(light.positionRange.w), 0.001);
            if (light.positionRange.w < 0.0) {
                attenuation = 1.0 / max(d * d, 0.001);
                float fade = smoothstep(1.0, 0.8, d);
                attenuation *= fade * fade;
            } else {
                attenuation = 1.0 / (1.0 + 2.0 * d + d * d);
            }
            if (ltype == 2u) { float spotDot = dot(-L, normalize(light.directionType.xyz));
                attenuation *= smoothstep(cos(light.coneAngles.y), cos(light.coneAngles.x), spotDot); }
        }
        float3 radiance = light.colorIntensity.rgb * light.colorIntensity.w * attenuation;
        float shadow = 1.0;
        if (light.shadowIndex >= 0 && rtShadowSource == 0u) {
            GPUShadowData sm = u_ShadowData[light.shadowIndex];
            if (sm.shadowParams.w < 0.5f) {
                shadow = lerp(1.0, SampleShadowPCF(worldPos, N, L, sm, cameraPosition.xyz,
                    u_ShadowMap0, u_ShadowMap1, u_ShadowMap2, u_ShadowSampler), sm.shadowParams.z);
            } else if (sm.shadowParams.w > 1.5f) {
                float4 spotClip = mul(sm.lightViewProj[0], float4(worldPos + N * sm.shadowParams.y, 1.0));
                float3 spotNdc = spotClip.xyz / spotClip.w;
                float2 spotUv = float2(spotNdc.x * 0.5 + 0.5, 0.5 - spotNdc.y * 0.5);
                float  spotZ  = spotNdc.z - sm.shadowParams.x;
                float2 ts = 1.0 / 1024.0; float spotShadow = 0.0;
                if (spotUv.x > 0.0 && spotUv.x < 1.0 && spotUv.y > 0.0 && spotUv.y < 1.0) {
                    for (int sx = -1; sx <= 1; sx++)
                        for (int sy = -1; sy <= 1; sy++)
                            spotShadow += (spotZ <= u_SpotShadowMap.Sample(u_ShadowSampler, spotUv + float2(sx, sy) * ts)) ? 1.0 : 0.0;
                    shadow = spotShadow / 9.0;
                }
            }
        }
        color += PBR_BRDF(albedo, metallic, roughness, N, V, L, dielectricF0, envBRDF, disneyA, disneyB, disneyC) * radiance * shadow;
        }
    }

    // RT 阴影整体因子（覆盖所有阴影光源；rtShadowSource=0 时恒为 1.0）
    color *= rtShadowFactor;

    // 4. IBL / BRDF 公共项（供源分派使用）
    float3 F0_ibl = lerp(float3(dielectricF0), albedo, metallic);
    float3 F = F_SchlickRoughness(max(dot(N, V), 0.0), F0_ibl, roughness, metallic);
    float3 kD = (1.0 - F) * (1.0 - metallic);
    float3 R = reflect(-V, N);
    if (furnace) {
        // 白炉测试聚焦「合成数学」，不掺 BRDF 的菲涅耳分布：kD 取 1
        kD = float3(1.0, 1.0, 1.0);
    }

    // 屏幕内可见性置信度（屏幕外无数据 → 靠近边缘 5% 内线性降权）
    float2 edge     = min(uv, 1.0 - uv);
    float  confEdge = saturate(min(edge.x, edge.y) / 0.05);
    float  camDist  = length(cameraPosition.xyz - worldPos);

    // ═══════════════════════════════════════════════════════════════
    // GI 分层合成（Wave 1：按源 id 分派，遍历通道的源数组）
    //
    // 每个通道的多个源描述的是**同一个物理量** → 归一化加权（权重和 = 1）避免双重计数。
    // IBL 与 RSM 也作为**普通源**参与（Wave 0.4 / 0.5）——不再有归一化之外的加法项。
    // 采样函数定义在文件作用域（见下方 SampleXxxSource / SourceWeight）。
    // ═══════════════════════════════════════════════════════════════

    // ── 间接漫反射合成 ──
    {
        GIChannelBlendParams bl = u_GIBlendParams.diffuse;
        float3 num = float3(0.0);
        float  den = 0.0;
        for (uint i = 0u; i < bl.count; ++i) {
            GISourceSlot s = bl.sources[i];
            if (s.weight <= 0.0) continue;
            float w = SourceWeight(s, confEdge, camDist);
            num += SampleDiffuseSource(s.id, worldPos, N, uv, albedo, kD, furnace) * w;
            den += w;
        }
        float3 indirect = (bl.mode != 0u)
                        ? ((den > 0.0) ? (num / den) : float3(0.0))   // 归一化：无双重计数
                        : num;                                        // 相加（对照）
        // 白炉判据只反映「合成数学」：giIntensity 是用户强度调节，判据下取 1 以便读回值即合成结果
        color += indirect * (furnace ? 1.0 : giIntensity);
    }

    // ── 白炉数值测试：到此即返回 ──
    // 白炉下 diffuse 通道的间接光真值 = 1；此时**所有**源都已进入归一化，
    // 若仍读到 > 1，说明存在归一化之外的加法项（双重计数）。
    if (furnace) {
        return float4(color, 1.0);
    }

    // ── AO 合成（归一化加权：SSAO / RTAO 可同时参与）──
    {
        GIChannelBlendParams ba = u_GIBlendParams.ao;
        float aoNum = 0.0;
        float aoDen = 0.0;
        for (uint i = 0u; i < ba.count; ++i) {
            GISourceSlot s = ba.sources[i];
            if (s.weight <= 0.0) continue;
            float v = (s.id == GISOURCE_RTAO)
                    ? u_RT_AO.Sample(u_GBufferSampler, uv).r
                    : u_SSAO.Sample(u_GBufferSampler, uv).r;
            aoNum += v * s.weight;
            aoDen += s.weight;
        }
        float aoVal = (aoDen > 0.0) ? (aoNum / aoDen) : 1.0;   // 无 AO 源 → 不遮蔽
        color *= lerp(1.0, ao * aoVal, aoIntensity);   // aoIntensity 控制 AO 强度（0=无 AO）
    }

    // ── 间接镜面合成（与 diffuse 同构）──
    // 环境镜面（IBL prefilter）与屏幕空间/光追反射描述同一物理量 → 归一化合成；
    // a<0 表示该源本条光线无效（高粗糙度跳过或 miss）→ 该源权重记 0，由其他源兜底
    {
        GIChannelBlendParams bs = u_GIBlendParams.specular;
        float3 specNum = float3(0.0);
        float  specDen = 0.0;
        for (uint i = 0u; i < bs.count; ++i) {
            GISourceSlot s = bs.sources[i];
            if (s.weight <= 0.0) continue;
            if (s.id == GISOURCE_SSR) {
                if (u_SSR.Sample(u_GBufferSampler, uv).a < 0.0) continue;              // 本条无效
            } else if (s.id == GISOURCE_RT_REFLECTION) {
                if (u_RT_Reflection.Sample(u_GBufferSampler, uv).a < 0.0) continue;
            }
            float w = SourceWeight(s, confEdge, camDist);
            specNum += SampleSpecularSource(s.id, uv, R, roughness, F, envBRDF) * w;
            specDen += w;
        }
        color += (bs.mode != 0u) ? (specNum / max(specDen, 1e-4)) : specNum;
    }

    color += gbC.rgb;  // emissive

    // 空中透视：对场景几何应用距离/高度相关大气消光 + 入射散射
    ApplyAerialPerspective(color, cameraPosition.xyz, worldPos, atmosphere.xyz, atmosphere.w);
    return float4(color, 1.0);
}
