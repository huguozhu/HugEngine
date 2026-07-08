// ============================================================
// PBR.frag.slang — PBR 片元着色器 (GPU-driven)
// ============================================================

#include "pbr_common.slang"

struct FSInput {
    [[vk::location(0)]] float3 worldPos;
    [[vk::location(1)]] float3 worldNormal;
    [[vk::location(2)]] float2 uv;
    [[vk::location(3)]] nointerpolation uint objectIndex;  // 从 VS 传入
};

// GPUObjectData 定义来自 ShaderTypes.slang（C++/Slang 共享）

// GPULight 定义来自 ShaderTypes.slang（C++/Slang 共享）

// GPUShadowData 定义来自 ShaderTypes.slang（C++/Slang 共享）

// Descriptor Sets:
//   set=0: per-frame 动态数据
//     binding=1: GPULight[]
//     binding=2: GPUObjectData[]
//     binding=3: GPUShadowData[]
//     binding=4: ShadowMap CSM cascade 0 (CombinedImageSampler)
//     binding=9: PointShadow Cubemap
//     binding=10-11: CSM cascade 1-2
//     binding=12-14: IBL (Irradiance/Prefilter/BRDF LUT)
//     binding=15-16: RSM (Position/Flux)
//   set=1: per-mesh 静态纹理（永不更新，避免帧间竞态）
//     binding=5: BaseColor + Sampler
//     binding=6: Normal + Sampler
//     binding=7: MetallicRoughness + Sampler
//     binding=8: Occlusion + Sampler
[[vk::binding(1, 0)]] StructuredBuffer<GPULight> u_Lights;
[[vk::binding(2, 0)]] StructuredBuffer<GPUObjectData> u_Objects;
[[vk::binding(3, 0)]] StructuredBuffer<GPUShadowData> u_ShadowData;
[[vk::binding(4, 0)]] Texture2D<float> u_ShadowMap0;   // CSM cascade 0
[[vk::binding(4, 0)]] SamplerState u_ShadowSampler;
[[vk::binding(10, 0)]] Texture2D<float> u_ShadowMap1;  // CSM cascade 1
[[vk::binding(11, 0)]] Texture2D<float> u_ShadowMap2;  // CSM cascade 2
// Bindless 纹理通过 u_Textures[] / u_Samplers[] 访问（common.slang 声明）
// 偏移: materialID + 0 = BaseColor, +1 = Normal, +2 = MetallicRoughness, +3 = Occlusion
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
    GPUObjectData obj = u_Objects[input.objectIndex];

    uint texBase = obj.materialID;
    float4 baseColorSample = u_Textures[texBase + 0].Sample(u_Samplers[0], input.uv);
    float3 albedo = obj.baseColorFactor.rgb * baseColorSample.rgb;
    float alpha   = obj.baseColorFactor.a * baseColorSample.a;
    if (alpha < obj.alphaCutoff) discard;

    // 法线贴图
    float3 texNormal = u_Textures[texBase + 1].Sample(u_Samplers[0], input.uv).rgb * 2.0 - 1.0;
    float3 N = normalize(input.worldNormal + texNormal * 0.5);
    float3 V = normalize(cameraPosition.xyz - input.worldPos);

    // 金属度/粗糙度贴图
    float4 mrSample = u_Textures[texBase + 2].Sample(u_Samplers[0], input.uv);
    float metallic  = obj.metallicFactor  * mrSample.b;
    float roughness = clamp(obj.roughnessFactor * mrSample.g, 0.04, 1.0);
    float ao = obj.aoFactor * u_Textures[texBase + 3].Sample(u_Samplers[0], input.uv).r;

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
    if (iblIntensity > 0.0 && u_ShadowData[0].shadowParams.z > 0.0) {
        // 将世界坐标变换到光源空间（使用第一个方向光的 CSM cascade 0 VP）
        float4 lightSpacePos = mul(u_ShadowData[0].lightViewProj[0], float4(input.worldPos, 1.0));
        float2 rsmCoord = lightSpacePos.xy / lightSpacePos.w * 0.5 + 0.5;  // [-1,1] → [0,1]

        float3 rsmIndirect = float3(0.0);
        const float rsmRadius = 0.005;  // RSM 采样半径
        for (int dx = -2; dx <= 2; dx++) {
            for (int dy = -2; dy <= 2; dy++) {
                float2 uv = saturate(rsmCoord + float2(dx, dy) * rsmRadius);
                float4 posSample  = u_RSMPositionMap.Sample(u_RSMSampler, uv);
                float4 fluxSample = u_RSMFluxMap.Sample(u_RSMSampler, uv);
                float3 samplePos  = posSample.xyz;
                float3 sampleN    = normalize(fluxSample.rgb * 2.0 - 1.0);
                float  flux       = fluxSample.a;
                float3 dir        = samplePos - input.worldPos;
                float  dist2      = max(dot(dir, dir), 0.01);
                float  cosDir     = max(dot(sampleN, -normalize(dir)), 0.0);
                rsmIndirect += flux * max(dot(N, normalize(dir)), 0.0) * cosDir / dist2;
            }
        }
        color += rsmIndirect * 0.03 * iblIntensity;  // RSM 间接光贡献
    }

    color += obj.emissiveFactor.rgb;

    // HDR 线性输出到 RGBA16_FLOAT 离屏目标，色调映射由 ToneMap Pass 完成
    return float4(color, alpha);
}
