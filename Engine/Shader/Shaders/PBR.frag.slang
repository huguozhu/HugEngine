// ============================================================
// PBR.frag.slang — PBR 渲染片元着色器
//
// Cook-Torrance BRDF + 方向光 + 内联 ACES ToneMapping
// ============================================================

#include "pbr_common.slang"

// --- 输入来自顶点着色器 ---
struct FSInput {
    [[vk::location(0)]] float3 worldPos;
    [[vk::location(1)]] float3 worldNormal;
    [[vk::location(2)]] float2 uv;
};

// --- Push constants（与顶点着色器共享布局）---
[[vk::push_constant]] cbuffer PushConstants {
    float4x4 modelMatrix;       // [0..64]
    float4x4 viewProjMatrix;    // [64..128]
    float4   baseColorFactor;   // [128..144]
    float    metallicFactor;    // [144]
    float    roughnessFactor;   // [148]
    float    aoFactor;          // [152]
    float    alphaCutoff;       // [156]
    float4   cameraPosition;    // [160..176]
    float4   lightDirection;    // [176..192]
    float4   lightColor;        // [192..208]
};

// ============================================================
// ACES Filmic Tone Mapping
// ============================================================
float3 ACESFilm(float3 x) {
    float a = 2.51;
    float b = 0.03;
    float c = 2.43;
    float d = 0.59;
    float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

float4 fragmentMain(FSInput input) : SV_Target0 {
    // 基础颜色
    float3 albedo = baseColorFactor.rgb;
    float alpha   = baseColorFactor.a;

    // Alpha Mask
    if (alpha < alphaCutoff) discard;

    // PBR 材质参数
    float metallic  = metallicFactor;
    float roughness = clamp(roughnessFactor, 0.04, 1.0);
    float ao        = aoFactor;

    // 几何法线 + 视线/光线方向
    float3 N = normalize(input.worldNormal);
    float3 V = normalize(cameraPosition.xyz - input.worldPos);
    float3 L = normalize(-lightDirection.xyz);

    // 光照
    float lightIntensity = lightDirection.w;
    float3 lightRadiance = lightColor.rgb * lightIntensity;

    // PBR BRDF
    float3 color = PBR_BRDF(albedo, metallic, roughness, N, V, L) * lightRadiance;
    color *= ao;
    color += albedo * 0.03 * ao;  // 简单环境光

    // ACES ToneMapping + sRGB 输出
    float3 mapped = ACESFilm(color);
    float3 srgb   = LinearToSRGB(mapped);
    return float4(srgb, alpha);
}
