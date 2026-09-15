// BloomComposite.frag.slang — Bloom 合成（上采样 + 叠加回原始 HDR）
// 输入：全分辨率 HDR + 半分辨率模糊 Bloom → 输出：全分辨率 HDR+Bloom

struct VSOutput {
    float4 position : SV_Position;
    [[vk::location(0)]] float2 uv;
};

[[vk::binding(0, 0)]] Texture2D<float4> u_HDR;
[[vk::binding(0, 0)]] SamplerState      u_HDRSampler;
[[vk::binding(1, 0)]] Texture2D<float4> u_Bloom;
[[vk::binding(1, 0)]] SamplerState      u_BloomSampler;

[[vk::push_constant]] cbuffer BloomParams {
    float bloomIntensity;  // Bloom 强度倍率
    float _pad[3];
};

float4 fragmentMain(VSOutput input) : SV_Target0 {
    float4 hdr    = u_HDR.Sample(u_HDRSampler, input.uv);
    float4 bloom  = u_Bloom.Sample(u_BloomSampler, input.uv);  // 线性采样自动双线性上采样
    float3 result = hdr.rgb + bloom.rgb * bloomIntensity;
    return float4(result, hdr.a);
}
