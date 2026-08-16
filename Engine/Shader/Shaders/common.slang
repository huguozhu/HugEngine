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

// 线性空间 → sRGB（标准分段传递函数，误差 < 0.5%）
// 阈值 0.0031308 为线性段与幂函数段的分界
float3 LinearToSRGB(float3 c) {
    float3 lo = c * 12.92;
    float3 hi = 1.055 * pow(max(c, 0.0), float3(1.0 / 2.4)) - 0.055;
    return c <= 0.0031308 ? lo : hi;
}

// sRGB → 线性空间（标准分段传递函数）
float3 SRGBToLinear(float3 c) {
    float3 lo = c / 12.92;
    float3 hi = pow((max(c, 0.0) + 0.055) / 1.055, float3(2.4));
    return c <= 0.04045 ? lo : hi;
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

// ST.2084 (PQ) 编码：线性 HDR 亮度 → PQ 非线性（HDR10 传输函数）
// linNits 为绝对亮度（尼特），峰值 10000
float3 LinearToPQ(float3 linNits) {
    const float m1 = 0.1593017578125;  // 2610/16384
    const float m2 = 78.84375;          // 2523/32
    const float c1 = 0.8359375;         // 3424/4096
    const float c2 = 18.8515625;        // 2413/128
    const float c3 = 18.6875;           // 2392/128
    float3 y = max(linNits, 0.0) / 10000.0;
    float3 yPow = pow(y, float3(m1));
    return pow((c1 + c2 * yPow) / (1.0 + c3 * yPow), float3(m2));
}

// BT.709 → BT.2020 色域映射（HDR10 广色域）
float3 BT709ToBT2020(float3 c) {
    float3x3 m = float3x3(
        0.6274, 0.3293, 0.0433,
        0.0691, 0.9195, 0.0114,
        0.0164, 0.0880, 0.8956);
    return mul(m, c);
}

// Bindless 纹理数组（全局共享，所有 mesh 共用 set=0）
[[vk::binding(kGPUBinding_BindlessTextures, kGPUDescSet_PerFrame)]] Texture2D<float4> u_Textures[];
[[vk::binding(kGPUBinding_BindlessSamplers, kGPUDescSet_PerFrame)]] SamplerState      u_Samplers[];

// Bindless SSBO 数组（StructuredBuffer<GPUObjectData>，binding 30，供 GPU Driven / RT / 材质系统共用）
[[vk::binding(kGPUBinding_BindlessSSBO, kGPUDescSet_PerFrame)]] StructuredBuffer<GPUObjectData> u_SSBO[];

#endif // HUGENGINE_COMMON_SLANG
