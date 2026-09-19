// ============================================================
// Tests/TestSurfaceCache.cpp — 步骤 14 的页状态机单测
//
// 验收要求：状态迁移合法性（Invalid→Requested→Allocating→Capturing→Captured→Dirty）
// 与"非法迁移可断言"。这里同时覆盖：
//   · 真值表本身（每条合法边、若干非法边）；
//   · 状态机 API 走完一整条生命周期；
//   · 非法迁移被拒绝且**表不进入非法状态**（Release 语义）；
//   · 校验和的可复现性与"字段参与度"（改任一字段校验和都要变）。
// ============================================================

#include "Lumen/SurfaceCacheTypes.h"

#include <doctest/doctest.h>
#include <cstring>

using namespace he;
using namespace he::render;

TEST_CASE("SurfaceCache: 六态常量与名字") {
    CHECK(kSCPageState_Invalid    == 0u);
    CHECK(kSCPageState_Requested  == 1u);
    CHECK(kSCPageState_Allocating == 2u);
    CHECK(kSCPageState_Capturing  == 3u);
    CHECK(kSCPageState_Captured   == 4u);
    CHECK(kSCPageState_Dirty      == 5u);
    CHECK(kSCPageState_Count      == 6u);
    CHECK(std::strcmp(SurfaceCachePageStateName(kSCPageState_Capturing), "Capturing") == 0);   // doctest 对 std::string 的 stringify 会炸，用 strcmp
}

TEST_CASE("SurfaceCache: 迁移真值表") {
    // 生命周期上的每一条合法边
    CHECK(IsLegalPageTransition(kSCPageState_Invalid,    kSCPageState_Requested));
    CHECK(IsLegalPageTransition(kSCPageState_Requested,  kSCPageState_Allocating));
    CHECK(IsLegalPageTransition(kSCPageState_Allocating, kSCPageState_Capturing));
    CHECK(IsLegalPageTransition(kSCPageState_Capturing,  kSCPageState_Captured));
    CHECK(IsLegalPageTransition(kSCPageState_Captured,   kSCPageState_Dirty));
    CHECK(IsLegalPageTransition(kSCPageState_Dirty,      kSCPageState_Capturing));
    // 兜底：任何状态都能被释放
    for (u32 s = 0; s < kSCPageState_Count; ++s)
        CHECK(IsLegalPageTransition(s, kSCPageState_Invalid));
    // 非法边（跳步/回退）
    CHECK_FALSE(IsLegalPageTransition(kSCPageState_Invalid,   kSCPageState_Captured));
    CHECK_FALSE(IsLegalPageTransition(kSCPageState_Requested, kSCPageState_Captured));
    CHECK_FALSE(IsLegalPageTransition(kSCPageState_Captured,  kSCPageState_Requested));
    CHECK_FALSE(IsLegalPageTransition(kSCPageState_Captured,  kSCPageState_Capturing));
    CHECK_FALSE(IsLegalPageTransition(kSCPageState_Dirty,     kSCPageState_Captured));
    CHECK_FALSE(IsLegalPageTransition(kSCPageState_Capturing, kSCPageState_Dirty));
    CHECK_FALSE(IsLegalPageTransition(kSCPageState_Count,     kSCPageState_Captured));   // 非法枚举值
}

TEST_CASE("SurfaceCache: 走完一条生命周期") {
    SurfaceCachePageTable table;
    table.Resize(4);
    CHECK(table.Size() == 4);
    CHECK(table.Count(kSCPageState_Invalid) == 4);
    CHECK_FALSE(table.AnyLive());

    CHECK(table.Request(0, /*card*/ 42, /*frame*/ 7));
    CHECK(table.Get(0).state == kSCPageState_Requested);
    CHECK(table.Get(0).cardIndex == 42);
    CHECK(table.Get(0).pageIndex == kSCInvalidPage);

    CHECK(table.Allocate(0, /*physical*/ 3));
    CHECK(table.Get(0).state == kSCPageState_Allocating);
    CHECK(table.Get(0).pageIndex == 3);

    CHECK(table.BeginCapture(0));
    CHECK(table.Get(0).state == kSCPageState_Capturing);

    CHECK(table.EndCapture(0, true));
    CHECK(table.Get(0).state == kSCPageState_Captured);
    CHECK(table.Get(0).captureCount == 1);

    CHECK(table.MarkDirty(0));
    CHECK(table.Get(0).state == kSCPageState_Dirty);

    CHECK(table.BeginCapture(0));           // 重捕获
    CHECK(table.EndCapture(0, true));
    CHECK(table.Get(0).captureCount == 2);

    CHECK(table.Evict(0));
    CHECK(table.Get(0).state == kSCPageState_Invalid);
    CHECK(table.Get(0).pageIndex == kSCInvalidPage);
    CHECK_FALSE(table.AnyLive());
    CHECK(table.Evict(0));                  // 幂等
}

TEST_CASE("SurfaceCache: 非法迁移被拒绝且不改状态") {
    SurfaceCachePageTable table;
    table.Resize(2);
    REQUIRE(table.Request(0, 1, 0));
    REQUIRE(table.Allocate(0, 1));
    REQUIRE(table.BeginCapture(0));
    REQUIRE(table.EndCapture(0, true));

    // Captured 不能再进 Capturing（必须先 Dirty）
    CHECK_FALSE(table.BeginCapture(0));
    CHECK(table.Get(0).state == kSCPageState_Captured);
    // Captured 不能直接回 Requested
    CHECK_FALSE(table.Request(0, 9, 1));
    CHECK(table.Get(0).state == kSCPageState_Captured);
    // 捕获失败 → Invalid（合法兜底）
    REQUIRE(table.MarkDirty(0));
    REQUIRE(table.BeginCapture(0));
    CHECK(table.EndCapture(0, /*ok*/ false));
    CHECK(table.Get(0).state == kSCPageState_Invalid);
    // 越界页号
    CHECK_FALSE(table.Request(99, 0, 0));
}

TEST_CASE("SurfaceCache: 校验和随任一字段变化") {
    SurfaceCachePageTable a;
    a.Resize(3);
    REQUIRE(a.Request(1, 5, 10));

    SurfaceCachePageTable b;
    b.Resize(3);
    REQUIRE(b.Request(1, 5, 10));
    CHECK(a.Checksum() == b.Checksum());      // 相同输入 ⇒ 相同校验和（可复现）

    SurfaceCachePageTable c;
    c.Resize(3);
    REQUIRE(c.Request(1, 5, 11));             // 只改帧号
    CHECK(a.Checksum() != c.Checksum());

    SurfaceCachePageTable d;
    d.Resize(3);
    REQUIRE(d.Request(1, 6, 10));             // 只改卡片号
    CHECK(a.Checksum() != d.Checksum());

    SurfaceCachePageTable e2;
    e2.Resize(4);                             // 只改页数
    CHECK(a.Checksum() != e2.Checksum());
}
