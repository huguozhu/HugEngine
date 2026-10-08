// ============================================================
// TestRenderThreadingMode.cpp — 阶段 0 T0.5：三态渲染线程模式参数
//
// 【为什么要有它】这个参数决定"渲染跑在哪根线程上"，一旦解析口径出错（比如把 "2" 当成
// 单线程），验收会拿到"看起来通过"的假数据。因此把三件事钉死：
//   ① 默认值必须是单线程（可直接回退的现状）；
//   ② 三个模式 × 两个判据（UsesRenderThread / UsesRHIThread）的真值表；
//   ③ 解析：数字与名称/别名、大小写不敏感、非法输入必须返回 nullopt（不能静默退回）。
// ============================================================
#include "Core/Engine.h"
#include "Core/RenderThreadingMode.h"

#include <doctest/doctest.h>

#include <string>

using namespace he;

TEST_CASE("RenderThreadingMode：默认单线程，判据为假") {
    EngineConfig config;                                        // 不构造 Engine（那会建窗口）
    CHECK(config.renderThreadingMode == RenderThreadingMode::SingleThreaded);
    CHECK_FALSE(config.UsesRenderThread());
    CHECK_FALSE(config.UsesRHIThread());
    CHECK(config.renderThreadSpinWaitUs == 0u);                 // 默认不自旋（空闲不烧核）
}

TEST_CASE("RenderThreadingMode：三态 × 两个判据的真值表") {
    EngineConfig config;

    config.renderThreadingMode = RenderThreadingMode::SingleThreaded;
    CHECK_FALSE(config.UsesRenderThread());
    CHECK_FALSE(config.UsesRHIThread());

    config.renderThreadingMode = RenderThreadingMode::RenderThread;
    CHECK(config.UsesRenderThread());                           // 有渲染线程
    CHECK_FALSE(config.UsesRHIThread());                        // 但没有 RHI 线程

    config.renderThreadingMode = RenderThreadingMode::RenderThreadAndRHI;
    CHECK(config.UsesRenderThread());                           // 三线程模式同时含渲染线程
    CHECK(config.UsesRHIThread());
}

TEST_CASE("RenderThreadingMode：规范名与枚举值一一对应") {
    CHECK(RenderThreadingMode::SingleThreaded     == static_cast<RenderThreadingMode>(0));
    CHECK(RenderThreadingMode::RenderThread       == static_cast<RenderThreadingMode>(1));
    CHECK(RenderThreadingMode::RenderThreadAndRHI == static_cast<RenderThreadingMode>(2));

    CHECK(std::string(RenderThreadingModeName(RenderThreadingMode::SingleThreaded)) == "single-threaded");
    CHECK(std::string(RenderThreadingModeName(RenderThreadingMode::RenderThread)) == "game+render");
    CHECK(std::string(RenderThreadingModeName(RenderThreadingMode::RenderThreadAndRHI)) == "game+render+rhi");
}

TEST_CASE("RenderThreadingMode：解析数字、名称与别名（大小写不敏感）") {
    // 数字
    CHECK(ParseRenderThreadingMode("0") == RenderThreadingMode::SingleThreaded);
    CHECK(ParseRenderThreadingMode("1") == RenderThreadingMode::RenderThread);
    CHECK(ParseRenderThreadingMode("2") == RenderThreadingMode::RenderThreadAndRHI);

    // 规范名
    CHECK(ParseRenderThreadingMode("single-threaded") == RenderThreadingMode::SingleThreaded);
    CHECK(ParseRenderThreadingMode("game+render") == RenderThreadingMode::RenderThread);
    CHECK(ParseRenderThreadingMode("game+render+rhi") == RenderThreadingMode::RenderThreadAndRHI);

    // 别名 + 大小写
    CHECK(ParseRenderThreadingMode("SINGLE") == RenderThreadingMode::SingleThreaded);
    CHECK(ParseRenderThreadingMode("Off") == RenderThreadingMode::SingleThreaded);
    CHECK(ParseRenderThreadingMode("RenderThread") == RenderThreadingMode::RenderThread);
    CHECK(ParseRenderThreadingMode("RHI") == RenderThreadingMode::RenderThreadAndRHI);
}

TEST_CASE("RenderThreadingMode：非法输入返回 nullopt（不静默退回）") {
    CHECK_FALSE(ParseRenderThreadingMode("").has_value());
    CHECK_FALSE(ParseRenderThreadingMode("3").has_value());     // 只定义了 0/1/2
    CHECK_FALSE(ParseRenderThreadingMode("-1").has_value());
    CHECK_FALSE(ParseRenderThreadingMode("triple").has_value());
    CHECK_FALSE(ParseRenderThreadingMode("render ").has_value()); // 尾部空格不视为合法
}

TEST_CASE("RenderThreadingMode：进程级模式的读写与判据一致") {
    const RenderThreadingMode saved = GetRenderThreadingMode();  // 复位用（避免污染其它用例）

    SetRenderThreadingMode(RenderThreadingMode::SingleThreaded);
    CHECK(GetRenderThreadingMode() == RenderThreadingMode::SingleThreaded);
    CHECK_FALSE(UsesRenderThread());
    CHECK_FALSE(UsesRHIThread());

    SetRenderThreadingMode(RenderThreadingMode::RenderThread);
    CHECK(UsesRenderThread());
    CHECK_FALSE(UsesRHIThread());

    SetRenderThreadingMode(RenderThreadingMode::RenderThreadAndRHI);
    CHECK(UsesRenderThread());
    CHECK(UsesRHIThread());

    SetRenderThreadingMode(saved);
}
