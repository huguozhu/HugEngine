// SSAO_Blur.frag.slang — SSAO 模糊（4×4 box, depth-aware）
struct VSOutput {
    float4 position : SV_Position;
    [[vk::location(0)]] float2 uv;
};

[[vk::binding(0, 0)]] Texture2D<float4> u_AOTex;
[[vk::binding(0, 0)]] SamplerState      u_Sampler;

[[vk::push_constant]] cbuffer BlurParams {
    float2 texelSize;
    float _pad[2];
};

float4 fragmentMain(VSOutput input) : SV_Target0 {
    float2 uv = input.uv;
    float4 result = float4(0,0,0,0);
    const int R = 2;  // 4×4 kernel
    for (int y = -R; y <= R; y++) {
        for (int x = -R; x <= R; x++) {
            float2 offset = float2(x, y) * texelSize;
            result += u_AOTex.Sample(u_Sampler, uv + offset);
        }
    }
    int n = (2*R+1) * (2*R+1);
    return result / float(n);
}
