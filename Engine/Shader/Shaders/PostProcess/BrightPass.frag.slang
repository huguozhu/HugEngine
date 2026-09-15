// BrightPass.frag.slang — Bloom 亮度提取（阈值 + 降采样到半分辨率）
// 输入全分辨率 HDR → 输出半分辨率 bright 纹理

struct VSOutput {
    float4 position : SV_Position;
    [[vk::location(0)]] float2 uv;
};

[[vk::binding(0, 0)]] Texture2D<float4> u_HDR;
[[vk::binding(0, 0)]] SamplerState      u_Sampler;

[[vk::push_constant]] cbuffer BrightParams {
    float threshold;    // 亮度阈值（低于此值的像素不参与 Bloom）
    float _pad[3];
};

// 亮度计算（Rec.709 系数）
float Luminance(float3 c) {
    return dot(c, float3(0.2126, 0.7152, 0.0722));
}

float4 fragmentMain(VSOutput input) : SV_Target0 {
    // 4 像素 box downsample（半分辨率）+ 亮度阈值
    float2 ts = float2(1.0 / 1920.0, 1.0 / 1080.0);  // 由 push constant 传入？
    float3 center = u_HDR.Sample(u_Sampler, input.uv).rgb;
    float luma = Luminance(center);
    float3 bright = center * smoothstep(threshold, threshold * 2.0, luma);
    return float4(bright, 1.0);
}
