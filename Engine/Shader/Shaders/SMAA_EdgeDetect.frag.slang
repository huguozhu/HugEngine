// SMAA_EdgeDetect.frag.slang — SMAA 边缘检测 Pass
// 基于亮度梯度的边缘检测，输出 R8 编码的边缘类型
//   R=0: 非边缘, R=0.5: 水平边缘, R=1.0: 垂直边缘

struct VSOutput {
    float4 position : SV_Position;
    [[vk::location(0)]] float2 uv;
};

[[vk::binding(0, 0)]] Texture2D<float4> u_Input;
[[vk::binding(0, 0)]] SamplerState      u_Sampler;

[[vk::push_constant]] cbuffer EdgeParams {
    float2 invScreenSize;  // 1/width, 1/height
    float2 _pad;
};

// 感知亮度（BT.709）
float Luma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

float4 fragmentMain(VSOutput input) : SV_Target0 {
    float2 uv  = input.uv;
    float2 step = invScreenSize;

    // 采样十字邻域
    float c  = Luma(u_Input.Sample(u_Sampler, uv).rgb);
    float l  = Luma(u_Input.Sample(u_Sampler, uv + float2(-1,  0) * step).rgb);
    float r  = Luma(u_Input.Sample(u_Sampler, uv + float2( 1,  0) * step).rgb);
    float t  = Luma(u_Input.Sample(u_Sampler, uv + float2( 0, -1) * step).rgb);
    float b  = Luma(u_Input.Sample(u_Sampler, uv + float2( 0,  1) * step).rgb);
    float tl = Luma(u_Input.Sample(u_Sampler, uv + float2(-1, -1) * step).rgb);
    float tr = Luma(u_Input.Sample(u_Sampler, uv + float2( 1, -1) * step).rgb);
    float bl = Luma(u_Input.Sample(u_Sampler, uv + float2(-1,  1) * step).rgb);
    float br = Luma(u_Input.Sample(u_Sampler, uv + float2( 1,  1) * step).rgb);

    // 水平梯度（沿 X 轴变化）
    float hEdge  = abs(l - c) * 2.0 + abs(r - c) * 2.0
                 + abs(tl - t) + abs(bl - b)
                 + abs(tr - t) + abs(br - b);
    // 垂直梯度（沿 Y 轴变化）
    float vEdge  = abs(t - c) * 2.0 + abs(b - c) * 2.0
                 + abs(tl - l) + abs(tr - r)
                 + abs(bl - l) + abs(br - r);

    // 归一化并阈值化
    float edgeMax = max(hEdge, vEdge);
    float threshold = 0.05;  // 亮度差阈值，低于此值视为平坦区域

    if (edgeMax < threshold)
        return float4(0.0, 0.0, 0.0, 0.0);  // 非边缘

    // 编码边缘类型：0=非边缘, 0.5=水平, 1.0=垂直
    float edgeType = (hEdge > vEdge) ? 1.0 : 0.5;

    return float4(edgeType, 0.0, 0.0, 0.0);
}
