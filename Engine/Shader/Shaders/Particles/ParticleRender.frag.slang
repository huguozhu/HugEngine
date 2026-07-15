// ParticleRender.frag — Billboard 粒子片元着色器
// 从 ColorOverLife / SizeOverLife 渐变纹理采样

#include "ParticleTypes.slang"

struct PSInput {
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
    float  lifeRatio : TEXCOORD1;
    uint   texIndex : TEXCOORD2;
    float3 worldPos : TEXCOORD3;
};

// Bindless 纹理数组 (复用 GBuffer set=0 中的 bindless)
[[vk::binding(5, 0)]] Texture2D u_Textures[] : register(t5, space0);
[[vk::binding(6, 0)]] SamplerState u_Samplers[] : register(s6, space0);

// 渐变纹理 (1D 采样: lifeRatio → color/size)
[[vk::binding(7, 0)]] Texture2D<float4> u_ColorOverLifeTex;   // RGBA8, 32×1
[[vk::binding(8, 0)]] Texture2D<float2> u_SizeOverLifeTex;    // RG32F, 32×1
[[vk::binding(9, 0)]] SamplerState u_GradientSampler;          // Linear clamp

[[vk::push_constant]] cbuffer PushConstant {
    GpuRenderParam u_RenderParam;
};

float4 fragmentMain(PSInput input) : SV_Target0 {
    // 1. 从渐变纹理采样颜色
    float2 gradientUV = float2(input.lifeRatio, 0.5);
    float4 color = u_ColorOverLifeTex.SampleLevel(u_GradientSampler, gradientUV, 0);

    // 2. 纹理子帧 UV 变换
    uint2 rowsCols = u_RenderParam.texRowsCols;
    uint totalFrames = max(rowsCols.x * rowsCols.y, 1u);
    uint frameIdx = input.texIndex % totalFrames;
    uint col = frameIdx % rowsCols.x;
    uint row = frameIdx / rowsCols.x;
    float2 subUV = (input.uv + float2(col, row)) / float2(rowsCols);

    // 3. 从 bindless 纹理采样 (如果有纹理)
    // 简化: 使用第一个纹理 (材质 ID = 0)
    float4 texColor = u_Textures[0].SampleLevel(u_Samplers[0], subUV, 0);
    color *= texColor;

    // 4. 软粒子 (基于深度比较, 后续添加)
    // float sceneDepth = u_SceneDepth.Load(int3(input.position.xy, 0));
    // float particleDepth = input.position.z;
    // float softAlpha = saturate((sceneDepth - particleDepth) * 10.0);
    // color.a *= softAlpha;

    // 5. Alpha test
    if (color.a < 0.01) discard;

    return color;
}
