// AutoExposure.comp.slang — 自动曝光（Compute Shader + SSBO reduction）
// 输入全屏 HDR → 组内 parallel reduction → 256 个 partial sum 写入 SSBO
// CPU 端 map SSBO 取平均 + 时间混合

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

float Luminance(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

[numthreads(16, 16, 1)]
void main(uint3 dtid : SV_DispatchThreadID, uint gi : SV_GroupIndex, uint3 gid : SV_GroupID) {
    // 每个线程处理一个像素（16×16 = 256 线程/组，覆盖全屏需要 ceil(W/16)×ceil(H/16) 组）
    float2 uv = (float2(dtid.xy) + 0.5) / float2(1920.0, 1080.0);
    float lum = Luminance(u_HDR.Sample(u_HDRSampler, uv).rgb);
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
        uint groupIdx = gid.y * ((1920 + 15) / 16) + gid.x;  // row-major group index
        if (groupIdx < 256) u_Result[groupIdx] = g_Sum[0] / 256.0;
    }
}
