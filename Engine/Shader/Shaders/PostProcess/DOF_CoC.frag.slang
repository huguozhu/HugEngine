// DOF_CoC.frag.slang — 景深 Circle of Confusion 计算
// 从深度缓冲计算模糊半径，输出到 R16_FLOAT 纹理

struct VSOutput {
    float4 position : SV_Position;
    [[vk::location(0)]] float2 uv;
};

[[vk::binding(0, 0)]] Texture2D<float>  u_Depth;
[[vk::binding(0, 0)]] SamplerState      u_Sampler;

[[vk::push_constant]] cbuffer DOFParams {
    float focusDepth;     // 对焦深度（NDC 或线性深度）
    float focusRange;     // 过渡范围（越小景深越浅）
    float maxCoC;         // 最大模糊半径（通常 0.02 ~ 0.05）
    float _pad;
};

float fragmentMain(VSOutput input) : SV_Target0 {
    float depth = u_Depth.Sample(u_Sampler, input.uv).r;
    // 线性化深度（Vulkan NDC depth 0~1，reverse-Z 时 near=1 far=0）
    float coc = abs(depth - focusDepth) / max(focusRange, 0.0001);
    coc = saturate(coc) * maxCoC;
    return coc;
}
