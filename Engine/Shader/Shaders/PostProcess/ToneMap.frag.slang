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
    float exposure;       // 自动曝光值（默认 1.0 = 无调整）
    float hdrEnabled;     // 1=HDR10（PQ 输出），0=SDR（sRGB 输出）
    float whitePointNits; // HDR 参考白点（尼特），相对线性亮度 → 绝对尼特
    float _pad;
};

float4 fragmentMain(FSInput input) : SV_Target0 {
    // 采样 HDR 颜色 + 应用曝光
    float3 hdrColor = u_HDRTexture.Sample(u_HDRSampler, input.uv).rgb * exposure;

    if (hdrEnabled > 0.5) {
        // HDR10：ACES → BT.2020 广色域 → PQ 编码（10-bit 非线性）
        float3 mapped  = ACESFilm(hdrColor);
        float3 rec2020 = BT709ToBT2020(mapped);
        float3 nits    = rec2020 * whitePointNits;  // 相对亮度 → 绝对尼特
        return float4(LinearToPQ(nits), 1.0);
    }

    // SDR：ACES → sRGB（保持原有行为）
    float3 mapped = ACESFilm(hdrColor);
    float3 srgb = LinearToSRGB(mapped);
    return float4(srgb, 1.0);
}
