// ============================================================
// common.slang — HugEngine 共享着色器常量和工具函数
//
// 常量定义统一在 ShaderTypes.slang（kGPU* 前缀），
// C++ 与 Slang 共享同一份数据源，修改常量只需改一处。
// ============================================================
#ifndef HUGENGINE_COMMON_SLANG
#define HUGENGINE_COMMON_SLANG

#include "ShaderTypes.slang"  // GPU struct + 常量（kGPU*）共享定义
#include "MathCommon.slang"   // 纯数学/颜色工具（PI、sRGB、Luminance、ACES、PQ）
#include "TexBuckets.slang"   // D3D11 的纹理数组桶（只在 HE_SHADER_D3D11 下有内容）

// Bindless 纹理数组（全局共享，所有 mesh 共用 set=0）
// ---- D3D12 专用：register() 只接受 token，故另定一组宏 ----
// Vulkan 侧仍用 kGPUBinding_* / kGPUDescSet_*，两者干互不影响
// D3D12 不允许同一 space 内有多个无界数组，space2 里各 bindless 数组各占一段定长区间：
//   纹理/采样器 t0..t1023 / s0..s1023（1024 = 采样器堆尾部 bindless 保留区的大小）
//   实例变换 t1024、可见实例索引 t1280、骨骼矩阵 t1536、材质 t1792（各 256 个；此前各 64 个，
//   缓冲句柄超过 64 就越界写进相邻区间）
// 之前纹理只有 64 个，Sponza 这类场景纹理句柄达四百多，越界后落到后面的缓冲区间，采样到错误资源。
#if defined(HE_SHADER_HLSL)   // D3D 后端（D3D11 / D3D12）用定长数组；D3D11 的 bindless 替代方案见 D3D11 后端实现规划 M3
  #define HE_BINDLESS_DIM 1024
  #define HE_BINDLESS_SSBO_DIM 256
#else
  #define HE_BINDLESS_DIM
  #define HE_BINDLESS_SSBO_DIM
#endif
#define kGPUBinding_BindlessTextures_hlsl t0
#define kGPUBinding_BindlessSamplers_hlsl s0
#define kGPUBinding_BindlessSSBO_hlsl     t1792
#define kGPUDescSet_Bindless_hlsl         space2

#if defined(HE_SHADER_D3D11)
// ---- D3D11（SM 5.0）：不能用运行时句柄索引资源数组（D3D11 后端实现规划 3.2）----
// 材质的 4 张纹理 / 4 个采样器绑在固定槽位，由渲染层每次绘制前用
// SetNonBindlessHandle(kGPUBinding_NB_MaterialTextures, 材质纹理基址) 指定；
// 材质参数缓冲同理（kGPUBinding_NB_Materials）。
[[vk::binding(kGPUBinding_NB_MaterialTextures + 0, kGPUDescSet_PerFrame)]] Texture2D<float4> u_MatTex0;
[[vk::binding(kGPUBinding_NB_MaterialTextures + 1, kGPUDescSet_PerFrame)]] Texture2D<float4> u_MatTex1;
[[vk::binding(kGPUBinding_NB_MaterialTextures + 2, kGPUDescSet_PerFrame)]] Texture2D<float4> u_MatTex2;
[[vk::binding(kGPUBinding_NB_MaterialTextures + 3, kGPUDescSet_PerFrame)]] Texture2D<float4> u_MatTex3;
[[vk::binding(kGPUBinding_NB_MaterialSamplers + 0, kGPUDescSet_PerFrame)]] SamplerState u_MatSmp0;
[[vk::binding(kGPUBinding_NB_MaterialSamplers + 1, kGPUDescSet_PerFrame)]] SamplerState u_MatSmp1;
[[vk::binding(kGPUBinding_NB_MaterialSamplers + 2, kGPUDescSet_PerFrame)]] SamplerState u_MatSmp2;
[[vk::binding(kGPUBinding_NB_MaterialSamplers + 3, kGPUDescSet_PerFrame)]] SamplerState u_MatSmp3;
[[vk::binding(kGPUBinding_NB_Materials, kGPUDescSet_PerFrame)]] StructuredBuffer<GPUMaterialData> u_Materials;

/// 材质第 slot 张纹理采样（slot 在调用点是编译期常量，内联后 switch 被折叠成单次采样）
float4 HE_D3D11_SampleMaterial(uint slot, float2 uv) {
    switch (slot) {
        case 0:  return u_MatTex0.Sample(u_MatSmp0, uv);
        case 1:  return u_MatTex1.Sample(u_MatSmp1, uv);
        case 2:  return u_MatTex2.Sample(u_MatSmp2, uv);
        default: return u_MatTex3.Sample(u_MatSmp3, uv);
    }
}
// 【纹理数组桶（M6）】先按句柄查桶：已分桶就从纹理数组采样（一次绘制覆盖多个材质的 GPU 驱动路径
// 也能取到正确纹理）；未分桶的退回逐绘制绑定的固定槽位（渲染层交给后端的基址）。
// 导数在分支外取好，桶号是动态值，switch 里用 SampleGrad
float4 HE_D3D11_SampleMaterialBucket(uint base, uint slot, float2 uv) {
    const float2 dx = ddx(uv);
    const float2 dy = ddy(uv);
    uint  bucket;
    float layer;
    if (HE_TexBucketLookup(base + slot, bucket, layer)) return HE_TexBucketSampleGrad(bucket, float3(uv, layer), dx, dy);
    return HE_D3D11_SampleMaterial(slot, uv);
}
float4 HE_D3D11_SampleHandle(uint handle, float2 uv) {
    const float2 dx = ddx(uv);
    const float2 dy = ddy(uv);
    uint  bucket;
    float layer;
    if (HE_TexBucketLookup(handle, bucket, layer)) return HE_TexBucketSampleGrad(bucket, float3(uv, layer), dx, dy);
    return u_MatTex0.Sample(u_MatSmp0, uv);
}
#define HE_MAT_SAMPLE(base, slot, uv) HE_D3D11_SampleMaterialBucket(base, slot, uv)
#define HE_TEX_SAMPLE(handle, uv)     HE_D3D11_SampleHandle(handle, uv)
#else
[[vk::binding(kGPUBinding_BindlessTextures, kGPUDescSet_PerFrame)]] Texture2D<float4> u_Textures[HE_BINDLESS_DIM] : register(kGPUBinding_BindlessTextures_hlsl, kGPUDescSet_Bindless_hlsl);
[[vk::binding(kGPUBinding_BindlessSamplers, kGPUDescSet_PerFrame)]] SamplerState      u_Samplers[HE_BINDLESS_DIM] : register(kGPUBinding_BindlessSamplers_hlsl, kGPUDescSet_Bindless_hlsl);

// Bindless 材质 SSBO 数组（StructuredBuffer<GPUMaterialData>，binding 30，per-material 数据）
// 索引 = materialID >> 2（每材质占 4 个纹理槽）
[[vk::binding(kGPUBinding_BindlessSSBO, kGPUDescSet_PerFrame)]] StructuredBuffer<GPUMaterialData> u_Materials[HE_BINDLESS_SSBO_DIM] : register(kGPUBinding_BindlessSSBO_hlsl, kGPUDescSet_Bindless_hlsl);

// 材质纹理采样：展开后与原写法逐字相同（u_Textures[基址 + 槽位].Sample(u_Samplers[基址 + 槽位], uv)）
#define HE_MAT_SAMPLE(base, slot, uv) u_Textures[base + slot].Sample(u_Samplers[base + slot], uv)
#define HE_TEX_SAMPLE(handle, uv)     u_Textures[handle].Sample(u_Samplers[handle], uv)
#endif

#endif // HUGENGINE_COMMON_SLANG
