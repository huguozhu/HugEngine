// ============================================================
// ToneMap.frag.slang — ACES Filmic Tone Mapping
//
// 读取 HDR 渲染目标，应用 Tone Mapping 后输出 sRGB
// ============================================================

#include "common.slang"

// --- HDR 输入纹理 ---
[[vk::binding(0, 0)]] Texture2D u_HDRTexture;
[[vk::binding(0, 0)]] SamplerState u_HDRSampler;  // 同 binding = CombinedImageSampler

struct FSInput {
    float2 uv : TEXCOORD0;
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
    // 采样 HDR 颜色
    float3 hdrColor = u_HDRTexture.Sample(u_HDRSampler, input.uv).rgb;

    // ACES Tone Mapping
    float3 mapped = ACESFilm(hdrColor);

    // 线性 → sRGB
    float3 srgb = LinearToSRGB(mapped);
    return float4(srgb, 1.0);
}
