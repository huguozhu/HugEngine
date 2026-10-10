// ============================================================
// MathCommon.slang — 纯数学/颜色工具（**零绑定、零 include**）
//
// 【为什么要与 common.slang 分开】这些函数是纯计算（PI 常数、sRGB 传递函数、感知亮度、
// ACES、PQ、BT.709→2020），任何 pass 都可能用到；而 `common.slang` 除了这些还声明了
// set-0 的 bindless 数组（`u_Textures[]` / `u_Samplers[]` / `u_Materials[]`）。
// 后处理等 pass 没有那套描述符布局，一旦为了用 `Luminance` 而 include `common.slang`，
// 就会把三个用不到的绑定带进它的反射里。因此把纯计算拆出来，谁都能安全引用。
//
// 【统一了什么】此前 `Luminance` 在 4 个文件里各写一份（PT_Atrous / AutoExposure /
// BrightPass / ColorGrading），sRGB 两个函数在 ColorGrading 里又抄了一份并注明
// "与 common.slang 保持一致" —— 现在只有这里一处。
//
// `common.slang` 会 include 本文件，故原有 include `common.slang` 的使用方不受影响。
// ============================================================
#ifndef HUGENGINE_MATH_COMMON_SLANG
#define HUGENGINE_MATH_COMMON_SLANG

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

#endif // HUGENGINE_MATH_COMMON_SLANG
