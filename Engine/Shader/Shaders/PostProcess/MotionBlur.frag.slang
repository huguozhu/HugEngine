// MotionBlur.frag.slang — 运动模糊（velocity-based 方向采样）
// 沿屏幕空间速度向量采样 N 次取平均

struct VSOutput {
    float4 position : SV_Position;
    [[vk::location(0)]] float2 uv;
};

[[vk::binding(0, 0)]] Texture2D<float4> u_HDR;
[[vk::binding(0, 0)]] SamplerState      u_HDRSampler;
[[vk::binding(1, 0)]] Texture2D<float2> u_Velocity;   // GBufferD: UV 空间运动矢量
[[vk::binding(1, 0)]] SamplerState      u_VelSampler;

[[vk::push_constant]] cbuffer MBParams {
    float intensity;      // 运动模糊强度
    uint  sampleCount;    // 采样次数（建议 8 ~ 16）
    float2 _pad;
};

static const uint kMaxSamples = 16;

float4 fragmentMain(VSOutput input) : SV_Target0 {
    float2 velocity = u_Velocity.Sample(u_VelSampler, input.uv).xy * intensity;
    float4 result   = u_HDR.Sample(u_HDRSampler, input.uv);
    float  weight   = 1.0;

    // 沿速度向量采样
    uint n = min(sampleCount, kMaxSamples);
    for (uint i = 1; i <= n; i++) {
        float t = float(i) / float(n);
        float2 offset = velocity * t;
        result += u_HDR.Sample(u_HDRSampler, input.uv + offset);
        result += u_HDR.Sample(u_HDRSampler, input.uv - offset);
        weight += 2.0;
    }
    return result / weight;
}
