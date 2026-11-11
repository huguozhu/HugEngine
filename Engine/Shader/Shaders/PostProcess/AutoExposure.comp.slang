// AutoExposure.comp.slang — 自动曝光（Compute Shader + SSBO reduction）
// 输入全屏 HDR → 组内 parallel reduction → 256 个 partial sum 写入 SSBO
// CPU 端 map SSBO 取平均 + 时间混合

#include "ShaderTypes.slang"    // HE_SAMPLE_L0
#include "MathCommon.slang"   // Luminance / sRGB 传递函数（唯一出处）
[[vk::binding(0, 0)]] Texture2D<float4>              u_HDR;
[[vk::binding(0, 0)]] SamplerState                     u_HDRSampler;
[[vk::binding(1, 0)]] RWStructuredBuffer<float>        u_Result;  // 256 floats

[[vk::push_constant]] cbuffer AEConst {
    float adaptSpeed;
    float targetLogLum;   // log2(targetLum)
    float deltaTime;
    uint  totalPixels;    // width * height（用于降采样）
};

groupshared float g_Sum[256];

[numthreads(16, 16, 1)]
void main(uint3 dtid : SV_DispatchThreadID, uint gi : SV_GroupIndex, uint3 gid : SV_GroupID) {
    // 固定 16×16 组 × 16×16 线程 = 256×256 线程，映射到全屏 [0,1]²（跨像素降采样），
    // 而不是硬编码 1920×1080（否则只采样纹理左上角，曝光值错误）
    float2 uv = (float2(dtid.xy) + 0.5) / float2(256.0, 256.0);
    // 【必须用显式 LOD】compute 着色器里用隐式导数采样（tex.Sample）会让 SPIR-V 带上
    // OpCapability ComputeDerivativeGroupQuadsKHR / ExecutionMode DerivativeGroupQuadsKHR，
    // 而 Mali 真机并不支持 computeDerivativeGroupQuads 特性（设备创建时该特性组被降级掉），
    // 于是 vkCreateComputePipelines 直接返回 VK_ERROR_INITIALIZATION_FAILED(-3)。
    // SampleLevel(..., 0) 语义等价（HDR 只有 1 级 mip），且不需要导数能力。
    float lum = Luminance(u_HDR.SampleLevel(u_HDRSampler, uv, 0).rgb);
    float logLum = log2(max(lum, 1e-5));

    // 组内 parallel reduction
    g_Sum[gi] = logLum;
    GroupMemoryBarrierWithGroupSync();
    for (uint s = 128; s > 0; s >>= 1) {
        if (gi < s) g_Sum[gi] += g_Sum[gi + s];
        GroupMemoryBarrierWithGroupSync();
    }

    // 每组写一个 partial sum 到 SSBO
    if (gi == 0) {
        uint groupIdx = gid.y * 16 + gid.x;  // row-major group index（16 组/行）
        if (groupIdx < 256) u_Result[groupIdx] = g_Sum[0] / 256.0;
    }
}
