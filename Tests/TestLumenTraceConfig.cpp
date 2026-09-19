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

#include <cstring>
#include <string>

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

// ============================================================
// 步骤 28：§6 的组合表必须逐行写进**代码**（而不是只写在文档里）
//   SDF × SurfaceCache    ✅ 首版  → Implemented
//   HW(RT) × SurfaceCache ✅      → Implemented
//   HW(RT) × HitLighting  ✅ 二版  → LegalNotImplemented（必须显式报"未实现"）
//   SDF × HitLighting     ❌      → Illegal
//   Screen × 任意         ✅ 优先层 → LegalNotImplemented
// ============================================================
TEST_CASE("LumenTraceConfig: §6 组合表逐行判定（步骤 28）") {
    // doctest 无法直接字符串化这个枚举 ⇒ 统一转成 u32 比较
    auto cls = [](LumenTraceSource tr, LumenShadeSource sh, bool st) {
        return (u32)LumenClassifyCombination(tr, sh, st);
    };
    const u32 kImpl  = (u32)LumenCombinationStatus::Implemented;
    const u32 kLegal = (u32)LumenCombinationStatus::LegalNotImplemented;
    const u32 kIll   = (u32)LumenCombinationStatus::Illegal;
    // 首版已实现的两行
    CHECK(cls(LumenTraceSource::SDF, LumenShadeSource::SurfaceCache, false) == kImpl);
    CHECK(cls(LumenTraceSource::HardwareRT, LumenShadeSource::SurfaceCache, false) == kImpl);
    // Neutral 是调试/降级路径：任何追踪源都合法且已实现
    CHECK(cls(LumenTraceSource::SDF, LumenShadeSource::Neutral, false) == kImpl);
    CHECK(cls(LumenTraceSource::HardwareRT, LumenShadeSource::Neutral, false) == kImpl);
    // 合法但首版未实现：Hit Lighting 需要材质求值 + NEE 直接光（第二版）
    CHECK(cls(LumenTraceSource::HardwareRT, LumenShadeSource::HitLighting, false) == kLegal);
    // 合法但首版未实现：屏幕空间追踪是"优先层"
    CHECK(cls(LumenTraceSource::Screen, LumenShadeSource::SurfaceCache, true) == kLegal);
    CHECK(cls(LumenTraceSource::SDF, LumenShadeSource::SurfaceCache, true) == kLegal);
    // 非法：SDF 拿不到命中点的三角形信息
    CHECK(cls(LumenTraceSource::SDF, LumenShadeSource::HitLighting, false) == kIll);
    CHECK(cls(LumenTraceSource::SDF, LumenShadeSource::HitLighting, true) == kIll);
}

TEST_CASE("LumenTraceConfig: 组合状态的中文名可读且互不相同") {
    // 用 strcmp 而不是 std::string 比较：doctest 对这个表达式做字符串化时会把 char 也塞进流，
    // MSVC 下会报 C2679（找不到 operator<<）。判据本身等价，且更少依赖 doctest 的格式化。
    CHECK(std::strcmp(LumenCombinationStatusName(LumenCombinationStatus::Implemented), "已实现") == 0);
    CHECK(std::strcmp(LumenCombinationStatusName(LumenCombinationStatus::LegalNotImplemented),
                      "合法但首版未实现") == 0);
    CHECK(std::strcmp(LumenCombinationStatusName(LumenCombinationStatus::Illegal), "非法组合") == 0);
}

TEST_CASE("LumenTraceConfig: 合法但未实现的组合仍必须通过 Validate（非法与未实现不是一回事）") {
    LumenTraceConfig cfg;
    cfg.trace = LumenTraceSource::HardwareRT;
    cfg.shade = LumenShadeSource::HitLighting;
    CHECK(cfg.Validate().empty());   // 合法：配置允许，只是首版未实现（运行期显式报"未实现"）
    CHECK((u32)LumenClassifyCombination(cfg.trace, cfg.shade, cfg.screenTrace)
          == (u32)LumenCombinationStatus::LegalNotImplemented);

    cfg.trace = LumenTraceSource::SDF;
    CHECK(!cfg.Validate().empty());  // 非法：连配置都不许过
    CHECK((u32)LumenClassifyCombination(cfg.trace, cfg.shade, cfg.screenTrace)
          == (u32)LumenCombinationStatus::Illegal);
}