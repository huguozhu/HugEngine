// CameraEffects.frag.slang — 镜头后处理（胶片颗粒/晕影/色差/镜头畸变）
// 输入 LDR BGRA8 → 输出 LDR BGRA8

struct VSOutput {
    float4 position : SV_Position;
    [[vk::location(0)]] float2 uv;
};

[[vk::binding(0, 0)]] Texture2D<float4> u_Input;
[[vk::binding(0, 0)]] SamplerState      u_Sampler;

[[vk::push_constant]] cbuffer PostFXParams {
    float filmGrain;      // 胶片颗粒强度（0=关闭）
    float vignette;       // 晕影强度（0=关闭）
    float caStrength;     // 色差强度（0=关闭）
    float distortion;     // 镜头畸变系数（正=桶形，负=枕形，0=关闭）
    float time;           // 时间（颗粒动画种子）
    float _pad[3];
};

// 哈希噪声（胶片颗粒）
float hashNoise(float2 p) {
    return frac(sin(dot(p, float2(12.9898, 78.233))) * 43758.5453);
}

float4 fragmentMain(VSOutput input) : SV_Target0 {
    float2 center = float2(0.5, 0.5);
    float2 duv = input.uv - center;
    float r2 = dot(duv, duv);

    // 1. 镜头畸变：径向缩放 UV（桶形/枕形）
    float2 dUv = duv * (1.0 + distortion * r2);

    // 2. 色差：R/B 通道在畸变基础上向内外偏移
    float2 caR = dUv * (1.0 + caStrength);
    float2 caB = dUv * (1.0 - caStrength);
    float3 color;
    color.r = u_Input.Sample(u_Sampler, center + caR).r;
    color.g = u_Input.Sample(u_Sampler, center + dUv).g;
    color.b = u_Input.Sample(u_Sampler, center + caB).b;

    // 3. 晕影：边缘径向暗化（smoothstep 内径→外径）
    float d = length(duv) * 2.0;  // 中心 0 → 边缘 ~1.4
    float vig = 1.0 - vignette * smoothstep(0.4, 1.4, d);
    color *= vig;

    // 4. 胶片颗粒：时变哈希噪声
    color += (hashNoise(input.uv * 1000.0 + time) - 0.5) * filmGrain;

    return float4(clamp(color, 0.0, 1.0), 1.0);
}
