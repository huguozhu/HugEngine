// ============================================================
// common.slang — HugEngine 共享着色器常量和工具函数
// ============================================================
#ifndef HUGENGINE_COMMON_SLANG
#define HUGENGINE_COMMON_SLANG

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

// Bindless 纹理数组（全局共享，所有 mesh 共用 set=0 binding 5-6）
[[vk::binding(5, 0)]] Texture2D<float4> u_Textures[];
[[vk::binding(6, 0)]] SamplerState      u_Samplers[];

#endif // HUGENGINE_COMMON_SLANG
