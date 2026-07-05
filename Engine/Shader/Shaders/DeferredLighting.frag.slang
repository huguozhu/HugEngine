// DeferredLighting.frag — 延迟光照全屏片段着色器
// 读取 GBuffer MRT + Shadow/IBL/RSM 纹理，逐光源 PBR + 间接光
#include "pbr_common.slang"

struct VSOutput {
    float4 position          : SV_Position;
    [[vk::location(0)]] float2 uv;
};

// GBuffer 纹理
[[vk::binding(0, 0)]] Texture2D<float4> u_GBufferA;  // albedo.rgb + metallic
[[vk::binding(0, 0)]] SamplerState      u_GBufferSampler;
[[vk::binding(1, 0)]] Texture2D<float4> u_GBufferB;  // normal*0.5+0.5 + roughness
[[vk::binding(2, 0)]] Texture2D<float4> u_GBufferC;  // emissive.rgb + ao
[[vk::binding(3, 0)]] Texture2D<float>  u_Depth;

// Shadow + IBL + RSM (复用 Forward PBR 绑定)
[[vk::binding(4, 0)]] Texture2D<float>  u_ShadowMap0;
[[vk::binding(4, 0)]] SamplerState      u_ShadowSampler;
[[vk::binding(9, 0)]] Texture2D<float>  u_SpotShadowMap;  // 聚光灯阴影贴图
[[vk::binding(10, 0)]] Texture2D<float> u_ShadowMap1;
[[vk::binding(11, 0)]] Texture2D<float> u_ShadowMap2;
[[vk::binding(12, 0)]] TextureCube<float4> u_IrradianceMap;
[[vk::binding(12, 0)]] SamplerState        u_IBLSampler;
[[vk::binding(13, 0)]] TextureCube<float4> u_PrefilterMap;
[[vk::binding(14, 0)]] Texture2D<float2>   u_BRDF_LUT;
[[vk::binding(15, 0)]] Texture2D<float4>   u_RSMPositionMap;
[[vk::binding(16, 0)]] Texture2D<float4>   u_RSMFluxMap;

// GPU 数据结构（与 PBR.frag.slang 一致）
struct GPULight {
    float4 colorIntensity;
    float4 directionType;
    float4 positionRange;
    float2 coneAngles;
    int    shadowIndex;
    int    _padLight;
};
struct GPUShadowData {
    float4x4 lightViewProj[3];
    float4   shadowParams;
    float4   splitDistances;
    float4   cameraForward;
    float4   pointLightData;
};

[[vk::binding(17, 0)]] StructuredBuffer<GPULight>      u_Lights;
[[vk::binding(18, 0)]] StructuredBuffer<GPUShadowData> u_ShadowData;
[[vk::binding(19, 0)]] Texture2D<float4> u_SSGI;  // SSGI 间接光照

// Clustered Shading: LightGrid + LightIndexList（每 cluster 只迭代影响它的光源）
struct LightGridCell { uint offset; uint count; };
[[vk::binding(7, 0)]]  StructuredBuffer<LightGridCell> u_LightGrid;       // 每 cluster 的光源列表头
[[vk::binding(8, 0)]]  StructuredBuffer<uint>          u_LightIndexList;  // 扁平光源索引数组

// Cluster 网格参数（push constant 传入）
[[vk::push_constant]] cbuffer LightingPC {
    float4x4 invViewProj;
    float4   cameraPosition;
    uint     lightCount;
    float    iblIntensity;
    uint     clusterTilesX;     // screen 宽度方向 tile 数（0=禁用 clustered）
    uint     clusterTilesY;     // screen 高度方向 tile 数
    float    clusterNear;       // 近裁剪面
    float    clusterFar;        // 远裁剪面
    float    clusterLogFactor;  // log(far/near) 用于计算 Z slice
    uint     useClustered;      // 是否启用 clustered shading
    uint     _pad2[2];
};

static const float kPrefilterMips = 5.0;
static const uint  kClusterSlices = 12;  // 与 C++ kDepthSlices 保持一致

float4 fragmentMain(VSOutput input) : SV_Target0 {
    float2 uv = input.uv;

    // 1. 反算世界坐标
    float depth = u_Depth.Sample(u_GBufferSampler, uv).r;
    float4 clipPos = float4(uv * 2.0 - 1.0, depth, 1.0);
    float4 worldPos4 = mul(invViewProj, clipPos);
    float3 worldPos  = worldPos4.xyz / worldPos4.w;

    // 2. 读取 GBuffer
    float4 gbA = u_GBufferA.Sample(u_GBufferSampler, uv);
    float4 gbB = u_GBufferB.Sample(u_GBufferSampler, uv);
    float4 gbC = u_GBufferC.Sample(u_GBufferSampler, uv);

    float3 albedo    = gbA.rgb;
    float  metallic  = gbA.a;
    float3 N         = normalize(gbB.rgb * 2.0 - 1.0);
    float  roughness = gbB.a;
    float  ao        = gbC.a;

    float3 V = normalize(cameraPosition.xyz - worldPos);
    float3 color = float3(0.0);

    // 3. 直接光照（Clustered 或线性回退）
    if (useClustered != 0u && clusterTilesX > 0u) {
        // --- Clustered Shading ---
        uint tileX = uint(input.position.x) / 64;
        uint tileY = uint(input.position.y) / 64;
        float linearDepth = clusterNear * clusterFar / (clusterFar - depth * (clusterFar - clusterNear));
        float sliceF = float(kClusterSlices) * log(linearDepth / clusterNear) / clusterLogFactor;
        uint  slice  = uint(clamp(sliceF, 0.0f, float(kClusterSlices - 1)));
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
            float d = dist / max(light.positionRange.w, 0.001);
            attenuation = 1.0 / (1.0 + 2.0 * d + d * d);
            if (ltype == 2u) {
                float spotDot = dot(-L, normalize(light.directionType.xyz));
                attenuation *= smoothstep(light.coneAngles.y, light.coneAngles.x, spotDot);
            }
        }
        float3 radiance = light.colorIntensity.rgb * light.colorIntensity.w * attenuation;

        float shadow = 1.0;
        if (light.shadowIndex >= 0) {
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
        color += PBR_BRDF(albedo, metallic, roughness, N, V, L) * radiance * shadow;
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
            float d = dist / max(light.positionRange.w, 0.001);
            attenuation = 1.0 / (1.0 + 2.0 * d + d * d);
            if (ltype == 2u) { float spotDot = dot(-L, normalize(light.directionType.xyz));
                attenuation *= smoothstep(light.coneAngles.y, light.coneAngles.x, spotDot); }
        }
        float3 radiance = light.colorIntensity.rgb * light.colorIntensity.w * attenuation;
        float shadow = 1.0;
        if (light.shadowIndex >= 0) {
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
        color += PBR_BRDF(albedo, metallic, roughness, N, V, L) * radiance * shadow;
        }
    }

    // 4. IBL
    float3 F0_ibl = lerp(float3(0.04), albedo, metallic);
    float3 F = F_SchlickRoughness(max(dot(N, V), 0.0), F0_ibl, roughness, metallic);
    float3 kD = (1.0 - F) * (1.0 - metallic);
    color += kD * u_IrradianceMap.Sample(u_IBLSampler, N).rgb * albedo * iblIntensity;
    float3 R = reflect(-V, N);
    float3 prefiltered = u_PrefilterMap.SampleLevel(u_IBLSampler, R, roughness * (kPrefilterMips - 1.0)).rgb;
    float2 envBRDF = u_BRDF_LUT.Sample(u_IBLSampler, float2(max(dot(N,V),0.0), roughness)).rg;
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

    color *= ao;

    // SSGI 间接漫反射
    float4 ssgi = u_SSGI.Sample(u_GBufferSampler, uv);
    color += ssgi.rgb;

    color += gbC.rgb;  // emissive
    return float4(color, 1.0);
}
