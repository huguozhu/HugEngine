// FXAA.frag.slang — Fast Approximate Anti-Aliasing
// 简化的 FXAA 3.11 算法：LDR 单 Pass，基于亮度边缘检测+混合

struct VSOutput {
    float4 position : SV_Position;
    [[vk::location(0)]] float2 uv;
};

[[vk::binding(0, 0)]] Texture2D<float4> u_Input;
[[vk::binding(0, 0)]] SamplerState      u_Sampler;

[[vk::push_constant]] cbuffer FXAAParams {
    float2 invScreenSize;  // 1/width, 1/height
    float2 _pad;
};

// RGB → 感知亮度
float Luma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }

float4 fragmentMain(VSOutput input) : SV_Target0 {
    float2 uv = input.uv;
    float2 step = invScreenSize;

    // 采样 3×3 邻域
    float3 c00 = u_Input.Sample(u_Sampler, uv + float2(-1,-1)*step).rgb;
    float3 c10 = u_Input.Sample(u_Sampler, uv + float2( 0,-1)*step).rgb;
    float3 c20 = u_Input.Sample(u_Sampler, uv + float2( 1,-1)*step).rgb;
    float3 c01 = u_Input.Sample(u_Sampler, uv + float2(-1, 0)*step).rgb;
    float3 c11 = u_Input.Sample(u_Sampler, uv).rgb;
    float3 c21 = u_Input.Sample(u_Sampler, uv + float2( 1, 0)*step).rgb;
    float3 c02 = u_Input.Sample(u_Sampler, uv + float2(-1, 1)*step).rgb;
    float3 c12 = u_Input.Sample(u_Sampler, uv + float2( 0, 1)*step).rgb;
    float3 c22 = u_Input.Sample(u_Sampler, uv + float2( 1, 1)*step).rgb;

    // 计算亮度
    float l00=Luma(c00),l10=Luma(c10),l20=Luma(c20);
    float l01=Luma(c01),l11=Luma(c11),l21=Luma(c21);
    float l02=Luma(c02),l12=Luma(c12),l22=Luma(c22);

    // 边缘检测：水平和垂直梯度
    float hGrad = abs(l01+l21-2*l11) * 0.5 + abs(l00+l20-2*l10)*0.25 + abs(l02+l22-2*l12)*0.25;
    float vGrad = abs(l10+l12-2*l11) * 0.5 + abs(l00+l02-2*l01)*0.25 + abs(l20+l22-2*l21)*0.25;

    // 边缘强度
    float edge = max(hGrad, vGrad);
    if (edge < 0.05) return float4(c11, 1.0);  // 平坦区域，不处理

    // 沿梯度方向混合
    float3 blended = c11;
    if (hGrad > vGrad) {
        // 水平边缘：沿垂直方向混合
        blended = (c01 + c11 + c21) / 3.0;
        float t = saturate(abs(l01-l21) / max(edge, 0.001));
        blended = lerp(blended, c11, t * 0.5);
    } else {
        // 垂直边缘：沿水平方向混合
        blended = (c10 + c11 + c12) / 3.0;
        float t = saturate(abs(l10-l12) / max(edge, 0.001));
        blended = lerp(blended, c11, t * 0.5);
    }

    return float4(blended, 1.0);
}
