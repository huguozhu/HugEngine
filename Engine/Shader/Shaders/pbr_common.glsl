// ============================================================
// pbr_common.glsl — PBR BRDF 函数库
//
// 基于 glTF 2.0 Metallic-Roughness 模型
// Cook-Torrance 微面元镜面 BRDF + Lambertian 漫反射
// ============================================================
#ifndef HUGENGINE_PBR_COMMON_GLSL
#define HUGENGINE_PBR_COMMON_GLSL

#include "common.glsl"

// ============================================================
// Fresnel — Schlick 近似
// f0: 垂直入射反射率
// f90: 掠射角反射率（通常为 1.0）
// cosTheta: NdotV 或 NdotL（钳制到 [0,1]）
// ============================================================
vec3 F_Schlick(vec3 f0, float cosTheta) {
    return f0 + (1.0 - f0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// ============================================================
// Normal Distribution Function — GGX (Trowbridge-Reitz)
// NdotH: 法线与半角向量夹角余弦
// roughness: 粗糙度（α² = roughness⁴ 以获得感知线性）
// ============================================================
float D_GGX(float NdotH, float roughness) {
    float alpha = roughness * roughness;
    float alpha2 = alpha * alpha;
    float denom = NdotH * NdotH * (alpha2 - 1.0) + 1.0;
    return alpha2 / (HE_PI * denom * denom);
}

// ============================================================
// Geometry Function — Smith with Schlick-GGX
// NdotV / NdotL: 法线与视线/光线方向夹角余弦
// roughness: 粗糙度
// ============================================================
float G_SchlickGGX(float NdotV, float roughness) {
    float k = (roughness + 1.0) * (roughness + 1.0) / 8.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}

float G_Smith(float NdotV, float NdotL, float roughness) {
    return G_SchlickGGX(NdotV, roughness) * G_SchlickGGX(NdotL, roughness);
}

// ============================================================
// PBR BRDF — Cook-Torrance 微面元模型
//
// albedo:    基础色（sRGB → 线性）
// metallic:  金属度 [0,1]
// roughness: 粗糙度 [0,1]
// N:         世界空间法线
// V:         视线方向（指向相机）
// L:         光线方向（指向光源）
//
// 返回: (diffuse + specular) * NdotL
// ============================================================
vec3 PBR_BRDF(vec3 albedo, float metallic, float roughness,
              vec3 N, vec3 V, vec3 L)
{
    vec3 H = normalize(V + L);
    float NdotL = max(dot(N, L), 0.0);
    float NdotV = max(dot(N, V), 0.0);
    float NdotH = max(dot(N, H), 0.0);

    // 菲涅尔反射率：非金属 ~0.04，金属 = albedo
    vec3 f0 = mix(vec3(0.04), albedo, metallic);

    // Cook-Torrance 镜面项
    vec3  F = F_Schlick(f0, max(dot(H, V), 0.0));
    float D = D_GGX(NdotH, roughness);
    float G = G_Smith(NdotV, NdotL, roughness);

    vec3 specular = (D * G * F) / max(4.0 * NdotV * NdotL, HE_EPSILON);

    // Lambertian 漫反射项（金属无漫反射）
    vec3 kD = (1.0 - F) * (1.0 - metallic);
    vec3 diffuse = kD * albedo / HE_PI;

    return (diffuse + specular) * NdotL;
}

#endif // HUGENGINE_PBR_COMMON_GLSL
