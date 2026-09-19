// LumenSH.h — 二阶（4 系数）球谐的 C++ 侧镜像（步骤 23）
//
// 【为什么单独一个头】`ScreenProbeSampling.slang` 里也定义了同一组常数与重建公式。
// 着色器侧不方便做单元测试，而"白炉下 SH 的 0 阶项必须等于 √π"这条验收恰恰是**解析**的 ——
// 放一个 C++ 镜像，就能用断言把它钉死，并且任何一侧改动都会被单测或运行期读数抓住。
//
// 【约定】与 Ramamoorthi & Hanrahan 的经典约定一致：
//   Y00 = 0.5·√(1/π) ≈ 0.2820948
//   Y1m = 0.5·√(3/π) · (y, z, x)  （m = -1, 0, 1 的顺序取 (y, z, x)）
//   辐照度重建（clamped cosine 卷积）：E(n) = π·l0·Y00 + (2π/3)·Σ_{m=1..3} l_m·Y_m(n)
// 白炉（半球内辐射度恒为 1）下 E(n) ≡ π，且 l0 ≡ 2π·Y00 = √π —— 与采样方向、采样数无关。
#pragma once

#include <cmath>
#include <cstdint>

namespace he::render {

/// 0.5·√(1/π)
inline constexpr double kSHY00 = 0.28209479177387814;
/// 0.5·√(3/π)
inline constexpr double kSHY1 = 0.48860251190291992;
/// 白炉下 0 阶系数的解析值：2π·Y00 = √π
inline constexpr double kSHWhiteFurnaceL0 = 1.7724538509055159;   // √π

/// 第 i 个基函数（i ∈ [0,4)）在方向 d（单位向量）上的值
inline double SHBasis(uint32_t i, double dx, double dy, double dz) {
    switch (i) {
        case 0: return kSHY00;
        case 1: return kSHY1 * dy;
        case 2: return kSHY1 * dz;
        default: return kSHY1 * dx;
    }
}

/// 由**辐射度**的 4 系数重建**辐照度**（单通道）：Â0 = π、Â1 = 2π/3
inline double SHRadianceToIrradiance(double l0, double l1, double l2, double l3,
                                     double nx, double ny, double nz) {
    constexpr double kA0 = 3.14159265358979323846;
    constexpr double kA1 = 2.09439510239319549231;   // 2π/3
    return l0 * (kA0 * kSHY00)
         + (l1 * (kSHY1 * ny) + l2 * (kSHY1 * nz) + l3 * (kSHY1 * nx)) * kA1;
}

/// 与 `ScreenProbeSampling.slang` 的 `Pcg` 逐位一致（保证 CPU 参考与 GPU 用同一组方向）
inline uint32_t LumenProbePcg(uint32_t v) {
    v = v * 747796405u + 2891336453u;
    const uint32_t w = ((v >> ((v >> 28u) + 4u)) ^ v) * 277803737u;
    return (w >> 22u) ^ w;
}

/// 与 shader 的 `U01` 一致：[0,1) 均匀
inline double LumenProbeU01(uint32_t seed) {
    return (double)(LumenProbePcg(seed) & 0x00FFFFFFu) / 16777216.0;
}

/// 与 shader 的 `ProbeRayRandom` 一致：同一 (光线扁平下标, 帧种子) 取到同一对随机数
struct LumenProbeRandom { double u, v; };
inline LumenProbeRandom LumenProbeRayRandom(uint32_t rayFlatIndex, uint32_t frameSeed) {
    return { LumenProbeU01(rayFlatIndex * 3u + frameSeed * 7919u + 1u),
             LumenProbeU01(rayFlatIndex * 7u + frameSeed * 6271u + 5u) };
}

/// 与 shader 的 `SampleUniformHemisphere` 一致：以 n 为 Z 轴的均匀半球采样（pdf = 1/(2π)）
inline void LumenSampleUniformHemisphere(double nx, double ny, double nz, LumenProbeRandom r,
                                         double& ox, double& oy, double& oz) {
    constexpr double kTwoPi = 6.28318530717958647692;
    // 构造以 n 为 Z 轴的正交基（与 shader 的 BuildBasis 同规则）：
    //   |n.z| < 0.999 → t = normalize(cross((0,0,1), n)) = normalize((-n.y, n.x, 0))
    //   否则          → t = normalize(cross((1,0,0), n)) = normalize((0, -n.z, n.y))
    double tx, ty, tz;
    if (std::fabs(nz) < 0.999) { tx = -ny; ty = nx; tz = 0.0; }
    else                       { tx = 0.0; ty = -nz; tz = ny; }
    const double tl = std::sqrt(tx * tx + ty * ty + tz * tz);
    tx /= tl; ty /= tl; tz /= tl;
    const double bx = ny * tz - nz * ty, by = nz * tx - nx * tz, bz = nx * ty - ny * tx;

    const double phi = kTwoPi * r.u;
    const double cosTheta = r.v < 0.0 ? 0.0 : (r.v > 1.0 ? 1.0 : r.v);
    const double sinTheta = std::sqrt(std::fmax(0.0, 1.0 - cosTheta * cosTheta));
    ox = tx * (sinTheta * std::cos(phi)) + bx * (sinTheta * std::sin(phi)) + nx * cosTheta;
    oy = ty * (sinTheta * std::cos(phi)) + by * (sinTheta * std::sin(phi)) + ny * cosTheta;
    oz = tz * (sinTheta * std::cos(phi)) + bz * (sinTheta * std::sin(phi)) + nz * cosTheta;
    const double l = std::sqrt(ox * ox + oy * oy + oz * oz);
    ox /= l; oy /= l; oz /= l;
}

} // namespace he::render
