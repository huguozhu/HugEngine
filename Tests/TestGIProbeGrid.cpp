// ============================================================
// Tests/TestGIProbeGrid.cpp — DDGI 探针网格拟合的纯几何单元测试（任务 14 / §9.2-K）
//
// 覆盖范围（**纯 CPU、无 RHI**）：
//   1. 「刚好罩住包围盒」这条不变量：逐轴覆盖 + 紧凑（最后一根探针不超过盒面一个格距）
//   2. 三轴共用同一格距，且最长边上的探针数等于请求值
//   3. 短轴按自己的边长取探针数（不随最长边统一取值，否则白铺探针）
//   4. 退化包围盒（零尺寸 / 反向 / NaN）返回 nullopt —— 调用方保持原参数
//   5. Sponza 场景（本仓库实测包围盒）的确定参数指纹
//
// 为什么能脱离 RHI：拟合规则在 `GI/GIProbeGrid.h`（只依赖 Core/Types.h 与 Math/Math.h），
// 故本文件只需把 `Engine/Render` 加入 include 路径，**无需链接 HugEngineRender**。
// ============================================================

#include "doctest.h"

#include "GI/GIProbeGrid.h"   // 探针网格拟合（RHI-free）

#include <limits>

using namespace he;
using namespace he::render;

namespace {

/// 「刚好罩住」= 覆盖到盒面 + 最后一根探针与盒面的距离不超过一个格距。
/// 后者保证紧凑：不做的话，把格距凭空放大一倍也能"覆盖"。
void CheckTightCover(const GIProbeGridFit& fit, const float3& mn, const float3& mx) {
    CHECK(fit.Covers(mn, mx));
    const float3 size = mx - mn;
    const u32    n[3] = { fit.countX, fit.countY, fit.countZ };
    const float  s[3] = { size.x, size.y, size.z };
    for (int a = 0; a < 3; ++a) {
        const float span = float(n[a] - 1u) * fit.cellSize;   // 该轴实际覆盖长度
        CHECK(span >= s[a]);                                  // 罩得住
        CHECK(span - s[a] < fit.cellSize);                    // 但不白铺一整格
    }
}

} // namespace

TEST_CASE("FitProbeGridToBounds：刚好罩住包围盒且不白铺探针") {
    const float3 boxes[][2] = {
        { float3(0.0f),          float3(10.0f, 10.0f, 10.0f) },     // 立方体
        { float3(-5.0f, 0.0f, 2.0f), float3(95.0f, 7.0f, 12.0f) },  // 极扁
        { float3(0.0f),          float3(3720.9f, 1555.9f, 2288.2f) }, // Sponza（实测）
        { float3(-1234.5f, -1.0f, 900.0f), float3(1.0f, 1000.0f, 901.0f) },
    };
    for (const auto& b : boxes) {
        for (u32 cells : { 2u, 4u, 8u, 16u, 32u }) {
            const auto fit = FitProbeGridToBounds(b[0], b[1], cells);
            REQUIRE(fit.has_value());
            CheckTightCover(*fit, b[0], b[1]);
            // 最长边上的探针数恰好等于请求值（格距就是按它定义的）
            const float3 size = b[1] - b[0];
            CHECK(fit->cellSize == doctest::Approx(std::max(size.x, std::max(size.y, size.z))
                                                  / float(cells - 1u)));
            CHECK(std::max(fit->countX, std::max(fit->countY, fit->countZ)) == cells);
            CHECK(fit->ProbeCount() >= 8u);
        }
    }
}

TEST_CASE("FitProbeGridToBounds：短轴按自己的边长取探针数，不随最长边统一") {
    // Sponza：X 最长 3720.9，Z 只有 2288.2 ⇒ 若三轴统一取 16，Z 轴会白铺 5 根探针
    const auto fit = FitProbeGridToBounds(float3(0.0f), float3(3720.9f, 1555.9f, 2288.2f), 16u);
    REQUIRE(fit.has_value());
    CHECK(fit->cellSize == doctest::Approx(248.06f).epsilon(0.001));
    CHECK(fit->countX == 16u);
    CHECK(fit->countY == 8u);
    CHECK(fit->countZ == 11u);
    CHECK(fit->ProbeCount() == 16u * 8u * 11u);   // 1408，而非三轴统一 16 时的 4096
    // 原点是包围盒最小角 ⇒ 直接就是世界空间锚点，不需要再算中心对齐
    CHECK(fit->origin.x == doctest::Approx(0.0f));
    CHECK(fit->origin.y == doctest::Approx(0.0f));
    CHECK(fit->origin.z == doctest::Approx(0.0f));
}

TEST_CASE("FitProbeGridToBounds：立方体三轴探针数相同（格距各向同性）") {
    const auto fit = FitProbeGridToBounds(float3(-50.0f, -50.0f, -50.0f),
                                         float3(50.0f, 50.0f, 50.0f), 8u);
    REQUIRE(fit.has_value());
    CHECK(fit->countX == 8u);
    CHECK(fit->countY == 8u);
    CHECK(fit->countZ == 8u);
    CHECK(fit->cellSize == doctest::Approx(100.0f / 7.0f));
    CHECK(fit->origin.x == doctest::Approx(-50.0f));
}

TEST_CASE("FitProbeGridToBounds：退化包围盒返回 nullopt（调用方保持原参数）") {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    CHECK_FALSE(FitProbeGridToBounds(float3(0.0f), float3(0.0f, 1.0f, 1.0f), 16u).has_value());   // X 零尺寸
    CHECK_FALSE(FitProbeGridToBounds(float3(0.0f), float3(1.0f, 0.0f, 1.0f), 16u).has_value());   // Y 零尺寸
    CHECK_FALSE(FitProbeGridToBounds(float3(0.0f), float3(1.0f, 1.0f, 0.0f), 16u).has_value());   // Z 零尺寸
    CHECK_FALSE(FitProbeGridToBounds(float3(10.0f), float3(1.0f, 10.0f, 10.0f), 16u).has_value()); // 反向
    CHECK_FALSE(FitProbeGridToBounds(float3(0.0f), float3(nan, 1.0f, 1.0f), 16u).has_value());    // NaN
    // 默认构造的 AABB（min=+FLT_MAX, max=-FLT_MAX）也必须被拒绝：它就是"还没有几何"
    const float big = std::numeric_limits<float>::max();
    CHECK_FALSE(FitProbeGridToBounds(float3(big), float3(-big), 16u).has_value());
}

TEST_CASE("FitProbeGridToBounds：cells 下限为 2（否则格距无意义）") {
    const auto fit = FitProbeGridToBounds(float3(0.0f), float3(10.0f, 10.0f, 10.0f), 0u);
    REQUIRE(fit.has_value());
    CHECK(fit->cellSize == doctest::Approx(10.0f));   // maxExtent / (2-1)
    CHECK(fit->countX == 2u);
    CHECK(fit->countY == 2u);
    CHECK(fit->countZ == 2u);
}
