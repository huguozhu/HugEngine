// ============================================================
// Tests/TestDenoiseSignal.cpp — 步骤 34（《Lumen设计与实现》§10 的 11.3）框架层校验
//
// 统一降噪框架有两件与设备无关、因而可以钉死在单测里的事：
//   ① **有效性契约**须为唯一真值：`alpha < 0` = 本条无数据，且"值无效"与"权重不计"
//      是同一件事的两半（少了后一半，同通道的其它源会被无数据源稀释 → 画面变暗）；
//   ② **信号登记**的计数语义：有空间引导 / 需重建升采样 / 有时域速度三类必须能数清楚，
//      否则 `LogSummary()` 打印出来的"多信号共存"就无法核对。
// 这两条都是"框架真的看见了什么"的判据，不需要 Vulkan 设备。
// ============================================================

#include "PostProcess/DenoiseSignal.h"

#include <doctest/doctest.h>

#include <cstring>

using namespace he;
using namespace he::render;

TEST_CASE("DenoiseSignal: 有效性契约只有 alpha 符号一个判据") {
    // 契约的两半：>= 0 有效、< 0 无效；0 本身是**有效**值（本帧估计为 0 与"没数据"不同）
    CHECK(denoise::IsValid(0.0f));
    CHECK(denoise::IsValid(1.0f));
    CHECK(denoise::IsValid(0.0001f));
    CHECK_FALSE(denoise::IsValid(-0.0001f));
    CHECK_FALSE(denoise::IsValid(denoise::kInvalidAlpha));
    // 契约文本必须把"权重也不计入分母"写明：这正是 §9.2-C 那次"画面莫名变暗"的根因
    const char* desc = denoise::ValidityContractName();
    CHECK(std::strstr(desc, "alpha < 0") != nullptr);
    CHECK(std::strstr(desc, "分母") != nullptr);
}

TEST_CASE("DenoiseSignal: 空间引导判定要求深度与法线同时存在") {
    rhi::IRHITexture* fakeDepth  = reinterpret_cast<rhi::IRHITexture*>(0x1);
    rhi::IRHITexture* fakeNormal = reinterpret_cast<rhi::IRHITexture*>(0x2);
    DenoiseSignal s;
    CHECK_FALSE(s.HasSpatialGuide());          // 两者都空：只能全屏滤波
    s.depth = fakeDepth;
    CHECK_FALSE(s.HasSpatialGuide());          // 只有深度：不足以判边
    s.normal = fakeNormal;
    CHECK(s.HasSpatialGuide());
}

TEST_CASE("DenoiseSignalRegistry: 三类计数与清空") {
    rhi::IRHITexture* fakeA = reinterpret_cast<rhi::IRHITexture*>(0x10);
    rhi::IRHITexture* fakeB = reinterpret_cast<rhi::IRHITexture*>(0x20);

    DenoiseSignalRegistry reg;
    CHECK(reg.Count() == 0u);

    DenoiseSignal full;                        // 全分辨率 + 空间引导 + 时域
    full.name = "SSGI"; full.depth = fakeA; full.normal = fakeB; full.velocity = fakeB;
    full.width = 1920; full.height = 1080; full.targetWidth = 1920; full.targetHeight = 1080;
    reg.Register(full);

    DenoiseSignal half;                        // 半分辨率：必须降噪 + 重建升采样
    half.name = "SSR"; half.depth = fakeA; half.normal = fakeB;
    half.width = 960; half.height = 540; half.targetWidth = 1920; half.targetHeight = 1080;
    half.needsUpscale = true;
    reg.Register(half);

    DenoiseSignal noGuide;                     // 无引导（例如纯 compute 输出的探针缓存）
    noGuide.name = "Lumen_ProbeCache";
    noGuide.width = 512; noGuide.height = 512;
    noGuide.targetWidth = 512; noGuide.targetHeight = 512;
    reg.Register(noGuide);

    CHECK(reg.Count() == 3u);
    CHECK(reg.DenoiseCount() == 2u);           // 前两个有屏幕空间引导
    CHECK(reg.UpscaleCount() == 1u);           // 只有 SSR 需要重建升采样
    CHECK(reg.TemporalCount() == 1u);          // 只有 SSGI 带速度缓冲
    // 名字是 std::string：doctest 无法 stringify 它和字面量的比较（C2679），故用 strcmp
    CHECK(std::strcmp(reg.Get(0).name.c_str(), "SSGI") == 0);
    CHECK(reg.Get(2).targetWidth == 512u);

    // 每帧 Clear：信号集合是当帧事实，不该残留上一帧已关闭的源
    reg.Clear();
    CHECK(reg.Count() == 0u);
    CHECK(reg.UpscaleCount() == 0u);
}

TEST_CASE("DenoiseSignal: 默认值即改造前的行为（引导参数 10 / 8）") {
    DenoiseSignal s;
    CHECK(s.depthSigma == doctest::Approx(10.0f));
    CHECK(s.normalSigma == doctest::Approx(8.0f));
    CHECK_FALSE(s.needsUpscale);
    CHECK(s.width == 0u);
    CHECK(s.height == 0u);
}
