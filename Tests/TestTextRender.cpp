// ============================================================
// Tests/TestTextRender.cpp — Phase A5 TextRender 单元测试
//
// 覆盖：文字栅格化（ASCII/CJK、字体兜底链）、
//       脏标记 → 尺寸更新、文字颜色 → 材质色同步。
// 注意：无 RHI 设备时只验证 CPU 栅格化与组件逻辑（纹理路径需运行时冒烟）。
// ============================================================

#include "doctest.h"

#include "Scene/World.h"
#include "Scene/Transform.h"
#include "Scene/TextRenderComponent.h"
#include "Scene/TextRenderSystem.h"

#include <filesystem>
#include <algorithm>

using namespace he;

namespace {
// 统计位图中有覆盖（alpha>0）的像素数
usize CountCoveredPixels(u32 w, u32 h, const std::vector<u8>& px) {
    usize n = 0;
    for (usize i = 3; i < px.size(); i += 4)
        if (px[i] > 0) ++n;
    return n;
}

// 系统字体兜底链中是否存在任一字体文件（Windows 开发机恒真）
bool AnyFallbackFontExists() {
    for (const char* f : {"C:/Windows/Fonts/msyh.ttc", "C:/Windows/Fonts/simhei.ttf",
                          "C:/Windows/Fonts/simsun.ttc", "C:/Windows/Fonts/arial.ttf"})
        if (std::filesystem::exists(f)) return true;
    return false;
}
} // namespace

TEST_CASE("RasterizeText 字体缺失时走兜底链（ASCII 恒有输出）") {
    u32 w = 0, h = 0;
    std::vector<u8> px;
    // 不存在的字体路径 → 依次尝试系统字体 → 内置 ASCII 位图字体兜底，恒成功
    bool ok = TextRenderSystem::RasterizeText("ABC 123", "Z:/nonexistent.ttf", 32, w, h, px);
    REQUIRE(ok == true);
    CHECK(w > 0);
    CHECK(h > 0);
    CHECK(px.size() == (usize)w * h * 4);
    CHECK(CountCoveredPixels(w, h, px) > 0);   // 有实际字形覆盖
}

TEST_CASE("RasterizeText 渲染 CJK 文字（系统字体兜底）") {
    if (!AnyFallbackFontExists()) {
        MESSAGE("本机无系统字体，跳过 CJK 用例");
        return;
    }
    u32 w = 0, h = 0;
    std::vector<u8> px;
    bool ok = TextRenderSystem::RasterizeText("你好，世界", "", 48, w, h, px);
    REQUIRE(ok == true);
    CHECK(w > 0);
    CHECK(h > 0);
    // 5 个 CJK 字符的覆盖像素远大于 1 个
    usize covered = CountCoveredPixels(w, h, px);
    CHECK(covered > 100);

    // 相同字号下，更长的文字产生更宽的位图
    u32 w2 = 0, h2 = 0;
    std::vector<u8> px2;
    REQUIRE(TextRenderSystem::RasterizeText("你", "", 48, w2, h2, px2));
    CHECK(w > w2);
}

TEST_CASE("TextRenderSystem 脏标记驱动尺寸更新（无设备）") {
    World world;
    Entity e = world.CreateEntity("Text");
    world.AddComponent<TransformComponent>(e);
    auto* tr = world.AddComponent<TextRenderComponent>(e);
    tr->text     = "Hello";
    tr->fontSize = 32.0f;

    // 初始 dirty → Update 后栅格化并更新 size（世界尺寸 = 位图像素 / 100）
    CHECK(tr->IsDirty() == true);
    TextRenderSystem::Update(world, nullptr);
    CHECK(tr->IsDirty() == false);
    CHECK(tr->size.x > 0.0f);
    CHECK(tr->size.y > 0.0f);

    // 未变化 → 尺寸不变
    float sx = tr->size.x;
    TextRenderSystem::Update(world, nullptr);
    CHECK(tr->size.x == doctest::Approx(sx));

    // 改文字 → 再次 dirty → 尺寸随内容变化
    tr->text = "Hello World!";
    CHECK(tr->IsDirty() == true);
    TextRenderSystem::Update(world, nullptr);
    CHECK(tr->IsDirty() == false);
    CHECK(tr->size.x > doctest::Approx(sx).epsilon(0.001));   // 更长的文字 → 更宽

    // 改字号 → 尺寸同步变化
    float h1 = tr->size.y;
    tr->fontSize = 96.0f;
    TextRenderSystem::Update(world, nullptr);
    CHECK(tr->size.y > h1);
}

TEST_CASE("TextRenderSystem 文字颜色同步到材质色") {
    World world;
    Entity e = world.CreateEntity("Text");
    world.AddComponent<TransformComponent>(e);
    auto* tr = world.AddComponent<TextRenderComponent>(e);
    tr->textColor = float4(1.0f, 0.5f, 0.2f, 0.9f);

    TextRenderSystem::Update(world, nullptr);
    CHECK(tr->baseColorFactor.x == doctest::Approx(1.0f));
    CHECK(tr->baseColorFactor.y == doctest::Approx(0.5f));
    CHECK(tr->baseColorFactor.z == doctest::Approx(0.2f));
    CHECK(tr->baseColorFactor.w == doctest::Approx(0.9f));
}
