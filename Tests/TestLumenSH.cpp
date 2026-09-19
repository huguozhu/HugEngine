// ============================================================
// Tests/TestLumenSH.cpp — 步骤 23 的 SH 投影数学单测
//
// 步骤 23 的验收口径是"SH 重建的辐照度与逐光线求和的误差在**白炉**下为 0（数值可断言）"。
// 这条在解析上是真的：均匀半球采样（pdf = 1/(2π)）下 0 阶系数
//     l0 = (2π/N)·Σ Y00 = 2π·Y00 = √π
// **与采样方向、采样数无关**；再用 clamped cosine 卷积系数（Â0 = π、Â1 = 2π/3）重建，
// 白炉下 E(n) ≡ π。这里把这几步逐条钉住，顺便验证 C++ 镜像与 shader 用的是同一套常数。
// ============================================================

#include "Lumen/LumenSH.h"

#include <doctest/doctest.h>

#include <cmath>

using namespace he::render;

namespace {

constexpr double kPi = 3.14159265358979323846;

} // namespace

TEST_CASE("LumenSH: 基函数常数与经典约定一致") {
    CHECK(kSHY00 == doctest::Approx(0.5 * std::sqrt(1.0 / kPi)).epsilon(1e-12));
    CHECK(kSHY1  == doctest::Approx(0.5 * std::sqrt(3.0 / kPi)).epsilon(1e-12));
    // 白炉 0 阶系数的解析值就是 √π
    CHECK(kSHWhiteFurnaceL0 == doctest::Approx(std::sqrt(kPi)).epsilon(1e-12));
    CHECK(2.0 * kPi * kSHY00 == doctest::Approx(kSHWhiteFurnaceL0).epsilon(1e-12));
}

TEST_CASE("LumenSH: 白炉下 l0 与采样方向、采样数无关") {
    // 任取若干"探针法线"，各用 1 / 8 / 64 条均匀半球方向做投影：l0 恒为 √π
    const double normals[3][3] = { {0.0, 0.0, 1.0}, {1.0, 0.0, 0.0}, {0.5773502691896258, 0.5773502691896258, 0.5773502691896258} };
    const uint32_t rayCounts[3] = { 1u, 8u, 64u };
    for (const auto& n : normals) {
        for (uint32_t N : rayCounts) {
            double sum = 0.0;
            for (uint32_t r = 0; r < N; ++r) {
                const auto rnd = LumenProbeRayRandom(r, /*frameSeed*/ 7u);
                double dx, dy, dz;
                LumenSampleUniformHemisphere(n[0], n[1], n[2], rnd, dx, dy, dz);
                sum += SHBasis(0u, dx, dy, dz);          // L ≡ 1 的白炉
            }
            const double l0 = (2.0 * kPi / (double)N) * sum;
            CHECK(l0 == doctest::Approx(kSHWhiteFurnaceL0).epsilon(1e-12));
        }
    }
}

TEST_CASE("LumenSH: 均匀半球采样确实落在半球内、长度为 1 且 pdf 均匀（E[cos]=1/2）") {
    // 逐条 CHECK 会把断言数撑到两万（噪音），这里改成"先聚合再断言一次"
    constexpr uint32_t kN = 20000u;
    double meanCos = 0.0, minDot = 1.0, maxLenErr = 0.0;
    for (uint32_t r = 0; r < kN; ++r) {
        const auto rnd = LumenProbeRayRandom(r, 3u);
        double dx, dy, dz;
        LumenSampleUniformHemisphere(0.0, 1.0, 0.0, rnd, dx, dy, dz);
        const double len = std::sqrt(dx * dx + dy * dy + dz * dz);
        maxLenErr = std::fmax(maxLenErr, std::fabs(len - 1.0));
        minDot = std::fmin(minDot, dy);
        meanCos += dy;
    }
    CHECK(maxLenErr < 1e-9);                                              // 单位向量
    CHECK(minDot >= 0.0);                                                 // 全部在半球内
    CHECK(meanCos / (double)kN == doctest::Approx(0.5).epsilon(0.02));    // 面积均匀 ⇒ E[cosθ] = 1/2
}

TEST_CASE("LumenSH: 全球白炉的解析系数重建出各向同性的 π（严格为 0 误差）") {
    // 白炉 = **整球**辐射度恒为 1（这正是引擎 HE_FURNACE=1 的语义）。此时
    //   l0 = 4π·Y00 = 2√π，l1..3 = ∫_sphere ω_m dω = 0
    // 于是只能带内重建，且 E(n) = π·l0·Y00 = π 与 n 无关 —— 这是"误差为 0"的最强形式。
    const double normals[4][3] = {
        {0.0, 0.0, 1.0}, {0.0, 1.0, 0.0}, {1.0, 0.0, 0.0}, {0.2672612419124244, 0.5345224838248488, 0.8017837257372732} };
    const double l0 = 4.0 * kPi * kSHY00;
    CHECK(l0 == doctest::Approx(2.0 * std::sqrt(kPi)).epsilon(1e-12));
    for (const auto& n : normals) {
        CHECK(SHRadianceToIrradiance(l0, 0.0, 0.0, 0.0, n[0], n[1], n[2])
              == doctest::Approx(kPi).epsilon(1e-12));
    }
}

TEST_CASE("LumenSH: 单侧（半球）白炉的解析重建为 π/2·(1+n̂·n)") {
    // 探针的光线只覆盖**法线半球**（单侧白炉）：l0 = √π、l_m = Y1·π·n̂_m。代入重建公式得
    //   E(n) = π/2 + (π/2)·(n̂·n)
    // 即"单侧白炉并不自动给出各向同性的 π"，只在 n = n̂ 时等于 π。这条把"为什么探针的白炉
    // 重建不能直接断言 ≡ π"钉住，免得后面把带限误差误判成实现错误。
    const double nHat[3] = {0.0, 0.0, 1.0};
    const double l0 = kSHWhiteFurnaceL0;
    const double l1 = kSHY1 * kPi * nHat[1];
    const double l2 = kSHY1 * kPi * nHat[2];
    const double l3 = kSHY1 * kPi * nHat[0];
    const double probes[3][3] = { {0.0, 0.0, 1.0}, {0.0, 1.0, 0.0}, {0.0, 0.0, 1.0} };
    for (const auto& n : probes) {
        const double cosAngle = nHat[0] * n[0] + nHat[1] * n[1] + nHat[2] * n[2];
        CHECK(SHRadianceToIrradiance(l0, l1, l2, l3, n[0], n[1], n[2])
              == doctest::Approx(0.5 * kPi * (1.0 + cosAngle)).epsilon(1e-12));
    }
    // n = n̂ 时正是 π
    CHECK(SHRadianceToIrradiance(l0, l1, l2, l3, 0.0, 0.0, 1.0) == doctest::Approx(kPi).epsilon(1e-12));
}

TEST_CASE("LumenSH: 单侧白炉下离散投影重建的误差随光线数下降（多探针平均）") {
    // 白炉下"逐光线 cos 加权求和"与"SH 重建"都是 π 的无偏估计，两者之差是**有限采样**误差。
    // 单条探针的差是随机量（8 条时可能恰好很小），所以必须按多探针取平均 —— 这也正是运行期
    // 日志里"SH 重建 vs 逐光线求和 平均相对差"的算法。
    auto meanRelDiff = [](uint32_t N) {
        const double nx = 0.0, ny = 0.0, nz = 1.0;
        double sumRel = 0.0;
        constexpr uint32_t kProbes = 2000u;
        for (uint32_t pi = 0u; pi < kProbes; ++pi) {
            double l0 = 0.0, l1 = 0.0, l2 = 0.0, l3 = 0.0, ref = 0.0;
            for (uint32_t r = 0; r < N; ++r) {
                // 用 (探针, 光线, 帧) 铺开随机流：每条探针拿到独立的一组方向
                const auto rnd = LumenProbeRayRandom(pi * N + r, 11u);
                double dx, dy, dz;
                LumenSampleUniformHemisphere(nx, ny, nz, rnd, dx, dy, dz);
                l0 += SHBasis(0u, dx, dy, dz);
                l1 += SHBasis(1u, dx, dy, dz);
                l2 += SHBasis(2u, dx, dy, dz);
                l3 += SHBasis(3u, dx, dy, dz);
                ref += std::fmax(0.0, dz);
            }
            const double k = 2.0 * kPi / (double)N;
            l0 *= k; l1 *= k; l2 *= k; l3 *= k; ref *= k;
            if (ref < 1e-9) continue;
            const double sh = SHRadianceToIrradiance(l0, l1, l2, l3, nx, ny, nz);
            sumRel += std::fabs(sh - ref) / ref;
        }
        return sumRel / (double)kProbes;
    };
    const double d8   = meanRelDiff(8u);
    const double d128 = meanRelDiff(128u);
    const double d512 = meanRelDiff(512u);
    CHECK(d128 < d8 * 0.5);        // 采样数 16 倍 ⇒ 平均误差至少减半
    CHECK(d512 < d128);            // 继续下降
    CHECK(d512 < 0.05);            // 512 条时已接近解析值
}

TEST_CASE("LumenSH: C++ 随机数与方向对 shader 约定可复现") {
    // 同一 (光线, 帧) 必须恒得同一对随机数（背靠背读数一致的前提）
    const auto a = LumenProbeRayRandom(123u, 45u);
    const auto b = LumenProbeRayRandom(123u, 45u);
    CHECK(a.u == b.u);
    CHECK(a.v == b.v);
    // 不同光线/不同帧必须不同（否则整屏方向重合）
    const auto c = LumenProbeRayRandom(124u, 45u);
    const auto d = LumenProbeRayRandom(123u, 46u);
    CHECK((c.u != a.u || c.v != a.v));
    CHECK((d.u != a.u || d.v != a.v));
    // U01 落在 [0,1)
    double minX = 1e9, maxX = -1e9;
    for (uint32_t s = 0; s < 256; ++s) {
        const double x = LumenProbeU01(s * 2654435761u + 1u);
        minX = std::fmin(minX, x);
        maxX = std::fmax(maxX, x);
    }
    CHECK(minX >= 0.0);
    CHECK(maxX < 1.0);
}
