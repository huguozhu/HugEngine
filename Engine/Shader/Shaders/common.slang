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

// Bindless 纹理数组（全局共享，所有 mesh 共用 set=0）
// ---- D3D12 专用：register() 只接受 token，故另定一组宏 ----
// Vulkan 侧仍用 kGPUBinding_* / kGPUDescSet_*，两者干互不影响
// D3D12 不允许同一 space 内有多个无界数组，space2 里各 bindless 数组各占一段定长区间：
//   纹理/采样器 t0..t1023 / s0..s1023（1024 = 采样器堆尾部 bindless 保留区的大小）
//   实例变换 t1024、可见实例索引 t1280、骨骼矩阵 t1536、材质 t1792（各 256 个；此前各 64 个，
//   缓冲句柄超过 64 就越界写进相邻区间）
// 之前纹理只有 64 个，Sponza 这类场景纹理句柄达四百多，越界后落到后面的缓冲区间，采样到错误资源。
#if defined(HE_SHADER_D3D12)
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

[[vk::binding(kGPUBinding_BindlessTextures, kGPUDescSet_PerFrame)]] Texture2D<float4> u_Textures[HE_BINDLESS_DIM] : register(kGPUBinding_BindlessTextures_hlsl, kGPUDescSet_Bindless_hlsl);
[[vk::binding(kGPUBinding_BindlessSamplers, kGPUDescSet_PerFrame)]] SamplerState      u_Samplers[HE_BINDLESS_DIM] : register(kGPUBinding_BindlessSamplers_hlsl, kGPUDescSet_Bindless_hlsl);

// Bindless 材质 SSBO 数组（StructuredBuffer<GPUMaterialData>，binding 30，per-material 数据）
// 索引 = materialID >> 2（每材质占 4 个纹理槽）
[[vk::binding(kGPUBinding_BindlessSSBO, kGPUDescSet_PerFrame)]] StructuredBuffer<GPUMaterialData> u_Materials[HE_BINDLESS_SSBO_DIM] : register(kGPUBinding_BindlessSSBO_hlsl, kGPUDescSet_Bindless_hlsl);

#endif // HUGENGINE_COMMON_SLANG
