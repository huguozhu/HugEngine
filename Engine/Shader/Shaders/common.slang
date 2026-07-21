// ============================================================
// common.slang — HugEngine 共享着色器常量和工具函数
//
// 常量定义统一在 ShaderTypes.slang（kGPU* 前缀），
// C++ 与 Slang 共享同一份数据源，修改常量只需改一处。
// ============================================================
#ifndef HUGENGINE_COMMON_SLANG
#define HUGENGINE_COMMON_SLANG

#include "ShaderTypes.slang"  // GPU struct + 常量（kGPU*）共享定义

// --- 着色器通用常量 ---
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

// ACES Filmic Tone Mapping（HDR → LDR）
// 系数来自 ACES 近似拟合，PBR / ToneMap 共用
float3 ACESFilm(float3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

// Bindless 纹理数组（全局共享，所有 mesh 共用 set=0）
[[vk::binding(kGPUBinding_BindlessTextures, kGPUDescSet_PerFrame)]] Texture2D<float4> u_Textures[];
[[vk::binding(kGPUBinding_BindlessSamplers, kGPUDescSet_PerFrame)]] SamplerState      u_Samplers[];

#endif // HUGENGINE_COMMON_SLANG
