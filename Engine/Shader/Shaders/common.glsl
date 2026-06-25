// ============================================================
// common.glsl — HugEngine 共享着色器常量和工具函数
// ============================================================
#ifndef HUGENGINE_COMMON_GLSL
#define HUGENGINE_COMMON_GLSL

#define HE_PI 3.14159265359
#define HE_2PI 6.28318530718
#define HE_HALF_PI 1.57079632679
#define HE_EPSILON 0.0001

// Tonemapping 模式
#define TONEMAP_NONE     0
#define TONEMAP_ACES     1
#define TONEMAP_FILMIC   2
#define TONEMAP_REINHARD 3

// Alpha 模式
#define ALPHA_MODE_OPAQUE  0
#define ALPHA_MODE_MASK    1
#define ALPHA_MODE_BLEND   2

// 线性空间 → sRGB
vec3 LinearToSRGB(vec3 color) {
    return pow(color, vec3(1.0 / 2.2));
}

// sRGB → 线性空间
vec3 SRGBToLinear(vec3 color) {
    return pow(color, vec3(2.2));
}

// 亮度计算（感知加权）
float Luminance(vec3 color) {
    return dot(color, vec3(0.2126, 0.7152, 0.0722));
}

#endif // HUGENGINE_COMMON_GLSL
