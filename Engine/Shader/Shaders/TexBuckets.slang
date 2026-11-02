// ============================================================================
//  TexBuckets.slang —— D3D11 的纹理数组桶（D3D11 后端实现规划 M6）
//
//  SM 5.0 不能用运行时句柄索引资源数组。D3D11 后端把 bindless 堆里的静态纹理按
//  （宽, 高, mip 数, 格式）拷进 ≤ 8 个 Texture2DArray，并提供「句柄 → (1 << 31 | 桶号 << 24 | 层号)」
//  映射表；这里按句柄查表、对字面量下标的桶采样。表里为 0（未分桶 / 空表 / 越界）时返回 false，
//  调用方退回逐绘制绑定的纹理。
//  只在 HE_SHADER_D3D11 下有内容：Vulkan / D3D12 包含本文件不产生任何代码。
// ============================================================================
#ifndef HUGENGINE_TEX_BUCKETS_SLANG
#define HUGENGINE_TEX_BUCKETS_SLANG

#include "ShaderTypes.slang"

#if defined(HE_SHADER_D3D11)
[[vk::binding(kGPUBinding_NB_TexBuckets + 0, kGPUDescSet_PerFrame)]] Texture2DArray<float4> u_TexBucket0;
[[vk::binding(kGPUBinding_NB_TexBuckets + 1, kGPUDescSet_PerFrame)]] Texture2DArray<float4> u_TexBucket1;
[[vk::binding(kGPUBinding_NB_TexBuckets + 2, kGPUDescSet_PerFrame)]] Texture2DArray<float4> u_TexBucket2;
[[vk::binding(kGPUBinding_NB_TexBuckets + 3, kGPUDescSet_PerFrame)]] Texture2DArray<float4> u_TexBucket3;
[[vk::binding(kGPUBinding_NB_TexBuckets + 4, kGPUDescSet_PerFrame)]] Texture2DArray<float4> u_TexBucket4;
[[vk::binding(kGPUBinding_NB_TexBuckets + 5, kGPUDescSet_PerFrame)]] Texture2DArray<float4> u_TexBucket5;
[[vk::binding(kGPUBinding_NB_TexBuckets + 6, kGPUDescSet_PerFrame)]] Texture2DArray<float4> u_TexBucket6;
[[vk::binding(kGPUBinding_NB_TexBuckets + 7, kGPUDescSet_PerFrame)]] Texture2DArray<float4> u_TexBucket7;
[[vk::binding(kGPUBinding_NB_TexBucketMap, kGPUDescSet_PerFrame)]]     StructuredBuffer<uint>  u_TexBucketMap;
[[vk::binding(kGPUBinding_NB_TexBucketSampler, kGPUDescSet_PerFrame)]] SamplerState            u_TexBucketSampler;

/// 查映射表：已分桶时给出桶号与层号
bool HE_TexBucketLookup(uint handle, out uint bucket, out float layer) {
    const uint e = u_TexBucketMap[handle];
    bucket = (e >> 24) & 0x7Fu;
    layer  = float(e & 0xFFFFFFu);
    return (e & 0x80000000u) != 0u;
}

/// 片元着色器用：导数在调用处取好（桶号是动态值，switch 分支里不能再算隐式导数）
float4 HE_TexBucketSampleGrad(uint bucket, float3 uvw, float2 dx, float2 dy) {
    switch (bucket) {
        case 0:  return u_TexBucket0.SampleGrad(u_TexBucketSampler, uvw, dx, dy);
        case 1:  return u_TexBucket1.SampleGrad(u_TexBucketSampler, uvw, dx, dy);
        case 2:  return u_TexBucket2.SampleGrad(u_TexBucketSampler, uvw, dx, dy);
        case 3:  return u_TexBucket3.SampleGrad(u_TexBucketSampler, uvw, dx, dy);
        case 4:  return u_TexBucket4.SampleGrad(u_TexBucketSampler, uvw, dx, dy);
        case 5:  return u_TexBucket5.SampleGrad(u_TexBucketSampler, uvw, dx, dy);
        case 6:  return u_TexBucket6.SampleGrad(u_TexBucketSampler, uvw, dx, dy);
        default: return u_TexBucket7.SampleGrad(u_TexBucketSampler, uvw, dx, dy);
    }
}

/// 计算着色器用：显式 LOD
float4 HE_TexBucketSampleLevel(uint bucket, float3 uvw, float lod) {
    switch (bucket) {
        case 0:  return u_TexBucket0.SampleLevel(u_TexBucketSampler, uvw, lod);
        case 1:  return u_TexBucket1.SampleLevel(u_TexBucketSampler, uvw, lod);
        case 2:  return u_TexBucket2.SampleLevel(u_TexBucketSampler, uvw, lod);
        case 3:  return u_TexBucket3.SampleLevel(u_TexBucketSampler, uvw, lod);
        case 4:  return u_TexBucket4.SampleLevel(u_TexBucketSampler, uvw, lod);
        case 5:  return u_TexBucket5.SampleLevel(u_TexBucketSampler, uvw, lod);
        case 6:  return u_TexBucket6.SampleLevel(u_TexBucketSampler, uvw, lod);
        default: return u_TexBucket7.SampleLevel(u_TexBucketSampler, uvw, lod);
    }
}
#endif // HE_SHADER_D3D11

#endif // HUGENGINE_TEX_BUCKETS_SLANG
