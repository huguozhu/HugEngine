// ============================================================
// PBR.frag — PBR 渲染片元着色器
//
// Cook-Torrance BRDF + 方向光
// 支持 Alpha Mask（通过 alphaCutoff）
// ============================================================
#version 450
#extension GL_GOOGLE_include_directive : enable

#include "pbr_common.glsl"

// --- 输入来自顶点着色器 ---
layout(location = 0) in vec3 inWorldPos;
layout(location = 1) in vec3 inWorldNormal;
layout(location = 2) in vec2 inUV;

// --- Push constants（与顶点着色器共享，C++ 端 PushConstantData）---
layout(push_constant) uniform PushConstants {
    mat4 modelMatrix;          // [0..64]
    mat4 viewProjMatrix;       // [64..128]
    vec4 baseColorFactor;      // [128..144]
    float metallicFactor;       // [144]
    float roughnessFactor;      // [148]
    float aoFactor;             // [152]
    float alphaCutoff;          // [156]
    vec4 cameraPosition;        // [160..176]
    vec4 lightDirection;        // [176..192]  xyz=方向, w=强度
    vec4 lightColor;            // [192..208]  rgb=颜色, w=未使用
} u_Push;

// --- HDR 输出（RGBA16F 渲染目标）---
layout(location = 0) out vec4 outColor;

// ============================================================
// ACES Filmic Tone Mapping（HDR → LDR 显示）
// 参考: Narkowicz 2015 "ACES Filmic Tone Mapping Curve"
// ============================================================
vec3 ACESFilm(vec3 x) {
    float a = 2.51;
    float b = 0.03;
    float c = 2.43;
    float d = 0.59;
    float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

void main() {
    // 基础颜色（若有纹理则在此采样，Phase 1 先用纯色）
    vec3 albedo = u_Push.baseColorFactor.rgb;
    float alpha = u_Push.baseColorFactor.a;

    // Alpha Mask（透明度截断测试）
    if (alpha < u_Push.alphaCutoff) {
        discard;
    }

    // PBR 材质参数
    float metallic  = u_Push.metallicFactor;
    float roughness = clamp(u_Push.roughnessFactor, 0.04, 1.0);  // 最小粗糙度避免除零
    float ao        = u_Push.aoFactor;

    // 几何法线
    vec3 N = normalize(inWorldNormal);
    // 视线方向（片元 → 相机）
    vec3 V = normalize(u_Push.cameraPosition.xyz - inWorldPos);
    // 光线方向（片元 → 光源）
    vec3 L = normalize(-u_Push.lightDirection.xyz);

    // 光照强度
    float lightIntensity = u_Push.lightDirection.w;
    vec3  lightRadiance  = u_Push.lightColor.rgb * lightIntensity;

    // PBR BRDF 计算
    vec3 color = PBR_BRDF(albedo, metallic, roughness, N, V, L) * lightRadiance;

    // 环境光遮蔽
    color *= ao;

    // 简单环境光（模拟天空漫反射，避免完全黑的阴影面）
    vec3 ambient = albedo * 0.03 * ao;
    color += ambient;

    // HDR 输出（不做 Tonemapping，留给后期处理 Pass）
    // ACES Tone Mapping + sRGB output
    vec3 mapped = ACESFilm(color);
    vec3 srgb = LinearToSRGB(mapped);

    outColor = vec4(srgb, alpha);
}
