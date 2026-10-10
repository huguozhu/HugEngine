// BrightPass.frag.slang — Bloom 亮度提取（阈值 + 降采样到半分辨率）
// 输入全分辨率 HDR → 输出半分辨率 bright 纹理

#include "MathCommon.slang"   // Luminance / sRGB 传递函数（唯一出处）
#include "FullscreenVS.slang"   // VSOutput / VSOutputWorldDir（接口唯一出处）

[[vk::binding(0, 0)]] Texture2D<float4> u_HDR;
[[vk::binding(0, 0)]] SamplerState      u_Sampler;

[[vk::push_constant]] cbuffer BrightParams {
    float threshold;    // 亮度阈值（低于此值的像素不参与 Bloom）
    float _pad[3];
};

float4 fragmentMain(VSOutput input) : SV_Target0 {
    // 4 像素 box downsample（半分辨率）+ 亮度阈值
    float2 ts = float2(1.0 / 1920.0, 1.0 / 1080.0);  // 由 push constant 传入？
    float3 center = u_HDR.Sample(u_Sampler, input.uv).rgb;
    float luma = Luminance(center);
    float3 bright = center * smoothstep(threshold, threshold * 2.0, luma);
    return float4(bright, 1.0);
}
