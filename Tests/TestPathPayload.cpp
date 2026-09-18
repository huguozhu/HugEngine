// ============================================================
// Tests/TestPathPayload.cpp — 路径追踪载荷布局的 CPU 单元测试（PT 任务 1）
//
// 覆盖范围（**纯 CPU、无 RHI**）：
//   1. C++ 镜像结构的大小与逐字段偏移（与 PT_Common.slang 的 struct 对称）
//   2. 默认值与 Slang 侧一致（等价于「材质没有 Disney 扩展」）
//   3. 可平凡复制 / 标准布局（作为 RT 载荷按字节传递的前提）
//   4. 按字节往返：字段写入 → memcpy → 读回，逐字段不变（模拟 GPU 载荷传递）
//
// 为什么能脱离 RHI：`RT/PathPayload.h` 只依赖 Math/Math.h 与标准库，
// 故本文件只需把 `Engine/Render` 加入 include 路径，**无需链接 HugEngineRender**。
// ============================================================

#include "doctest.h"

#include "RT/PathPayload.h"   // PathPayload（C++ 镜像）

#include <cstring>
#include <type_traits>

using namespace he;
using namespace he::render;

TEST_CASE("PathPayload：大小与逐字段偏移与 Slang 侧一致") {
    CHECK(sizeof(PathPayload) == 96);
    CHECK(kPathPayloadSize == 96);

    // 6 × float4，无隐式填充
    CHECK(offsetof(PathPayload, albedoMetallic) == 0);
    CHECK(offsetof(PathPayload, normalRough)    == 16);
    CHECK(offsetof(PathPayload, emissiveT)      == 32);
    CHECK(offsetof(PathPayload, disneyA)        == 48);
    CHECK(offsetof(PathPayload, disneyB)        == 64);
    CHECK(offsetof(PathPayload, surfaceParams)  == 80);

    CHECK(std::is_trivially_copyable_v<PathPayload>);
    CHECK(std::is_standard_layout_v<PathPayload>);
}

TEST_CASE("PathPayload：默认值与「无 Disney 扩展」等价") {
    const PathPayload p;

    // 基础命中信息：albedo/normal 零值、hitT=-1（未命中的哨兵）
    CHECK(p.albedoMetallic.x == 0.0f);
    CHECK(p.albedoMetallic.w == 0.0f);
    CHECK(p.normalRough.x == 0.0f);
    CHECK(p.normalRough.w == 1.0f);
    CHECK(p.emissiveT.w == -1.0f);

    // Disney 默认：aniso=0, subsurface=0, specular=0.5, sheen=0
    CHECK(p.disneyA.x == 0.0f);
    CHECK(p.disneyA.y == 0.0f);
    CHECK(p.disneyA.z == 0.5f);
    CHECK(p.disneyA.w == 0.0f);

    // clearcoat=0, clearcoatGloss=1, specularTint=(1,1)
    CHECK(p.disneyB.x == 0.0f);
    CHECK(p.disneyB.y == 1.0f);
    CHECK(p.disneyB.z == 1.0f);
    CHECK(p.disneyB.w == 1.0f);

    // disneyC=1, F0=0.04（IOR=1.5 推导）, ior=1.5, transmission=0（预留）
    CHECK(p.surfaceParams.x == 1.0f);
    CHECK(p.surfaceParams.y == doctest::Approx(0.04f));
    CHECK(p.surfaceParams.z == 1.5f);
    CHECK(p.surfaceParams.w == 0.0f);
}

TEST_CASE("PathPayload：按字节往返后逐字段不变") {
    PathPayload src;
    src.albedoMetallic = float4(0.25f, 0.5f, 0.75f, 1.0f);
    src.normalRough    = float4(0.0f, 1.0f, 0.0f, 0.4f);
    src.emissiveT      = float4(1.0f, 2.0f, 3.0f, 12.5f);
    src.disneyA        = float4(0.1f, 0.2f, 0.3f, 0.4f);
    src.disneyB        = float4(0.5f, 0.6f, 0.7f, 0.8f);
    src.surfaceParams  = float4(0.9f, 0.04f, 1.45f, 0.33f);

    // 模拟「rchit 写入 → 载荷按字节交给 rgen 读取」
    unsigned char bytes[sizeof(PathPayload)];
    std::memcpy(bytes, &src, sizeof(PathPayload));
    PathPayload dst;
    std::memcpy(&dst, bytes, sizeof(PathPayload));

    // 逐分量比较（glm::vec4 没有 doctest 的 stringify 支持，避免直接比整个向量）
    auto sameVec = [](const float4& a, const float4& b) {
        return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w;
    };
    CHECK(sameVec(dst.albedoMetallic, src.albedoMetallic));
    CHECK(sameVec(dst.normalRough,    src.normalRough));
    CHECK(sameVec(dst.emissiveT,      src.emissiveT));
    CHECK(sameVec(dst.disneyA,        src.disneyA));
    CHECK(sameVec(dst.disneyB,        src.disneyB));
    CHECK(sameVec(dst.surfaceParams,  src.surfaceParams));
}
