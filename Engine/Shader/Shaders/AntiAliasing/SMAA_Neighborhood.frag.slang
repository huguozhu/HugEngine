// SMAA_Neighborhood.frag.slang — SMAA 邻域混合 Pass
// 读取原始 LDR 颜色 + 混合权重，4 方向邻域加权混合 → 输出抗锯齿结果

struct VSOutput {
    float4 position : SV_Position;
    [[vk::location(0)]] float2 uv;
};

[[vk::binding(0, 0)]] Texture2D<float4> u_Input;       // 原始 LDR 颜色（Linear 采样）
[[vk::binding(0, 0)]] SamplerState      u_InputSampler;

[[vk::binding(1, 0)]] Texture2D<float4> u_BlendTex;    // RGBA8 混合权重（Point 采样）
[[vk::binding(1, 0)]] SamplerState      u_BlendSampler;

[[vk::push_constant]] cbuffer NeighborParams {
    float2 invScreenSize;
    float2 _pad;
};

float4 fragmentMain(VSOutput input) : SV_Target0 {
    float2 uv   = input.uv;
    float2 step = invScreenSize;

    // 读取混合权重
    float4 w = u_BlendTex.Sample(u_BlendSampler, uv);
    // w = (左权重, 右权重, 上权重, 下权重)

    // 读取中心颜色
    float4 center = u_Input.Sample(u_InputSampler, uv);

    // 如果所有权重都为零（非边缘像素），直接返回
    float totalW = w.r + w.g + w.b + w.a;
    if (totalW < 0.001)
        return center;

    // 读取 4 个邻域像素
    float4 left  = u_Input.Sample(u_InputSampler, uv + float2(-1,  0) * step);
    float4 right = u_Input.Sample(u_InputSampler, uv + float2( 1,  0) * step);
    float4 up    = u_Input.Sample(u_InputSampler, uv + float2( 0, -1) * step);
    float4 down  = u_Input.Sample(u_InputSampler, uv + float2( 0,  1) * step);

    // 加权混合：权重越大，邻域贡献越多（边缘方向上的邻域）
    float4 blended = center;
    blended = lerp(blended, left,  w.r);   // 向左混合
    blended = lerp(blended, right, w.g);   // 向右混合
    blended = lerp(blended, up,    w.b);   // 向上混合
    blended = lerp(blended, down,  w.a);   // 向下混合

    return blended;
}
