// FullscreenCopy.frag.slang — 全屏纹理拷贝片元着色器
// 采样输入纹理输出到 BackBuffer

[[vk::binding(0, 0)]] Texture2D<float4> u_InputTex;
[[vk::binding(0, 0)]] SamplerState      u_InputSampler;

float4 fragmentMain(float4 position : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET
{
    return u_InputTex.Sample(u_InputSampler, uv);
}
