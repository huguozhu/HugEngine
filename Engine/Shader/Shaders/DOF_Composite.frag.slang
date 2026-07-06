// DOF_Composite.frag.slang — 景深合成（CoC 驱动的 lerp 混合）

struct VSOutput {
    float4 position : SV_Position;
    [[vk::location(0)]] float2 uv;
};

[[vk::binding(0, 0)]] Texture2D<float4> u_Sharp;      // 原始清晰 HDR
[[vk::binding(0, 0)]] SamplerState      u_SharpSampler;
[[vk::binding(1, 0)]] Texture2D<float4> u_Blurred;    // 高斯模糊后的 HDR
[[vk::binding(1, 0)]] SamplerState      u_BlurredSampler;
[[vk::binding(2, 0)]] Texture2D<float>  u_CoC;        // CoC 模糊半径
[[vk::binding(2, 0)]] SamplerState      u_CoCSampler;

[[vk::push_constant]] cbuffer CompositeParams {
    float dofIntensity;   // 景深强度倍率
    float _pad[3];
};

float4 fragmentMain(VSOutput input) : SV_Target0 {
    float4 sharp   = u_Sharp.Sample(u_SharpSampler, input.uv);
    float4 blurred = u_Blurred.Sample(u_BlurredSampler, input.uv);
    float  coc     = u_CoC.Sample(u_CoCSampler, input.uv).r * dofIntensity;
    float  t       = saturate(coc);
    return lerp(sharp, blurred, t);
}
