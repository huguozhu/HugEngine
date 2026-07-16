// ParticleRender.frag — 软粒子 + 纹理采样 + ColorOverLife 渐变

#include "ParticleTypes.slang"

// 场景深度（软粒子）
[[vk::binding(3, 0)]] Texture2D<float> u_SceneDepth;
// 粒子圆形纹理
[[vk::binding(4, 0)]] Texture2D<float4> u_ParticleTex;
[[vk::binding(4, 0)]] SamplerState u_ParticleSampler;

[[vk::push_constant]] cbuffer PushConstant {
    GpuRenderParam u_RenderParam;
};

struct PSInput {
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
    float  lifeRatio : TEXCOORD1;
    uint   particleIdx : TEXCOORD2;
    float3 worldPos : TEXCOORD3;
};

float4 fragmentMain(PSInput input) : SV_Target0 {
    // 1. 采样粒子圆形纹理
    float4 texColor = u_ParticleTex.Sample(u_ParticleSampler, input.uv);

    // 2. ColorOverLife：startColor → endColor 按生命周期插值
    float4 lifeColor = lerp(u_RenderParam.startColor, u_RenderParam.endColor, input.lifeRatio);

    // 3. 组合纹理 + 颜色
    float4 color = texColor * lifeColor;

    // 4. 软粒子：读取场景深度，靠近几何体时淡出
    {
        float sceneDepth = u_SceneDepth.Load(int3(input.position.xy, 0));
        float particleDepth = input.position.z;
        float depthDiff = sceneDepth - particleDepth;
        float softFactor = saturate(depthDiff * 50.0);
        color.a *= softFactor;
    }

    if (color.a < 0.02) discard;
    return color;
}
