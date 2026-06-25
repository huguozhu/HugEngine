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
