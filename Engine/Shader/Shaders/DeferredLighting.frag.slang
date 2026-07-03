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

[[vk::push_constant]] cbuffer LightingPC {
    float4x4 invViewProj;
    float4   cameraPosition;
    uint     lightCount;
    float    iblIntensity;
    uint     _pad[2];
};

static const float kPrefilterMips = 5.0;

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

    // 3. 直接光照
    uint count = min(lightCount, 8u);
    for (uint i = 0u; i < count; i++) {
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
                shadow = lerp(1.0, SampleShadowPCF(worldPos, N, L, sm, cameraPosition.xyz,
                    u_ShadowMap0, u_ShadowMap1, u_ShadowMap2, u_ShadowSampler), sm.shadowParams.z);
            }
        }
        color += PBR_BRDF(albedo, metallic, roughness, N, V, L) * radiance * shadow;
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
    color += gbC.rgb;  // emissive
    return float4(color, 1.0);
}
