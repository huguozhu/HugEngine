// ============================================================
// pbr_common.slang — PBR BRDF 函数库
//
// Cook-Torrance 微面元镜面 BRDF + Lambertian 漫反射
// ============================================================
#ifndef HUGENGINE_PBR_COMMON_SLANG
#define HUGENGINE_PBR_COMMON_SLANG

#include "common.slang"

// ============================================================
// Fresnel — Schlick 近似
// ============================================================
float3 F_Schlick(float3 f0, float cosTheta) {
    return f0 + (1.0 - f0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// ============================================================
// Normal Distribution Function — GGX (Trowbridge-Reitz)
// ============================================================
float D_GGX(float NdotH, float roughness) {
    float alpha = roughness * roughness;
    float alpha2 = alpha * alpha;
    float denom = NdotH * NdotH * (alpha2 - 1.0) + 1.0;
    return alpha2 / (HE_PI * denom * denom);
}

// ============================================================
// Geometry Function — Smith + Schlick-GGX
// ============================================================
float G_SchlickGGX(float NdotV, float roughness) {
    float k = (roughness + 1.0) * (roughness + 1.0) / 8.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}

float G_Smith(float NdotV, float NdotL, float roughness) {
    return G_SchlickGGX(NdotV, roughness) * G_SchlickGGX(NdotL, roughness);
}

// ============================================================
// PCF 阴影采样（Percentage-Closer Filtering）
//
// 3x3 核比较采样，返回 [0,1] 可见度。
// 1.0 = 完全照亮，0.0 = 完全遮蔽。
// ============================================================
float SampleShadowPCF(float3 worldPos, float3 worldNormal, float3 lightDir,
                       GPUShadowData sm, float3 cameraPos,
                       Texture2D<float> shadowMap0,
                       Texture2D<float> shadowMap1,
                       Texture2D<float> shadowMap2,
                       SamplerState shadowSampler)
{
    // CSM 级联选择
    float viewZ = dot(worldPos - cameraPos, sm.cameraForward.xyz);
    uint cascade = 0u;
    if (viewZ > sm.splitDistances[0]) cascade = 1u;
    if (viewZ > sm.splitDistances[1]) cascade = 2u;

    float4x4 lightViewProj = sm.lightViewProj[cascade];
    float3 biasedPos = worldPos + worldNormal * sm.shadowParams.y;

    float4 shadowClip = mul(lightViewProj, float4(biasedPos, 1.0));
    float3 ndc = shadowClip.xyz / shadowClip.w;
    float2 uv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    float  depth = ndc.z - sm.shadowParams.x;

    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0)
        return 1.0;

    float2 texelSize = 1.0 / 2048.0;
    float shadow = 0.0;
    if (cascade == 0u) {
        for (int x = -1; x <= 1; x++)
            for (int y = -1; y <= 1; y++)
                shadow += (depth <= shadowMap0.Sample(shadowSampler, uv + float2(x, y) * texelSize)) ? 1.0 : 0.0;
    } else if (cascade == 1u) {
        for (int x = -1; x <= 1; x++)
            for (int y = -1; y <= 1; y++)
                shadow += (depth <= shadowMap1.Sample(shadowSampler, uv + float2(x, y) * texelSize)) ? 1.0 : 0.0;
    } else {
        for (int x = -1; x <= 1; x++)
            for (int y = -1; y <= 1; y++)
                shadow += (depth <= shadowMap2.Sample(shadowSampler, uv + float2(x, y) * texelSize)) ? 1.0 : 0.0;
    }
    return shadow / 9.0;
}

// ============================================================
// 点光源阴影 — Cubemap 单次采样
//
// 从片段位置向光源方向采样 Cubemap，比较距离。
// 返回 [0,1] 可见度（1=完全照亮，0=完全遮蔽）。
// ============================================================
float SamplePointShadow(float3 worldPos, float3 lightPos, float lightRange,
                         GPUShadowData sm, TextureCube<float> shadowCube,
                         SamplerState shadowSampler)
{
    float3 toLight = lightPos - worldPos;
    float  dist    = length(toLight);
    float3 dir     = toLight / max(dist, 0.001);

    // 超出范围 → 无阴影
    if (dist > lightRange)
        return 1.0;

    // 深度采样（方向向量直接用作 Cubemap 纹理坐标）
    float shadowDepth = shadowCube.Sample(shadowSampler, dir).r;

    // 归一化距离用于比较：世界空间距离 / 光源范围
    float normalizedDist = dist / lightRange;

    // 应用深度偏移后比较
    return (normalizedDist - sm.shadowParams.x <= shadowDepth) ? 1.0 : 0.0;
}

// ============================================================
// PBR BRDF — Cook-Torrance + Lambertian
// ============================================================
float3 PBR_BRDF(float3 albedo, float metallic, float roughness,
                float3 N, float3 V, float3 L)
{
    float3 H = normalize(V + L);
    float NdotL = max(dot(N, L), 0.0);
    float NdotV = max(dot(N, V), 0.0);
    float NdotH = max(dot(N, H), 0.0);

    // 菲涅尔反射率：非金属 ~0.04，金属 = albedo
    float3 f0 = lerp(float3(0.04), albedo, metallic);

    // Cook-Torrance 镜面项
    float3 F = F_Schlick(f0, max(dot(H, V), 0.0));
    float  D = D_GGX(NdotH, roughness);
    float  G = G_Smith(NdotV, NdotL, roughness);

    float3 specular = (D * G * F) / max(4.0 * NdotV * NdotL, HE_EPSILON);

    // Lambertian 漫反射项（金属无漫反射）
    float3 kD = (1.0 - F) * (1.0 - metallic);
    float3 diffuse = kD * albedo / HE_PI;

    return (diffuse + specular) * NdotL;
}

#endif // HUGENGINE_PBR_COMMON_SLANG
