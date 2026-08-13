// DeferredLighting.frag — 延迟光照全屏片段着色器
// 读取 GBuffer MRT + Shadow/IBL/RSM 纹理，逐光源 PBR + 间接光
#include "pbr_common.slang"
#include "RT_DDGI.slang"  // DDGI 探针查询共享库（常量 + EvalDDGI_SH + SampleDDGI）

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
[[vk::binding(kGPUBinding_SpotShadow, kGPUDescSet_PerFrame)]] Texture2D<float>  u_SpotShadowMap;  // 聚光灯阴影贴图
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
};

// kGPUPrefilterMips / kGPUClusterSlices 统一在 ShaderTypes.slang（kGPU*）

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
                attenuation *= smoothstep(light.coneAngles.y, light.coneAngles.x, spotDot); }
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

    // 4. IBL
    float3 F0_ibl = lerp(float3(dielectricF0), albedo, metallic);
    float3 F = F_SchlickRoughness(max(dot(N, V), 0.0), F0_ibl, roughness, metallic);
    float3 kD = (1.0 - F) * (1.0 - metallic);
    color += kD * u_IrradianceMap.Sample(u_IBLSampler, N).rgb * albedo * iblIntensity;
    float3 R = reflect(-V, N);
    float3 prefiltered = u_PrefilterMap.SampleLevel(u_IBLSampler, R, roughness * (kGPUPrefilterMips - 1.0)).rgb;
    color += prefiltered * (F * envBRDF.r + envBRDF.g) * iblIntensity;

    // 5. RSM 间接
    if (iblIntensity > 0.0 && u_ShadowData[0].shadowParams.z > 0.0) {
        float4 lsp = mul(u_ShadowData[0].lightViewProj[0], float4(worldPos, 1.0));
        float2 rsmUV = saturate(lsp.xy / lsp.w * 0.5 + 0.5);
        float3 rsmIndirect = float3(0.0);
        for (int dx = -2; dx <= 2; dx++) {
            for (int dy = -2; dy <= 2; dy++) {
                float2 suv = saturate(rsmUV + float2(dx, dy) * 0.005);
                float4 ps = u_RSMPositionMap.Sample(u_IBLSampler, suv);
                float4 fs = u_RSMFluxMap.Sample(u_IBLSampler, suv);
                float3 sN = normalize(fs.rgb * 2.0 - 1.0);
                float3 dir = ps.xyz - worldPos;
                float d2 = max(dot(dir, dir), 0.01);
                rsmIndirect += fs.a * max(dot(N, normalize(dir)), 0.0) * max(dot(sN, -normalize(dir)), 0.0) / d2;
            }
        }
        color += rsmIndirect * 0.03 * iblIntensity;
    }

    // SSAO 屏幕空间遮蔽（与 GBuffer AO 相乘）；rtAOSource=1 时改用 RT AO 遮罩
    float ssao = u_SSAO.Sample(u_GBufferSampler, uv).r;
    float aoVal = (rtAOSource != 0u) ? u_RT_AO.Sample(u_GBufferSampler, uv).r : ssao;
    color *= ao * aoVal;

    // SSGI 间接漫反射（屏幕空间）；rtDiffuseSource=1 时改用 RT GI 输出
    float4 ssgi = u_SSGI.Sample(u_GBufferSampler, uv);
    float4 rtGI = (rtDiffuseSource != 0u) ? u_RT_GI.Sample(u_GBufferSampler, uv) : ssgi;
    color += rtGI.rgb;

    // DDGI 间接漫反射（探针网格，世界空间三线性插值 + SH 评估）
    float3 ddgi = SampleDDGI(worldPos, N);
    color += ddgi * 0.5;  // 强度缩放（与 SSGI 叠加，避免过度照亮）

    // SSR 间接镜面反射；rtSpecularSource=1 时改用 RT 反射输出
    float4 ssr = u_SSR.Sample(u_GBufferSampler, uv);
    float4 rtRefl = (rtSpecularSource != 0u) ? u_RT_Reflection.Sample(u_GBufferSampler, uv) : ssr;
    // a<0（高粗糙度跳过或 miss）不叠加 RT 反射，specular IBL prefilter 已提供回退
    if (rtRefl.a >= 0.0) color += rtRefl.rgb;

    color += gbC.rgb;  // emissive
    return float4(color, 1.0);
}
