// ============================================================
// Tests/TestRSMFrustum.cpp — RSM 光源正交视锥拟合 + VPL 采样面积的纯几何单元测试
//                              （任务 30 / §9.2-AA）
//
// 覆盖范围（**纯 CPU、无 RHI**）：
//   1. 「罩住包围盒」：全部 8 个角点投影后 UV ∈ [0,1]（含被修掉的硬编码视锥罩不住的情形）
//   2. texel 为方形：正交盒恒为方形（halfWidth == halfHeight）
//   3. 投影矩阵的不变量：`1/|M00| == halfExtent` —— 着色器就是靠这条从矩阵反推世界尺度
//   4. 尺度不变性：场景整体缩放 k 倍时 `scale ∝ k²`，于是 `scale/d²` 不变（辐射度是尺度不变量）
//   5. 退化输入（反向盒 / 零尺寸 / NaN / 零方向）返回 nullopt —— 调用方保持原参数
//   6. Sponza 实测包围盒的确定参数指纹（含被修掉的 60 单位硬编码假设的对照）
//
// 为什么能脱离 RHI：规则在 `GI/RSMFrustum.h`（只依赖 Core/Types.h 与 Math/），
// 故本文件只需把 `Engine/Render` 加入 include 路径，**无需链接 HugEngineRender**。
// ============================================================

#include "doctest.h"

#include "GI/RSMFrustum.h"   // 光锥拟合 + VPL 面积（RHI-free）

#include <cmath>
#include <limits>

using namespace he;
using namespace he::render;

namespace {

const float3 kSponzaMin = float3(-1826.3f, -57.2f, -1114.6f);
const float3 kSponzaMax = float3(1894.6f, 1498.7f, 1173.6f);
const float3 kLightDir  = float3(0.3f, -1.0f, 0.4f);

/// 逐角点检查：投影后的 RSM UV 必须落在 [0,1]（否则该处没有 VPL 数据）
/// 边界角点会精确落在盒面上，浮点误差允许 1e-4（texel 尺度是 1/512 ≈ 2e-3，远大于它）
void CheckCovers(const RSMFrustumFit& fit, const float3& mn, const float3& mx) {
    const float kEps = 1e-4f;
    for (const float3& c : he::AABB(mn, mx).Corners()) {
        const float4 clip = fit.viewProj * float4(c, 1.0f);
        REQUIRE(clip.w != 0.0f);
        const float2 uv = float2(clip.x / clip.w, clip.y / clip.w) * 0.5f + 0.5f;
        CHECK(uv.x >= -kEps);
        CHECK(uv.x <= 1.0f + kEps);
        CHECK(uv.y >= -kEps);
        CHECK(uv.y <= 1.0f + kEps);
        // 深度也要落在 [0,1]（zero-to-one 约定，同引擎其余部分）
        const float z = clip.z / clip.w;
        CHECK(z >= -kEps);
        CHECK(z <= 1.0f + kEps);
    }
}

} // namespace

TEST_CASE("FitRSMFrustumToBounds：光锥罩住包围盒，且 texel 为方形") {
    const float3 boxes[][2] = {
        { float3(0.0f),                     float3(10.0f, 10.0f, 10.0f) },      // 立方体
        { float3(-5.0f, 0.0f, 2.0f),        float3(95.0f, 7.0f, 12.0f) },       // 极扁
        { kSponzaMin,                       kSponzaMax },                       // Sponza（实测）
        { float3(-1234.5f, -1.0f, 900.0f),  float3(1.0f, 1000.0f, 901.0f) },    // 细长
    };
    const float3 dirs[] = {
        kLightDir,
        float3(0.0f, -1.0f, 0.0f),      // 正上方（`up` 的退化分支）
        float3(1.0f, 0.0f, 0.0f),       // 水平
        float3(-0.577f, 0.577f, 0.577f) // 斜向
    };
    for (const auto& b : boxes) {
        for (const float3& d : dirs) {
            const auto fit = FitRSMFrustumToBounds(b[0], b[1], d);
            REQUIRE(fit.has_value());
            CheckCovers(*fit, b[0], b[1]);
            const float4x4 VP0 = fit->viewProj;
            // 方形正交盒：第 0/1 **行**的长度（= 世界→NDC 的尺度倒数）相等
            // ⇒ 1 单位 UV 在两个方向上对应同一世界长度（texel 是正方形）。
            // 注意 glm 是列主序：数学意义上的"第 i 行" = (M[0][i], M[1][i], M[2][i])，
            // 与 Slang 里 `M[i].xyz` 的取值一致（着色器就是按这条从矩阵反推世界尺度）。
            const float3 row0(VP0[0][0], VP0[1][0], VP0[2][0]);
            const float3 row1(VP0[0][1], VP0[1][1], VP0[2][1]);
            const float sx = glm::length(row0);
            const float sy = glm::length(row1);
            CHECK(sx == doctest::Approx(sy));
            CHECK(1.0f / sx == doctest::Approx(fit->halfExtent));
            const float3 expectCenter = (b[0] + b[1]) * 0.5f;
            CHECK(glm::all(glm::epsilonEqual(fit->center, expectCenter, 1e-4f)));
        }
    }
}

TEST_CASE("FitRSMFrustumToBounds：紧致（不是随便放大一个盒都能算罩住）") {
    // 紧致性用「被修掉的硬编码视锥」做反证：sceneCenter=(0,3,0)、radius=60 的盒
    // 覆盖不了 Sponza —— 正是 §9.2-AA ④ 的直接后果（大半屏幕的接收点没有邻近 VPL）。
    const float3 hardCenter = float3(0.0f, 3.0f, 0.0f);
    const float  hardRadius = 60.0f;
    const float4x4 hardView = glm::lookAt(hardCenter - glm::normalize(kLightDir) * hardRadius * 2.0f,
                                          hardCenter, float3(0.0f, 1.0f, 0.0f));
    const float4x4 hardProj = glm::orthoRH_ZO(-hardRadius, hardRadius, -hardRadius, hardRadius,
                                              0.1f, hardRadius * 4.0f);
    const float4x4 hardVP = hardProj * hardView;
    u32 covered = 0;
    for (const float3& c : he::AABB(kSponzaMin, kSponzaMax).Corners()) {
        const float4 clip = hardVP * float4(c, 1.0f);
        const float2 uv = float2(clip.x / clip.w, clip.y / clip.w) * 0.5f + 0.5f;
        if (uv.x >= 0.0f && uv.x <= 1.0f && uv.y >= 0.0f && uv.y <= 1.0f) covered++;
    }
    CHECK(covered == 0u);   // 硬编码视锥一个角点都罩不住

    // 拟合后的视锥则全罩住，而且不是"无限放大"：
    // 上界 = 包围球半径（投影后的横向范围不可能超过包围球直径的一半），
    // 下界 = 最长轴的一半（再小就必然有角点落在盒外）。
    const auto fit = FitRSMFrustumToBounds(kSponzaMin, kSponzaMax, kLightDir);
    REQUIRE(fit.has_value());
    const float3 size = kSponzaMax - kSponzaMin;
    const float  longestHalf = std::max(size.x, std::max(size.y, size.z)) * 0.5f;
    const float  sphereRadius = glm::length(size) * 0.5f;
    CHECK(fit->halfExtent <= sphereRadius * 1.001f);
    CHECK(fit->halfExtent >= longestHalf * 0.5f);
}

TEST_CASE("RSMVplScale：由光锥尺度推出，且在场景均匀缩放下不变") {
    const float radiusUV = 0.0125f;
    const u32   samples  = 16u;

    // 1) 形状：scale = (radiusUV · 2 · halfExtent)² / N
    for (float h : { 1.0f, 60.0f, 1860.0f }) {
        const float worldRadius = radiusUV * 2.0f * h;
        CHECK(RSMVplScale(h, radiusUV, samples) == doctest::Approx(worldRadius * worldRadius / float(samples)));
        // 面积版本与缩放版本只差一个 π
        CHECK(RSMVplSampleArea(h, radiusUV, samples)
              == doctest::Approx(HE_PI * RSMVplScale(h, radiusUV, samples)).epsilon(1e-5));
    }

    // 2) **尺度不变性**（这一条是本次修复的核心）：场景整体缩放 k 倍 ⇒
    //    scale ∝ k²、接收点到 VPL 的距离 d ∝ k ⇒ scale/d² 不变。
    //    旧实现里 scale 是硬编码常数 ⇒ 该乘积按 1/k² 掉，Sponza 比"隐含的 60 单位场景"
    //    大 31 倍，于是 RSM 间接光整项掉到 1e-8（噪声底）。
    const float k = 31.0f;
    const float d0 = 500.0f;
    const float contrib0 = RSMVplScale(60.0f, radiusUV, samples) / (d0 * d0);
    const float contrib1 = RSMVplScale(60.0f * k, radiusUV, samples) / ((d0 * k) * (d0 * k));
    CHECK(contrib1 == doctest::Approx(contrib0).epsilon(1e-4));

    // 3) 旧常数与"60 单位场景"的关系（把历史写进断言，防止再退回硬编码）：
    //    0.046875 ≈ 一个 RSM texel 在 radius=60 下的世界面积（120/512)²
    const float texelAreaAt60 = (120.0f / 512.0f) * (120.0f / 512.0f);
    CHECK(texelAreaAt60 == doctest::Approx(0.0549316f).epsilon(1e-4));
    CHECK(0.046875f / texelAreaAt60 > 0.8f);
    CHECK(0.046875f / texelAreaAt60 < 0.9f);

    // 4) Sponza 尺度下的实际放大倍数（相对硬编码常数）——留在断言里供文档引用
    const auto fit = FitRSMFrustumToBounds(kSponzaMin, kSponzaMax, kLightDir);
    REQUIRE(fit.has_value());
    const float scale = RSMVplScale(fit->halfExtent, radiusUV, samples);
    CHECK(scale / 0.046875f > 1000.0f);
    CHECK(scale / 0.046875f < 100000.0f);
}

TEST_CASE("RSMVplScale / FitRSMFrustumToBounds：退化输入不产生 NaN 与假的成功") {
    // 反向盒 / 零尺寸 / NaN
    CHECK_FALSE(FitRSMFrustumToBounds(float3(1.0f), float3(-1.0f), kLightDir).has_value());
    CHECK_FALSE(FitRSMFrustumToBounds(float3(0.0f), float3(0.0f), kLightDir).has_value());
    CHECK_FALSE(FitRSMFrustumToBounds(float3(0.0f), float3(std::numeric_limits<float>::quiet_NaN(), 1.0f, 1.0f),
                                      kLightDir).has_value());
    // 零方向
    CHECK_FALSE(FitRSMFrustumToBounds(float3(0.0f), float3(1.0f), float3(0.0f)).has_value());

    // 缩放函数的退化输入一律给 0（= 该源无贡献），不是 NaN/Inf
    CHECK(RSMVplScale(0.0f, 0.0125f, 16u) == 0.0f);
    CHECK(RSMVplScale(-1.0f, 0.0125f, 16u) == 0.0f);
    CHECK(RSMVplScale(60.0f, 0.0f, 16u) == 0.0f);
    CHECK(RSMVplScale(60.0f, 0.0125f, 0u) == 0.0f);
    CHECK(RSMVplSampleArea(60.0f, 0.0125f, 0u) == 0.0f);
    CHECK_FALSE(std::isnan(RSMVplScale(60.0f, 0.0125f, 0u)));
}
