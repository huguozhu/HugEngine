// ============================================================
// Tests/TestPTMaterialParams.cpp — Disney 材质参数打包的 CPU 单元测试（PT 任务 2）
//
// 覆盖范围（**纯 CPU、无 RHI**）：
//   1. 默认参数打包 == pbr_common.slang 里 PBR_BRDF 的默认实参（等价性）
//   2. 非默认参数逐字段打包正确（含 F0 = (ior-1)²/(ior+1)² 推导）
//   3. 打包结果与 PathPayload 的默认 Disney 字段一致（两条数据链不漂移）
//
// 为什么重要：PT 是参考渲染器，"PT 与光栅化对同一材质算出不同 BRDF" 会直接
// 污染对照结论。两侧现在都调用共享的 PBR_BRDF，参数打包也收敛到
// RT/PTMaterialParams.h 一处，本测试把这份约束钉住。
//
// 为什么能脱离 RHI：PTMaterialParams.h / PathPayload.h 只依赖 Core/Types.h
// 与 Math/Math.h，故本文件只需把 `Engine/Render` 加入 include 路径。
// ============================================================

#include "doctest.h"

#include "RT/PTMaterialParams.h"
#include "RT/PathPayload.h"

using namespace he;
using namespace he::render;

namespace {

/// 逐分量比较（glm::vec4 没有 doctest 的 stringify 支持，避免直接比整个向量）
bool SameVec(const float4& a, const float4& b) {
    return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w;
}

/// PBR_BRDF 的默认实参（pbr_common.slang:149-155）：
/// dielectricF0 = 0.04, disneyA = (0,0,0.5,0), disneyB = (0,1,1,1), disneyC = 1
constexpr float kBrdfDefaultF0   = 0.04f;
const float4    kBrdfDefaultA    = float4(0.0f, 0.0f, 0.5f, 0.0f);
const float4    kBrdfDefaultB    = float4(0.0f, 1.0f, 1.0f, 1.0f);
constexpr float kBrdfDefaultC    = 1.0f;

} // namespace

TEST_CASE("PTMaterialParams：默认材质的打包结果与 PBR_BRDF 默认实参一致") {
    const PTMaterialParams p = PackDisneyParams(
        0.0f, 0.0f, 0.5f, 0.0f,   // aniso / subsurface / specular / sheen
        0.0f, 1.0f,               // clearcoat / clearcoatGloss
        1.0f, 1.0f, 1.0f,         // specularTint
        kDefaultIOR, 0.0f);       // ior / transmission

    CHECK(SameVec(p.disneyA, kBrdfDefaultA));
    CHECK(SameVec(p.disneyB, kBrdfDefaultB));
    CHECK(p.surfaceParams.x == kBrdfDefaultC);
    CHECK(p.surfaceParams.y == doctest::Approx(kBrdfDefaultF0));
    CHECK(p.surfaceParams.z == kDefaultIOR);
    CHECK(p.surfaceParams.w == 0.0f);

    // 常量本身也必须与默认实参一致
    CHECK(SameVec(kDefaultDisneyA, kBrdfDefaultA));
    CHECK(SameVec(kDefaultDisneyB, kBrdfDefaultB));
    CHECK(kDefaultDisneyC == kBrdfDefaultC);
}

TEST_CASE("PTMaterialParams：非默认参数逐字段打包 + IOR→F0 推导") {
    const float ior = 1.33f;   // 水
    const PTMaterialParams p = PackDisneyParams(
        0.3f, 0.1f, 0.8f, 0.2f,
        0.5f, 0.7f,
        0.9f, 0.8f, 0.7f,
        ior, 0.25f);

    CHECK(p.disneyA.x == 0.3f);
    CHECK(p.disneyA.y == 0.1f);
    CHECK(p.disneyA.z == 0.8f);
    CHECK(p.disneyA.w == 0.2f);

    CHECK(p.disneyB.x == 0.5f);
    CHECK(p.disneyB.y == 0.7f);
    CHECK(p.disneyB.z == 0.9f);
    CHECK(p.disneyB.w == 0.8f);

    CHECK(p.surfaceParams.x == 0.7f);   // disneyC = specularTint.b
    CHECK(p.surfaceParams.z == ior);
    CHECK(p.surfaceParams.w == 0.25f);  // transmission（预留字段位）

    // F0 = (ior-1)²/(ior+1)²
    const float expectedF0 = (ior - 1.0f) * (ior - 1.0f) / ((ior + 1.0f) * (ior + 1.0f));
    CHECK(p.surfaceParams.y == doctest::Approx(expectedF0));
    CHECK(DielectricF0FromIOR(1.5f) == doctest::Approx(0.04f));
    CHECK(DielectricF0FromIOR(1.0f) == doctest::Approx(0.0f));
}

TEST_CASE("PTMaterialParams：与 PathPayload 的默认 Disney 字段一致") {
    const PTMaterialParams p;      // 默认构造
    const PathPayload    payload;  // 默认构造

    CHECK(SameVec(p.disneyA, payload.disneyA));
    CHECK(SameVec(p.disneyB, payload.disneyB));
    CHECK(SameVec(p.surfaceParams, payload.surfaceParams));
}
