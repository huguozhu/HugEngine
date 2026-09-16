// ============================================================
// Tests/TestGITypes.cpp — GI 数据模型与降级规则单元测试（P0 / D2）
//
// 覆盖范围（**纯 CPU、无 RHI**）：
//   1. 频段映射 GIBandOf 的完整性（含「High 频段可达」的回归断言）
//   2. 能力位 ToPipelineCap / IsRayTracingSource
//   3. 层栈 GIChannelStack 的增删改查与容量上限
//   4. GIConfig 门控谓词**严格由层栈派生**（防止再引入「影子开关」）
//   5. 四档预设 GIConfigFromPreset 的基线与精度
//   6. GIRegistry 可用性判断与逐源降级（含每通道兜底）
//   7. shader UBO 镜像结构的布局不漂移
//
// 为什么能脱离 RHI：这些类型已下沉到 `GI/GITypes.h`（只依赖 Core/Types.h），
// 故本文件只需把 `Engine/Render` 加入 include 路径，**无需链接 HugEngineRender**
// （链接它会连带拉入 RHI/Vulkan 与 slangc 生成的 SPV 头，代价过高）。
//
// 边界说明：本文件验证的是 **C++ 侧的配置 / 注册表 / 降级逻辑**，
// 不是 shader 里的合成数学（那部分由 06.GILab 的白炉测试覆盖）。
// ============================================================

#include "doctest.h"

#include "GI/GITypes.h"     // 数据模型 + GIRegistry（RHI-free）

using namespace he;
using namespace he::render;

// ============================================================
// doctest 可读化：断言失败时打印「源名 / 频段名」而非裸枚举值
//
// 注意：没有这层支持时，doctest 会尝试用 filldata<T[N]> 的数组分支去
// 字符串化 C 数组（例如与字符串字面量 "None" 比较时），那会走
// `<< in[i]`（T=char）而编译失败。此外这里也避免与字符串字面量直接比较。
// ============================================================
namespace doctest {

template <>
struct StringMaker<he::render::GISourceId> {
    static String convert(const he::render::GISourceId& id) {
        return String(he::render::GISourceName(id));
    }
};

template <>
struct StringMaker<he::render::GIBand> {
    static String convert(const he::render::GIBand& b) {
        return String(he::render::GIBandName(b));
    }
};

} // namespace doctest

// ============================================================
// 全源清单（None 之外的全部 GISourceId）——多处用例复用
// ============================================================
namespace {

constexpr GISourceId kAllSources[] = {
    GISourceId::IBL,   GISourceId::Lightmap, GISourceId::DDGI,
    GISourceId::SSGI,  GISourceId::SSR,      GISourceId::SSAO,
    GISourceId::RSM,   GISourceId::RTGI,     GISourceId::RTReflection,
    GISourceId::RTAO,  GISourceId::GTAO,
};

/// 该源所在的层栈通道（1=diffuse 2=specular 3=ao）
int ChannelOf(GISourceId id) {
    switch (id) {
    case GISourceId::SSGI:
    case GISourceId::DDGI:
    case GISourceId::RSM:
    case GISourceId::RTGI:
    case GISourceId::IBL:
    case GISourceId::Lightmap:      return 1;
    case GISourceId::SSR:
    case GISourceId::RTReflection:  return 2;
    case GISourceId::SSAO:
    case GISourceId::GTAO:
    case GISourceId::RTAO:          return 3;
    default:                        return 0;
    }
}

} // namespace

// ============================================================
// 1. 频段映射
// ============================================================
TEST_CASE("GIBandOf：低频/中频/高频映射正确") {
    // 低频：远场 / 环境
    CHECK(GIBandOf(GISourceId::IBL)      == GIBand::Low);
    CHECK(GIBandOf(GISourceId::Lightmap) == GIBand::Low);
    CHECK(GIBandOf(GISourceId::DDGI)     == GIBand::Low);

    // 中频：近处细节
    CHECK(GIBandOf(GISourceId::SSGI) == GIBand::Mid);
    CHECK(GIBandOf(GISourceId::SSR)  == GIBand::Mid);
    CHECK(GIBandOf(GISourceId::SSAO) == GIBand::Mid);
    CHECK(GIBandOf(GISourceId::RSM)  == GIBand::Mid);
    CHECK(GIBandOf(GISourceId::GTAO) == GIBand::Mid);

    // 高频：光追三源（**回归断言**）
    // 此前的实现里 RTGI/RTReflection/RTAO 与 default 共用同一条 fallthrough，
    // 导致 GIBand::High 成为不可达枚举值、面板把光追源标成「中频」。
    CHECK(GIBandOf(GISourceId::RTGI)         == GIBand::High);
    CHECK(GIBandOf(GISourceId::RTReflection) == GIBand::High);
    CHECK(GIBandOf(GISourceId::RTAO)         == GIBand::High);
}

TEST_CASE("GIBandOf：GIBand::High 确实可达（防 fallthrough 缺陷复发）") {
    bool highReachable = false;
    for (GISourceId id : kAllSources) {
        if (GIBandOf(id) == GIBand::High) { highReachable = true; break; }
    }
    CHECK(highReachable);
}

TEST_CASE("频段与源名称：每个已知源都有非空名称") {
    // 说明：断言一律先归约为 bool 再 CHECK —— 直接与字符串字面量比较会让
    // doctest 去字符串化 const char[N]（见文件顶部注释）。
    for (GISourceId id : kAllSources) {
        const char* name = GISourceName(id);
        const bool hasName   = (name != nullptr && name[0] != '\0');
        const bool notNone   = (name != nullptr && StringView(name) != StringView("None"));
        CHECK(hasName);
        CHECK(notNone);
    }

    const bool noneIsNone =
        (StringView(GISourceName(GISourceId::None)) == StringView("None"));
    CHECK(noneIsNone);

    const bool bandNamesOk =
        (StringView(GIBandName(GIBand::Low))  != StringView("?")) &&
        (StringView(GIBandName(GIBand::Mid))  != StringView("?")) &&
        (StringView(GIBandName(GIBand::High)) != StringView("?"));
    CHECK(bandNamesOk);
}

// ============================================================
// 2. 能力位
// ============================================================
TEST_CASE("ToPipelineCap：已实现源均有能力位，预留源为 None") {
    for (GISourceId id : kAllSources) {
        const u32 cap = ToPipelineCap(id);
        if (id == GISourceId::Lightmap) {
            // 预留源：尚未实现，故无能力位 → IsAvailable 恒 false（面板选不到）
            CHECK(cap == kPipelineGINone);
        } else {
            CHECK(cap != kPipelineGINone);
        }
    }
    CHECK(ToPipelineCap(GISourceId::None) == kPipelineGINone);
}

TEST_CASE("ToPipelineCap(GISourceId::IBL) 同时覆盖 diffuse 与 specular 两个通道") {
    const u32 cap = ToPipelineCap(GISourceId::IBL);
    CHECK((cap & kPipelineGIDiffIBL) != 0u);
    CHECK((cap & kPipelineGISpecIBL) != 0u);
}

TEST_CASE("ToPipelineCap(ShadowChannel)：阴影独立于层栈") {
    CHECK(ToPipelineCap(ShadowChannel::Raster) == kPipelineGIShadowRaster);
    CHECK(ToPipelineCap(ShadowChannel::RT)     == kPipelineGIShadowRT);
    CHECK(ToPipelineCap(ShadowChannel::None)   == kPipelineGINone);
}

TEST_CASE("IsRayTracingSource：仅光追三源为真") {
    for (GISourceId id : kAllSources) {
        const bool expect = (id == GISourceId::RTGI ||
                             id == GISourceId::RTReflection ||
                             id == GISourceId::RTAO);
        CHECK(IsRayTracingSource(id) == expect);
    }
    CHECK_FALSE(IsRayTracingSource(GISourceId::None));
}

// ============================================================
// 3. 层栈语义
// ============================================================
TEST_CASE("GIChannelStack：初始为空且默认归一化合成") {
    GIChannelStack st;
    CHECK(st.count == 0u);
    CHECK_FALSE(st.AnyActive());
    CHECK_FALSE(st.Has(GISourceId::IBL));
    CHECK(st.WeightOf(GISourceId::IBL) == doctest::Approx(0.0f));
    CHECK(st.FalloffOf(GISourceId::IBL) == doctest::Approx(0.0f));
    CHECK(st.mode == GIBlendMode::Normalized);
}

TEST_CASE("GIChannelStack::Set：新增 / 更新 / weight<=0 移除") {
    GIChannelStack st;

    st.Set(GISourceId::IBL, 1.0f);
    CHECK(st.count == 1u);
    CHECK(st.Has(GISourceId::IBL));
    CHECK(st.WeightOf(GISourceId::IBL) == doctest::Approx(1.0f));

    // 同一源重复 Set → 更新而非追加
    st.Set(GISourceId::IBL, 0.5f, 12.5f);
    CHECK(st.count == 1u);
    CHECK(st.WeightOf(GISourceId::IBL) == doctest::Approx(0.5f));
    CHECK(st.FalloffOf(GISourceId::IBL) == doctest::Approx(12.5f));

    // weight <= 0 → 视为移除
    st.Set(GISourceId::IBL, 0.0f);
    CHECK(st.count == 0u);
    CHECK_FALSE(st.Has(GISourceId::IBL));

    // 负权重同样视为移除，且不会写入
    st.Set(GISourceId::DDGI, -1.0f);
    CHECK(st.count == 0u);
}

TEST_CASE("GIChannelStack::Remove：后续元素前移且 count 递减") {
    GIChannelStack st;
    st.Set(GISourceId::IBL,  1.0f);
    st.Set(GISourceId::SSGI, 1.0f);
    st.Set(GISourceId::RTGI, 1.0f);
    CHECK(st.count == 3u);

    st.Remove(GISourceId::SSGI);
    CHECK(st.count == 2u);
    CHECK(st.sources[0].id == GISourceId::IBL);
    CHECK(st.sources[1].id == GISourceId::RTGI);   // 前移

    st.Remove(GISourceId::GTAO);                   // 不存在的源：无副作用
    CHECK(st.count == 2u);

    st.Clear();
    CHECK(st.count == 0u);
    CHECK_FALSE(st.AnyActive());
}

TEST_CASE("GIChannelStack::Set：容量上限 kMaxSources，超出静默忽略") {
    GIChannelStack st;
    st.Set(GISourceId::IBL,  1.0f);
    st.Set(GISourceId::DDGI, 1.0f);
    st.Set(GISourceId::SSGI, 1.0f);
    st.Set(GISourceId::RTGI, 1.0f);
    CHECK(st.count == GIChannelStack::kMaxSources);

    // 第 5 个源无处安放 → 忽略（不覆盖、不越界）
    st.Set(GISourceId::RSM, 1.0f);
    CHECK(st.count == GIChannelStack::kMaxSources);
    CHECK_FALSE(st.Has(GISourceId::RSM));

    // 但已存在的源仍可更新（不受容量限制）
    st.Set(GISourceId::IBL, 0.25f);
    CHECK(st.WeightOf(GISourceId::IBL) == doctest::Approx(0.25f));
}

TEST_CASE("GIChannelStack::Has：weight<=0 的槽位不算参与合成") {
    GIChannelStack st;
    st.Set(GISourceId::IBL, 1.0f);
    // 直接篡改权重模拟「残留但未参与」的槽位
    st.sources[0].weight = 0.0f;
    CHECK(st.count == 1u);
    CHECK_FALSE(st.Has(GISourceId::IBL));    // Has 以 weight>0 为准
    CHECK_FALSE(st.AnyActive());
}

// ============================================================
// 4. GIConfig 门控谓词 —— 关键不变量：谓词严格由层栈派生
// ============================================================
TEST_CASE("GIConfig：空层栈时所有通道谓词为假") {
    GIConfig c;
    CHECK_FALSE(c.ShouldRunSSGI());
    CHECK_FALSE(c.ShouldRunDDGI());
    CHECK_FALSE(c.ShouldRunRTGI());
    CHECK_FALSE(c.ShouldRunRSM());
    CHECK_FALSE(c.ShouldRunSSR());
    CHECK_FALSE(c.ShouldRunRTReflection());
    CHECK_FALSE(c.ShouldRunSpecular());
    CHECK_FALSE(c.ShouldRunSSAO());
    CHECK_FALSE(c.ShouldRunGTAO());
    CHECK_FALSE(c.ShouldRunRTAO());
    CHECK_FALSE(c.ShouldRunAO());
    CHECK_FALSE(c.AnyRTSource());
}

TEST_CASE("GIConfig：diffuse 通道谓词与层栈严格一致（无影子开关）") {
    struct Case { GISourceId id; bool (*pred)(const GIConfig&); };
    const Case cases[] = {
        { GISourceId::SSGI, [](const GIConfig& c) { return c.ShouldRunSSGI(); } },
        { GISourceId::DDGI, [](const GIConfig& c) { return c.ShouldRunDDGI(); } },
        { GISourceId::RTGI, [](const GIConfig& c) { return c.ShouldRunRTGI(); } },
        { GISourceId::RSM,  [](const GIConfig& c) { return c.ShouldRunRSM();  } },
    };
    for (const Case& cs : cases) {
        GIConfig c;
        CHECK_FALSE(cs.pred(c));                    // 未入栈 → 关
        c.diffuse.Set(cs.id, 1.0f);
        CHECK(cs.pred(c));                          // 入栈 → 开
        c.diffuse.Set(cs.id, 0.0f);
        CHECK_FALSE(cs.pred(c));                    // 出栈 → 关
    }
}

TEST_CASE("GIConfig：specular 与 ao 通道谓词与层栈严格一致") {
    GIConfig c;
    c.specular.Set(GISourceId::SSR, 1.0f);
    CHECK(c.ShouldRunSSR());
    CHECK(c.ShouldRunSpecular());

    c.specular.Set(GISourceId::RTReflection, 1.0f);
    CHECK(c.ShouldRunRTReflection());
    CHECK(c.ShouldRunSpecular());                   // 任一镜面源即真

    c.specular.Clear();
    CHECK_FALSE(c.ShouldRunSpecular());

    c.ao.Set(GISourceId::GTAO, 1.0f);
    CHECK(c.ShouldRunGTAO());
    CHECK(c.ShouldRunAO());
    CHECK_FALSE(c.ShouldRunSSAO());                 // GTAO 与 SSAO 是不同的源标识

    c.ao.Clear();
    c.ao.Set(GISourceId::RTAO, 1.0f);
    CHECK(c.ShouldRunRTAO());
    CHECK(c.ShouldRunAO());
}

TEST_CASE("GIConfig：RSM 额外受 rsmIndirect 总开关约束") {
    GIConfig c;
    c.diffuse.Set(GISourceId::RSM, 1.0f);

    c.rsmIndirect = true;
    CHECK(c.ShouldRunRSM());

    c.rsmIndirect = false;                          // 总开关关闭 → 即使层栈含 RSM 也不跑
    CHECK(c.diffuse.Has(GISourceId::RSM));
    CHECK_FALSE(c.ShouldRunRSM());
}

TEST_CASE("GIConfig：阴影谓词独立于层栈，由 ShadowChannel 推导") {
    GIConfig c;
    c.diffuse.Set(GISourceId::RTGI, 1.0f);          // 层栈含光追源

    c.shadow = ShadowChannel::None;
    CHECK_FALSE(c.ShouldRunShadow());
    CHECK_FALSE(c.ShouldRunRTShadow());
    CHECK(c.AnyRTSource());                         // 仍因 RTGI 而为真

    c.shadow = ShadowChannel::Raster;
    CHECK(c.ShouldRunShadow());
    CHECK_FALSE(c.ShouldRunRTShadow());

    c.shadow = ShadowChannel::RT;
    CHECK(c.ShouldRunShadow());
    CHECK(c.ShouldRunRTShadow());
}

TEST_CASE("GIConfig::AnyRTSource：四个光追触发点各自成立") {
    // 1) RTGI（diffuse）
    { GIConfig c; c.diffuse.Set(GISourceId::RTGI, 1.0f);          CHECK(c.AnyRTSource()); }
    // 2) RT 反射（specular）
    { GIConfig c; c.specular.Set(GISourceId::RTReflection, 1.0f); CHECK(c.AnyRTSource()); }
    // 3) RTAO（ao）
    { GIConfig c; c.ao.Set(GISourceId::RTAO, 1.0f);               CHECK(c.AnyRTSource()); }
    // 4) RT 阴影（独立枚举）
    { GIConfig c; c.shadow = ShadowChannel::RT;                   CHECK(c.AnyRTSource()); }
    // 非光追组合不应触发
    { GIConfig c; c.diffuse.Set(GISourceId::SSGI, 1.0f);
      c.specular.Set(GISourceId::SSR, 1.0f);
      c.ao.Set(GISourceId::SSAO, 1.0f);
      c.shadow = ShadowChannel::Raster;                           CHECK_FALSE(c.AnyRTSource()); }
}

// ============================================================
// 5. 四档预设
// ============================================================
TEST_CASE("GIConfigFromPreset：所有档位都以 IBL / SSAO / 光栅阴影为基线") {
    const GIQualityPreset presets[] = {
        GIQualityPreset::Low, GIQualityPreset::Medium,
        GIQualityPreset::High, GIQualityPreset::Ultra,
    };
    for (GIQualityPreset p : presets) {
        const GIConfig c = GIConfigFromPreset(p);
        // 低频环境兜底：diffuse 与 specular 的 IBL 必须始终在场
        CHECK(c.diffuse.Has(GISourceId::IBL));
        CHECK(c.specular.Has(GISourceId::IBL));
        // AO 基线（Ultra 会额外叠加 RTAO）
        CHECK(c.ao.Has(GISourceId::SSAO));
        // 阴影默认光栅（Ultra 改为 RT）
        if (p != GIQualityPreset::Ultra) {
            CHECK(c.shadow == ShadowChannel::Raster);
        }
    }
}

TEST_CASE("GIConfigFromPreset：Low 为半分辨率且无探针") {
    const GIConfig c = GIConfigFromPreset(GIQualityPreset::Low);
    CHECK(c.halfRes);
    CHECK(c.giIntensity == doctest::Approx(0.6f));
    CHECK(c.diffuse.Has(GISourceId::SSGI));
    CHECK_FALSE(c.diffuse.Has(GISourceId::DDGI));
    CHECK_FALSE(c.AnyRTSource());
}

TEST_CASE("GIConfigFromPreset：Medium / High 为屏幕空间 + 探针，全分辨率") {
    for (GIQualityPreset p : { GIQualityPreset::Medium, GIQualityPreset::High }) {
        const GIConfig c = GIConfigFromPreset(p);
        CHECK_FALSE(c.halfRes);
        CHECK(c.diffuse.Has(GISourceId::SSGI));
        CHECK(c.diffuse.Has(GISourceId::DDGI));
        CHECK_FALSE(c.AnyRTSource());
    }
    CHECK(GIConfigFromPreset(GIQualityPreset::Medium).giIntensity == doctest::Approx(0.8f));
    CHECK(GIConfigFromPreset(GIQualityPreset::High).giIntensity   == doctest::Approx(1.0f));
}

TEST_CASE("GIConfigFromPreset：Ultra 启用光追三源 + RT 阴影") {
    const GIConfig c = GIConfigFromPreset(GIQualityPreset::Ultra);
    CHECK(c.diffuse.Has(GISourceId::RTGI));
    CHECK(c.diffuse.Has(GISourceId::DDGI));
    CHECK(c.specular.Has(GISourceId::RTReflection));
    CHECK(c.ao.Has(GISourceId::RTAO));
    CHECK(c.shadow == ShadowChannel::RT);
    CHECK(c.AnyRTSource());
    CHECK(c.giIntensity == doctest::Approx(1.2f));
    CHECK_FALSE(c.halfRes);
}

TEST_CASE("GIConfigFromPreset：层栈不超容量（否则源会被静默丢弃）") {
    const GIQualityPreset presets[] = {
        GIQualityPreset::Low, GIQualityPreset::Medium,
        GIQualityPreset::High, GIQualityPreset::Ultra,
    };
    for (GIQualityPreset p : presets) {
        const GIConfig c = GIConfigFromPreset(p);
        CHECK(c.diffuse.count   <= GIChannelStack::kMaxSources);
        CHECK(c.specular.count  <= GIChannelStack::kMaxSources);
        CHECK(c.ao.count        <= GIChannelStack::kMaxSources);
    }
}

// ============================================================
// 6. 可用性与降级
// ============================================================
TEST_CASE("GIRegistry::IsAvailable：管线能力 ∧ 设备能力") {
    // 管线不提供 → 不可用（Forward 无 GBuffer，故无屏幕空间源）
    CHECK_FALSE(GIRegistry::IsAvailable(GISourceId::SSGI, PipelineCaps::Forward, true));
    CHECK_FALSE(GIRegistry::IsAvailable(GISourceId::DDGI, PipelineCaps::Forward, true));
    CHECK_FALSE(GIRegistry::IsAvailable(GISourceId::SSAO, PipelineCaps::Forward, true));
    // Forward 提供的源
    CHECK(GIRegistry::IsAvailable(GISourceId::IBL, PipelineCaps::Forward, true));
    CHECK(GIRegistry::IsAvailable(GISourceId::RSM, PipelineCaps::Forward, true));

    // Deferred 提供屏幕空间源与探针
    CHECK(GIRegistry::IsAvailable(GISourceId::SSGI, PipelineCaps::Deferred, false));
    CHECK(GIRegistry::IsAvailable(GISourceId::DDGI, PipelineCaps::Deferred, false));

    // 光追源还需设备能力
    CHECK_FALSE(GIRegistry::IsAvailable(GISourceId::RTGI, PipelineCaps::Deferred, false));
    CHECK_FALSE(GIRegistry::IsAvailable(GISourceId::RTReflection, PipelineCaps::Deferred, false));
    CHECK_FALSE(GIRegistry::IsAvailable(GISourceId::RTAO, PipelineCaps::Deferred, false));
    CHECK(GIRegistry::IsAvailable(GISourceId::RTGI, PipelineCaps::Deferred, true));

    // None 与预留源恒不可用
    CHECK_FALSE(GIRegistry::IsAvailable(GISourceId::None, PipelineCaps::Deferred, true));
    CHECK_FALSE(GIRegistry::IsAvailable(GISourceId::Lightmap, PipelineCaps::Deferred, true));
}

TEST_CASE("GIRegistry::IsAvailable(ShadowChannel)：RT 阴影需设备光追") {
    CHECK(GIRegistry::IsAvailable(ShadowChannel::None, PipelineCaps::Forward, false));
    CHECK(GIRegistry::IsAvailable(ShadowChannel::Raster, PipelineCaps::Forward, false));
    CHECK_FALSE(GIRegistry::IsAvailable(ShadowChannel::RT, PipelineCaps::Deferred, false));
    CHECK(GIRegistry::IsAvailable(ShadowChannel::RT, PipelineCaps::Deferred, true));
}

TEST_CASE("GIRegistry::Degrade：Ultra + Deferred 无光追 → 裁掉光追源并回退阴影") {
    const GIConfig ultra = GIConfigFromPreset(GIQualityPreset::Ultra);
    const GIConfig d = GIRegistry::Degrade(ultra, PipelineCaps::Deferred, /*rtSupported=*/false);

    CHECK_FALSE(d.diffuse.Has(GISourceId::RTGI));
    CHECK_FALSE(d.specular.Has(GISourceId::RTReflection));
    CHECK_FALSE(d.ao.Has(GISourceId::RTAO));

    CHECK(d.shadow == ShadowChannel::Raster);        // RT 阴影 → 光栅阴影
    // 非光追源保留
    CHECK(d.diffuse.Has(GISourceId::DDGI));
    CHECK(d.ao.Has(GISourceId::SSAO));
    CHECK(d.specular.Has(GISourceId::IBL));
}

TEST_CASE("GIRegistry::Degrade：Ultra + Forward 无光追 → 只剩管线支持的源") {
    const GIConfig ultra = GIConfigFromPreset(GIQualityPreset::Ultra);
    const GIConfig d = GIRegistry::Degrade(ultra, PipelineCaps::Forward, false);

    // Forward 只提供 IBL 与 RSM（且 RS 源需场景，Ultra 未含）→ diffuse 只剩 IBL
    CHECK(d.diffuse.Has(GISourceId::IBL));
    CHECK_FALSE(d.diffuse.Has(GISourceId::RTGI));
    CHECK_FALSE(d.diffuse.Has(GISourceId::DDGI));
    CHECK(d.specular.Has(GISourceId::IBL));
    CHECK_FALSE(d.specular.Has(GISourceId::RTReflection));

    // Forward 无 GBuffer → 没有任何可用 AO 源，故 ao 通道为空是**正确**结果
    CHECK_FALSE(GIRegistry::IsAvailable(GISourceId::SSAO, PipelineCaps::Forward, false));
    CHECK(d.ao.count == 0u);
    CHECK_FALSE(d.ShouldRunAO());

    CHECK(d.shadow == ShadowChannel::Raster);
}

TEST_CASE("GIRegistry::Degrade：weight<=0 的残留槽位也会被清理") {
    GIConfig c;
    c.diffuse.Set(GISourceId::IBL, 1.0f);
    c.diffuse.sources[c.diffuse.count].id = GISourceId::SSGI;   // 手工塞入一个 0 权重槽
    c.diffuse.sources[c.diffuse.count].weight = 0.0f;
    c.diffuse.count++;

    const GIConfig d = GIRegistry::Degrade(c, PipelineCaps::Deferred, false);
    CHECK(d.diffuse.count == 1u);
    CHECK(d.diffuse.sources[0].id == GISourceId::IBL);
}

TEST_CASE("GIRegistry::Degrade：通道被裁空时兜底（IBL / SSAO）") {
    // 构造一个只含不可用源的配置：diffuse/specular 含 Lightmap（预留源，恒不可用），
    // ao 含 RTAO（无光追设备不可用）
    GIConfig c;
    c.diffuse.Set(GISourceId::Lightmap, 1.0f);
    c.specular.Set(GISourceId::Lightmap, 1.0f);
    c.ao.Set(GISourceId::RTAO, 1.0f);

    const GIConfig d = GIRegistry::Degrade(c, PipelineCaps::Deferred, false);

    CHECK(d.diffuse.Has(GISourceId::IBL));           // 兜底 IBL
    CHECK_FALSE(d.diffuse.Has(GISourceId::Lightmap));
    CHECK(d.specular.Has(GISourceId::IBL));
    CHECK(d.ao.Has(GISourceId::SSAO));               // 兜底 SSAO
    CHECK_FALSE(d.ao.Has(GISourceId::RTAO));
}

TEST_CASE("GIRegistry::Degrade：结果自洽 —— 无不可用源残留、无权重<=0 槽位") {
    // 属性测试：管线 × 设备能力 × 四档预设 全组合
    const u32 caps[] = { PipelineCaps::Forward, PipelineCaps::Deferred };
    const bool rt[] = { false, true };
    const GIQualityPreset presets[] = {
        GIQualityPreset::Low, GIQualityPreset::Medium,
        GIQualityPreset::High, GIQualityPreset::Ultra,
    };

    for (u32 cap : caps) {
        for (bool rtSupported : rt) {
            for (GIQualityPreset p : presets) {
                const GIConfig d =
                    GIRegistry::Degrade(GIConfigFromPreset(p), cap, rtSupported);

                const GIChannelStack* stacks[] = { &d.diffuse, &d.specular, &d.ao };
                for (const GIChannelStack* st : stacks) {
                    for (u32 i = 0; i < st->count; i++) {
                        // 1) 不再含任何不可用源
                        CHECK(GIRegistry::IsAvailable(st->sources[i].id, cap, rtSupported));
                        // 2) 不再含 weight<=0 的僵尸槽位
                        CHECK(st->sources[i].weight > 0.0f);
                    }
                    // 3) 只要该通道存在可用源，降级后就必须非空（兜底生效）
                    bool anyAvailable = false;
                    for (GISourceId id : kAllSources) {
                        if (ChannelOf(id) == 0) continue;
                        const bool inChannel =
                            (st == &d.diffuse   && ChannelOf(id) == 1) ||
                            (st == &d.specular  && ChannelOf(id) == 2) ||
                            (st == &d.ao        && ChannelOf(id) == 3);
                        if (inChannel && GIRegistry::IsAvailable(id, cap, rtSupported)) {
                            anyAvailable = true; break;
                        }
                    }
                    if (anyAvailable) CHECK(st->count > 0u);
                }

                // 4) 阴影通道要么可用，要么被降为 None
                if (d.shadow != ShadowChannel::None) {
                    CHECK(GIRegistry::IsAvailable(d.shadow, cap, rtSupported));
                }
            }
        }
    }
}

TEST_CASE("GIRegistry::Degrade：不改动入参（纯函数语义）") {
    const GIConfig src = GIConfigFromPreset(GIQualityPreset::Ultra);
    const GIConfig before = src;

    (void)GIRegistry::Degrade(src, PipelineCaps::Forward, false);

    CHECK(src.diffuse.count   == before.diffuse.count);
    CHECK(src.specular.count  == before.specular.count);
    CHECK(src.ao.count        == before.ao.count);
    CHECK(src.shadow          == before.shadow);
    CHECK(src.diffuse.Has(GISourceId::RTGI));        // 入参仍保有光追源
}

// ============================================================
// 7. shader UBO 镜像结构（防 C++/slang 双侧漂移）
// ============================================================
TEST_CASE("shader UBO 镜像：结构与容量与 ShaderTypes.slang 一致") {
    // 布局：GISourceSlot = {u32,float,float,u32} = 16B
    CHECK(sizeof(GISourceSlotData) == 16u);
    // GIChannelBlendParams = 4×16B 源槽 + count/mode/furnaceMode/_pad = 80B
    CHECK(sizeof(GIChannelBlendData) == 80u);

    // 容量常量必须与 shader 的 kGIMaxSourcesPerChannel = 4 一致
    CHECK(GIChannelStack::kMaxSources      == 4u);
    CHECK(GIChannelBlendData::kMaxSources  == 4u);
    CHECK(GIChannelStack::kMaxSources == GIChannelBlendData::kMaxSources);
}

TEST_CASE("GIChannelBlendData::Add：weight<=0 忽略、容量上限生效") {
    GIChannelBlendData b;
    CHECK(b.count == 0u);

    b.Add(static_cast<u32>(GISourceId::IBL), 1.0f);
    b.Add(static_cast<u32>(GISourceId::DDGI), 0.5f, 20.0f);
    CHECK(b.count == 2u);
    CHECK(b.mode == 1u);                                     // 默认归一化
    CHECK(b.sources[1].falloffDistance == doctest::Approx(20.0f));

    b.Add(static_cast<u32>(GISourceId::SSGI), 0.0f);         // weight<=0 → 忽略
    CHECK(b.count == 2u);

    b.Add(static_cast<u32>(GISourceId::SSGI), 1.0f);
    b.Add(static_cast<u32>(GISourceId::RSM),  1.0f);
    CHECK(b.count == GIChannelBlendData::kMaxSources);

    b.Add(static_cast<u32>(GISourceId::GTAO), 1.0f);         // 超容量 → 忽略
    CHECK(b.count == GIChannelBlendData::kMaxSources);
}
