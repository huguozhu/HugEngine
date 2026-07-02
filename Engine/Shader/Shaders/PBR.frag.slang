// ============================================================
// PBR.frag.slang — PBR 片元着色器 (GPU-driven)
// ============================================================

#include "pbr_common.slang"

struct FSInput {
    [[vk::location(0)]] float3 worldPos;
    [[vk::location(1)]] float3 worldNormal;
    [[vk::location(2)]] float2 uv;
};

struct GPUObjectData {
    float4x4 worldMatrix;
    float4   baseColorFactor;
    float4   emissiveFactor;
    float    metallicFactor;
    float    roughnessFactor;
    float    aoFactor;
    float    alphaCutoff;
    uint     materialFlags;
    uint     materialID;
    uint     _pad[2];
};

struct GPULight {
    float4 colorIntensity;
    float4 directionType;
    float4 positionRange;
    float2 coneAngles;
    int    shadowIndex;
    int    _padLight;
};

struct GPUShadowData {
    float4x4 lightViewProj[3];  // 方向光 CSM 级联矩阵
    float4   shadowParams;      // x=bias, y=normalBias, z=strength, w=类型
    float4   splitDistances;    // CSM 级联分割点
    float4   cameraForward;     // 相机前向(级联选择)
    float4   pointLightData;    // xyz=点光位置, w=范围
};

// Descriptor Sets (set=0):
//   binding=1: GPULight[]
//   binding=2: GPUObjectData[]
//   binding=3: GPUShadowData[]
//   binding=4: ShadowMap 深度纹理 (CombinedImageSampler)
[[vk::binding(1, 0)]] StructuredBuffer<GPULight> u_Lights;
[[vk::binding(2, 0)]] StructuredBuffer<GPUObjectData> u_Objects;
[[vk::binding(3, 0)]] StructuredBuffer<GPUShadowData> u_ShadowData;
[[vk::binding(4, 0)]] Texture2D<float> u_ShadowMap0;   // CSM cascade 0
[[vk::binding(4, 0)]] SamplerState u_ShadowSampler;
[[vk::binding(10, 0)]] Texture2D<float> u_ShadowMap1;  // CSM cascade 1
[[vk::binding(11, 0)]] Texture2D<float> u_ShadowMap2;  // CSM cascade 2
[[vk::binding(5, 0)]] Texture2D<float4> u_BaseColorTex;
[[vk::binding(5, 0)]] SamplerState u_BaseColorSampler;
[[vk::binding(6, 0)]] Texture2D<float4> u_NormalTex;
[[vk::binding(6, 0)]] SamplerState u_NormalSampler;
[[vk::binding(7, 0)]] Texture2D<float4> u_MetallicRoughnessTex;
[[vk::binding(7, 0)]] SamplerState u_MetallicRoughnessSampler;
[[vk::binding(8, 0)]] Texture2D<float4> u_OcclusionTex;
[[vk::binding(8, 0)]] SamplerState u_OcclusionSampler;
[[vk::binding(9, 0)]] TextureCube<float> u_PointShadowCube;
[[vk::binding(9, 0)]] SamplerState u_PointShadowSampler;
// IBL 纹理
[[vk::binding(12, 0)]] TextureCube<float4> u_IrradianceMap;
[[vk::binding(12, 0)]] SamplerState       u_IBLSampler;
[[vk::binding(13, 0)]] TextureCube<float4> u_PrefilterMap;
[[vk::binding(14, 0)]] Texture2D<float2>   u_BRDF_LUT;
static const float kPrefilterMips = 5.0;  // 预滤波 mip 数（匹配 GI_IBL::kPrefilterMips）

// RSM 纹理（Reflective Shadow Maps）
[[vk::binding(15, 0)]] Texture2D<float4> u_RSMPositionMap;   // worldPos.xyz
[[vk::binding(15, 0)]] SamplerState      u_RSMSampler;
[[vk::binding(16, 0)]] Texture2D<float4> u_RSMFluxMap;       // normal.xy + flux

[[vk::push_constant]] cbuffer FrameConstants {
    float4x4 viewProjMatrix;
    float4   cameraPosition;
    uint     lightCount;
    uint     objectIndex;
    float    iblIntensity;  // IBL 强度倍率（0=关闭）
    uint     _pad;
};

float3 ACESFilm(float3 x) {
    float a = 2.51; float b = 0.03; float c = 2.43; float d = 0.59; float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

float4 fragmentMain(FSInput input) : SV_Target0 {
    GPUObjectData obj = u_Objects[objectIndex];

    float4 baseColorSample = u_BaseColorTex.Sample(u_BaseColorSampler, input.uv);
    float3 albedo = obj.baseColorFactor.rgb * baseColorSample.rgb;
    float alpha   = obj.baseColorFactor.a * baseColorSample.a;
    if (alpha < obj.alphaCutoff) discard;

    // 法线贴图
    float3 texNormal = u_NormalTex.Sample(u_NormalSampler, input.uv).rgb * 2.0 - 1.0;
    float3 N = normalize(input.worldNormal + texNormal * 0.5);
    float3 V = normalize(cameraPosition.xyz - input.worldPos);

    // 金属度/粗糙度贴图
    float4 mrSample = u_MetallicRoughnessTex.Sample(u_MetallicRoughnessSampler, input.uv);
    float metallic  = obj.metallicFactor  * mrSample.b;
    float roughness = clamp(obj.roughnessFactor * mrSample.g, 0.04, 1.0);
    float ao = obj.aoFactor * u_OcclusionTex.Sample(u_OcclusionSampler, input.uv).r;

    // 多光源循环
    float3 color = albedo * 0.08 * ao;  // 低环境光，避免过曝，让光源主导照明
    uint count = min(lightCount, 8u);
    for (uint i = 0; i < count; i++) {
        GPULight light = u_Lights[i];
        uint ltype = uint(light.directionType.w);
        float3 L; float attenuation = 1.0;
        if (ltype == 0u) {
            L = normalize(-light.directionType.xyz);
        } else {
            float3 toLight = light.positionRange.xyz - input.worldPos;
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

        // 阴影采样
        float shadow = 1.0;
        if (light.shadowIndex >= 0) {
            GPUShadowData sm = u_ShadowData[light.shadowIndex];
            // shadowParams.w: 0=方向光, 1=点光
            if (sm.shadowParams.w < 0.5f) {
                // 方向光阴影 — 2D 纹理 PCF
                shadow = lerp(1.0, SampleShadowPCF(input.worldPos, N, L, sm,
                              cameraPosition.xyz,
                              u_ShadowMap0, u_ShadowMap1, u_ShadowMap2,
                              u_ShadowSampler),
                              sm.shadowParams.z);
            } else {
                // 点光源阴影 — Cubemap 采样
                float3 lightPos = sm.pointLightData.xyz;
                float  range    = sm.pointLightData.w;
                shadow = lerp(1.0, SamplePointShadow(input.worldPos, lightPos, range,
                              sm, u_PointShadowCube, u_PointShadowSampler),
                              sm.shadowParams.z);
            }
        }

        color += PBR_BRDF(albedo, metallic, roughness, N, V, L) * radiance * shadow;
    }
    color *= ao;

    // ---- IBL（环境光照） ----
    float3 F0_ibl = lerp(float3(0.04), albedo, metallic);
    float3 F = F_SchlickRoughness(max(dot(N, V), 0.0), F0_ibl, roughness, metallic);
    float3 kD = (1.0 - F) * (1.0 - metallic);
    float3 irradiance = u_IrradianceMap.Sample(u_IBLSampler, N).rgb;
    color += kD * irradiance * albedo * iblIntensity;

    // 镜面反射 IBL：预滤波环境图（roughness→mip）+ BRDF LUT
    float3 R = reflect(-V, N);
    float mipLevel = roughness * float(kPrefilterMips - 1);
    float3 prefilteredColor = u_PrefilterMap.SampleLevel(u_IBLSampler, R, mipLevel).rgb;
    float2 envBRDF = u_BRDF_LUT.Sample(u_IBLSampler, float2(max(dot(N, V), 0.0), roughness)).rg;
    color += prefilteredColor * (F * envBRDF.r + envBRDF.g) * iblIntensity;

    // ---- RSM 间接光照（Reflective Shadow Maps） ----
    if (iblIntensity > 0.0) {
        // 采样 RSM：在 light space 采样临近像素，累加间接漫反射
        float3 rsmIndirect = float3(0.0);
        float2 rsmCoord = float2(0.5, 0.5);  // placeholder: light-space projection
        const float rsmRadius = 0.01;  // 采样半径（RSM 空间）
        for (int dx = -2; dx <= 2; dx++) {
            for (int dy = -2; dy <= 2; dy++) {
                float2 uv = rsmCoord + float2(dx, dy) * rsmRadius;
                float4 posSample  = u_RSMPositionMap.Sample(u_RSMSampler, uv);
                float4 fluxSample = u_RSMFluxMap.Sample(u_RSMSampler, uv);
                float3 samplePos  = posSample.xyz;
                float3 sampleN    = normalize(fluxSample.rgb * 2.0 - 1.0);  // 法线编码 [0,1]→[-1,1]
                float  flux       = fluxSample.a;  // 通量存储在 alpha 通道
                float3 dir        = samplePos - input.worldPos;
                float  dist2      = max(dot(dir, dir), 0.01);
                float  NdotDir    = max(dot(N, normalize(dir)), 0.0);
                rsmIndirect += flux * NdotDir / dist2 * albedo;
            }
        }
        color += rsmIndirect * 0.02 * iblIntensity;  // RSM 衰减系数
    }

    color += obj.emissiveFactor.rgb;

    // HDR 线性输出到 RGBA16_FLOAT 离屏目标，色调映射由 ToneMap Pass 完成
    return float4(color, alpha);
}
