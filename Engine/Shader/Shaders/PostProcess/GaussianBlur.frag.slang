// GaussianBlur.frag.slang — 7×7 可分离高斯模糊（sigma=1.0）
// 用于 Bloom 后处理链路：BrightPass → GaussianBlur → Composite

struct VSOutput {
    float4 position : SV_Position;
    [[vk::location(0)]] float2 uv;
};

[[vk::binding(0, 0)]] Texture2D<float4> u_Input;
[[vk::binding(0, 0)]] SamplerState      u_Sampler;

[[vk::push_constant]] cbuffer BlurParams {
    float2 texelSize;   // 1/width, 1/height
    float2 _pad;
};

// 1D 高斯权重（sigma=1.0, radius=3，总和≈1.0）
static const float kWeights[7] = {
    0.00598f, 0.060626f, 0.241843f, 0.383103f, 0.241843f, 0.060626f, 0.00598f
};

float4 fragmentMain(VSOutput input) : SV_Target0 {
    float2 uv = input.uv;
    float4 result = float4(0.0, 0.0, 0.0, 0.0);

    // 7×7 可分离高斯核：w(x,y) = w[x] * w[y]
    for (int y = -3; y <= 3; y++) {
        float wy = kWeights[y + 3];
        for (int x = -3; x <= 3; x++) {
            float w = wy * kWeights[x + 3];
            float2 offset = float2(float(x), float(y)) * texelSize;
            result += u_Input.Sample(u_Sampler, uv + offset) * w;
        }
    }
    return result;
}
