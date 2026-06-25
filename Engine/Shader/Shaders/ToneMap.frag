// ============================================================
// ToneMap.frag — ACES Filmic Tone Mapping
//
// 读取 HDR 渲染目标，应用 Tone Mapping 后输出 sRGB
// ============================================================
#version 450
#extension GL_GOOGLE_include_directive : enable

#include "common.glsl"

// --- 输入 UV（来自全屏三角形）---
layout(location = 0) in vec2 inUV;

// --- HDR 输入纹理 ---
layout(set = 0, binding = 0) uniform sampler2D u_HDRTexture;

// --- LDR 输出 ---
layout(location = 0) out vec4 outColor;

// ============================================================
// ACES Filmic Tone Mapping（Fit 版本）
// 比标准 ACES 保留更多高光细节
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
    // 采样 HDR 颜色
    vec3 hdrColor = texture(u_HDRTexture, inUV).rgb;

    // ACES Tone Mapping
    vec3 mapped = ACESFilm(hdrColor);

    // 线性 → sRGB 伽马校正
    vec3 srgb = LinearToSRGB(mapped);

    outColor = vec4(srgb, 1.0);
}
