// ============================================================
// Tests/TestPTVolumeMath.cpp — 电介质界面 / 参与介质数学的 CPU 单元测试（PT 任务 4）
//
// 覆盖范围（**纯 CPU、无 RHI**，全部与解析解对照）：
//   1. 均匀介质：指数分布自由程采样的生存概率 == Beer-Lambert 解析解 exp(-σ·D)
//      （判据「均匀介质的衰减与解析解对照」；同时校验中位自由程 = ln2/σ）
//   2. Snell 折射：垂直入射不偏折；斜入射满足 sinθt = η·sinθi
//   3. 全内反射：超过临界角 asin(1/η) 时返回 false
//   4. Fresnel：掠射趋近 1、垂直入射等于 f0
//   5. glTF 体积参数 → σ_t 的推导（不吸收时为 0）
//
// 为什么能脱离 RHI：RT/PTVolumeMath.h 只依赖 Core/Types.h 与 Math/Math.h。
// ============================================================

#include "doctest.h"

#include "RT/PTVolumeMath.h"

#include <cmath>

using namespace he;
using namespace he::render;

TEST_CASE("参与介质：指数分布采样与 Beer-Lambert 解析解一致") {
    const float sigma = 0.7f;          // 均匀介质吸收系数
    const int   N     = 200000;
    const float D     = 1.5f;          // 探测距离

    int beyond = 0;
    double sum = 0.0;
    for (int i = 0; i < N; ++i) {
        // 分层采样 u，避免用随机数（结果可复现）
        const float u = (i + 0.5f) / N;
        const float d = SampleDistanceInMediumCPU(u, sigma);
        if (d > D) ++beyond;
        sum += d;
    }

    // 生存概率 P(d > D) = exp(-σ·D)（MC 抽样比例，容差 1%）
    const float expected = std::exp(-sigma * D);
    const float measured = static_cast<float>(beyond) / N;
    CHECK(measured == doctest::Approx(expected).epsilon(0.01f));

    // 均值 = 1/σ（指数分布）
    CHECK(static_cast<float>(sum / N) == doctest::Approx(1.0f / sigma).epsilon(0.01f));

    // 中位自由程 = ln2/σ
    CHECK(SampleDistanceInMediumCPU(0.5f, sigma) == doctest::Approx(std::log(2.0f) / sigma));

    // Beer 解析解本身
    const float3 T = BeerTransmittanceCPU(float3(sigma), D);
    CHECK(T.x == doctest::Approx(expected));
}

TEST_CASE("Snell 折射：垂直入射不偏折，斜入射满足折射定律") {
    const float3 N(0.0f, 0.0f, 1.0f);
    const float  ior = 1.5f;
    const float  eta = 1.0f / ior;     // 空气 → 介质

    SUBCASE("垂直入射") {
        const float3 I(0.0f, 0.0f, -1.0f);
        float3 T;
        REQUIRE(RefractDirectionCPU(I, N, eta, T));
        CHECK(std::abs(T.x) < 1e-6f);
        CHECK(std::abs(T.y) < 1e-6f);
        CHECK(T.z == doctest::Approx(-1.0f).epsilon(1e-5f));
    }

    SUBCASE("斜入射：sinθt = η · sinθi") {
        const float thetaI = 0.6f;                       // 入射角（弧度）
        const float3 I(std::sin(thetaI), 0.0f, -std::cos(thetaI));
        float3 T;
        REQUIRE(RefractDirectionCPU(I, N, eta, T));
        const float sinThetaT = std::sqrt(std::max(0.0f, 1.0f - T.z * T.z));
        CHECK(sinThetaT == doctest::Approx(eta * std::sin(thetaI)).epsilon(1e-4f));
        // 折射方向应在介质侧（z < 0）
        CHECK(T.z < 0.0f);
    }
}

TEST_CASE("全内反射：入射角超过临界角时返回 false") {
    const float3 N(0.0f, 0.0f, 1.0f);
    const float  ior = 1.5f;
    const float  eta = ior;            // 介质 → 空气
    const float  critical = std::asin(1.0f / ior);

    SUBCASE("略小于临界角：正常折射") {
        const float thetaI = critical * 0.95f;
        const float3 I(std::sin(thetaI), 0.0f, -std::cos(thetaI));
        float3 T;
        CHECK(RefractDirectionCPU(I, N, eta, T));
    }
    SUBCASE("略大于临界角：全内反射") {
        const float thetaI = critical * 1.05f;
        const float3 I(std::sin(thetaI), 0.0f, -std::cos(thetaI));
        float3 T;
        CHECK_FALSE(RefractDirectionCPU(I, N, eta, T));
    }
}

TEST_CASE("菲涅尔：掠射趋近 1，垂直入射等于 f0") {
    const float f0 = 0.04f;
    CHECK(FresnelDielectricCPU(1.0f, f0) == doctest::Approx(f0));
    CHECK(FresnelDielectricCPU(0.0f, f0) == doctest::Approx(1.0f));
    CHECK(FresnelDielectricCPU(0.5f, f0) > f0);
    CHECK(FresnelDielectricCPU(0.5f, f0) < 1.0f);
}

TEST_CASE("glTF 体积参数 → 吸收系数 σ_t") {
    // 不衰减（attenuationDistance = 0 对应 glTF 的 +inf）
    const float3 zero = ComputeSigmaT(float3(0.5f), 0.0f);
    CHECK(zero.x == 0.0f);
    CHECK(zero.y == 0.0f);
    CHECK(zero.z == 0.0f);

    // attenuationColor=(1,1,1) 不吸收 → σ_t = 0
    const float3 none = ComputeSigmaT(float3(1.0f), 2.0f);
    CHECK(none.x == doctest::Approx(0.0f));

    // 已知例：color=0.5、distance=2 → σ_t = -ln(0.5)/2
    const float3 st = ComputeSigmaT(float3(0.5f), 2.0f);
    CHECK(st.x == doctest::Approx(-std::log(0.5f) / 2.0f));

    // 该 σ_t 下距离 2 的透射率 == 0.5（与 Beer 解析解自洽）
    CHECK(BeerTransmittanceCPU(st, 2.0f).x == doctest::Approx(0.5f).epsilon(1e-5f));
}
