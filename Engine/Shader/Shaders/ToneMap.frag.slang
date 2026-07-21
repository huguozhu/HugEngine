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

// ACESFilm 定义在 common.slang 中，此处直接使用

[[vk::push_constant]] cbuffer ToneMapPC {
    float exposure;   // 自动曝光值（默认 1.0 = 无调整）
    float _pad[3];
};

float4 fragmentMain(FSInput input) : SV_Target0 {
    // 采样 HDR 颜色 + 应用曝光
    float3 hdrColor = u_HDRTexture.Sample(u_HDRSampler, input.uv).rgb * exposure;

    // ACES Tone Mapping
    float3 mapped = ACESFilm(hdrColor);

    // 线性 → sRGB
    float3 srgb = LinearToSRGB(mapped);
    return float4(srgb, 1.0);
}
