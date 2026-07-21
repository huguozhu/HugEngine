// ============================================================
// common.slang — HugEngine 共享着色器常量和工具函数
//
// 注意：Descriptor Set 和 Binding 索引常量需与 C++ RHI/Types.h
// 中的定义保持严格一致，以免 CPU/GPU 绑定错位。
// ============================================================
#ifndef HUGENGINE_COMMON_SLANG
#define HUGENGINE_COMMON_SLANG

#include "ShaderTypes.slang"  // GPU struct 共享定义（GPUObjectData 等）

// --- Descriptor Set 索引（与 C++ kDescSet* 一致）---
static const uint kSet_PerFrame = 0;  // set=0: 逐帧数据（Camera/Lights/Shadows）
static const uint kSet_Material = 1;  // set=1: 逐材质数据（Bindless Textures）
static const uint kSet_Bindless = 2;  // set=2: 无绑定资源（TLAS 等）

// --- Binding 索引（与 C++ kBinding* 一致，set=0 内）---
static const uint kB_ObjectData       = 2;   // Object SSBO
static const uint kB_BindlessTextures = 5;   // Bindless 纹理数组（SampledImage）
static const uint kB_BindlessSamplers = 6;   // Bindless 采样器数组（Sampler）
static const uint kB_LightGrid        = 7;   // 光源网格（Clustered Shading SSBO）
static const uint kB_LightIndexList   = 8;   // 光源索引列表（Clustered Shading SSBO）

// --- Forward 管线 Binding 索引（set=0）---
static const uint kB_Lights          = 1;   // GPULight[] SSBO
static const uint kB_ShadowData      = 3;   // GPUShadowData[] SSBO
static const uint kB_ShadowMap0      = 4;   // CSM cascade 0
static const uint kB_PointShadow     = 9;   // 点光源阴影 Cubemap
static const uint kB_ShadowMap1      = 10;  // CSM cascade 1
static const uint kB_ShadowMap2      = 11;  // CSM cascade 2
static const uint kB_IrradianceMap   = 12;  // IBL 辐照度图
static const uint kB_PrefilterMap    = 13;  // IBL 预滤波环境图
static const uint kB_BRDF_LUT        = 14;  // BRDF 积分 LUT
static const uint kB_RSMPosition     = 15;  // RSM 位置图
static const uint kB_RSMFlux         = 16;  // RSM 通量图

// --- Deferred 管线 Binding 索引（set=0）---
static const uint kB_GBufferA      = 0;   // GBuffer albedo+metallic
static const uint kB_GBufferB      = 1;   // GBuffer normal+roughness
static const uint kB_GBufferC      = 2;   // GBuffer emissive+ao
static const uint kB_GBufferDepth  = 3;   // 深度缓冲
static const uint kB_GBufferE      = 23;  // GBuffer worldPos（MRT4）
static const uint kB_SpotShadow    = 9;   // 聚光灯阴影贴图（绑定槽 9，Forward 为 PointShadow）
static const uint kB_Lights_DL     = 17;  // GPULight[] SSBO
static const uint kB_ShadowData_DL = 18;  // GPUShadowData[] SSBO
static const uint kB_SSGI          = 19;  // SSGI 间接光照
static const uint kB_SSAO          = 20;  // SSAO 环境光遮蔽
static const uint kB_SSR           = 21;  // SSR 屏幕空间反射
static const uint kB_DDGIProbes    = 22;  // DDGI 探针数据

// --- 通用常量 ---
static const float HE_PI      = 3.14159265359;
static const float HE_2PI     = 6.28318530718;
static const float HE_HALF_PI = 1.57079632679;
static const float HE_EPSILON = 0.0001;

// 线性空间 → sRGB
float3 LinearToSRGB(float3 color) {
    return pow(color, 1.0 / 2.2);
}

// sRGB → 线性空间
float3 SRGBToLinear(float3 color) {
    return pow(color, 2.2);
}

// 亮度计算（感知加权）
float Luminance(float3 color) {
    return dot(color, float3(0.2126, 0.7152, 0.0722));
}

// Bindless 纹理数组（全局共享，所有 mesh 共用 set=0）
[[vk::binding(kB_BindlessTextures, kSet_PerFrame)]] Texture2D<float4> u_Textures[];
[[vk::binding(kB_BindlessSamplers, kSet_PerFrame)]] SamplerState      u_Samplers[];

#endif // HUGENGINE_COMMON_SLANG
