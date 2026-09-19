// ============================================================
// Tests/TestLumenTraceConfig.cpp — 步骤 21 的二维配置校验单测
//
// 验收要求："非法组合在配置加载期即报错"。这里把合法/非法组合逐条钉住：
//   · SDF × HitLighting 必须被拒（SDF 拿不到重心坐标/材质）
//   · traceRep 必须在 8–16（§6）
//   · shadeRep ≥ 1
//   · trace = Screen 与 screenTrace = false 自相矛盾
// ============================================================

#include "Lumen/LumenTraceConfig.h"

#include <doctest/doctest.h>

using namespace he;
using namespace he::render;

TEST_CASE("LumenTraceConfig: 首版默认配置合法") {
    LumenTraceConfig cfg;   // 首版：SDF(+HW 远场) × SurfaceCache、screenTrace = false、traceRep = 8
    CHECK(cfg.trace == LumenTraceSource::SDF);
    CHECK(cfg.shade == LumenShadeSource::SurfaceCache);
    CHECK(cfg.traceRep == 8u);
    CHECK(cfg.shadeRep == 1u);
    CHECK(cfg.screenTrace == false);
    CHECK(cfg.Validate().empty());
}

TEST_CASE("LumenTraceConfig: SDF × HitLighting 必须被拒绝") {
    LumenTraceConfig cfg;
    cfg.shade = LumenShadeSource::HitLighting;
    const std::string err = cfg.Validate();
    CHECK_FALSE(err.empty());
    CHECK(err.find("SDF") != std::string::npos);
    CHECK(err.find("HitLighting") != std::string::npos);
    // 换成硬件光追追踪就合法（光追的 closest-hit 才能给重心坐标与材质）
    cfg.trace = LumenTraceSource::HardwareRT;
    CHECK(cfg.Validate().empty());
}

TEST_CASE("LumenTraceConfig: 光线数必须在 8–16") {
    LumenTraceConfig cfg;
    cfg.traceRep = 7;
    CHECK_FALSE(cfg.Validate().empty());
    cfg.traceRep = 17;
    CHECK_FALSE(cfg.Validate().empty());
    for (u32 n = 8; n <= 16; ++n) { cfg.traceRep = n; CHECK(cfg.Validate().empty()); }
}

TEST_CASE("LumenTraceConfig: shadeRep 与 screenTrace 的一致性") {
    LumenTraceConfig cfg;
    cfg.shadeRep = 0;
    CHECK_FALSE(cfg.Validate().empty());
    cfg.shadeRep = 2;
    CHECK(cfg.Validate().empty());

    cfg.trace = LumenTraceSource::Screen;      // 声明追屏幕空间却关着开关 ⇒ 拒绝
    CHECK_FALSE(cfg.Validate().empty());
    cfg.screenTrace = true;
    CHECK(cfg.Validate().empty());
}

TEST_CASE("LumenTraceConfig: 名字与枚举一一对应") {
    CHECK(std::string(LumenTraceSourceName(LumenTraceSource::SDF)) == std::string("SDF"));
    CHECK(std::string(LumenTraceSourceName(LumenTraceSource::HardwareRT)) == std::string("HardwareRT"));
    CHECK(std::string(LumenShadeSourceName(LumenShadeSource::SurfaceCache)) == std::string("SurfaceCache"));
    CHECK(std::string(LumenShadeSourceName(LumenShadeSource::HitLighting)) == std::string("HitLighting"));
}
