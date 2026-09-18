// ============================================================
// Tests/TestGITypes.cpp — GI 数据模型与降级规则单元测试（P0 / D2）
//
// 覆盖范围（**纯 CPU、无 RHI**）：
//   1. 源分类谓词（世界空间 / 屏幕空间 / 光追）的互斥性与完备性
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
// doctest 可读化：断言失败时打印「源名」而非裸枚举值
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
// 1. 源分类（谓词，而非「频段」枚举）
//
// 历史：这里原有一个 `GIBand { Low, Mid, High }`（显示为「低频/中频/高频」），
// 但它的两条分界线管的是**两个正交维度**——Low↔Mid 是【尺度】（世界空间大范围
// vs 屏幕空间像素级），Mid↔High 是【精度】（屏幕空间近似 vs 光追精确）。
// 反证：RTGI 输出 1/4 分辨率，空间分辨率低于全分辨率 SSGI，却被标成「高频」。
// 结果是 `SSGI=Mid` 而 `RTGI=High`——同一物理量的两面被标成不同"频段"。
// 故该枚举已删除，改由三个**互斥且完备**的谓词表达。
// ============================================================
TEST_CASE("源分类谓词：三个类别互斥且完备（对 11 个源构成划分）") {
    u32 nWorld = 0, nScreen = 0, nRT = 0;
    for (GISourceId id : kAllSources) {
        const int hits = (IsWorldSpaceSource(id)  ? 1 : 0)
                       + (IsScreenSpaceSource(id) ? 1 : 0)
                       + (IsRayTracingSource(id)  ? 1 : 0);
        CHECK(hits == 1);                    // 恰好属于一类
        if (IsWorldSpaceSource(id))  nWorld++;
        if (IsScreenSpaceSource(id)) nScreen++;
        if (IsRayTracingSource(id))  nRT++;
    }
    CHECK(nWorld  == 3u);                    // IBL / Lightmap / DDGI
    CHECK(nScreen == 5u);                    // SSGI / SSR / SSAO / RSM / GTAO
    CHECK(nRT     == 3u);                    // RTGI / RTReflection / RTAO
    CHECK(nWorld + nScreen + nRT == 11u);    // 覆盖全部（无遗漏、无重叠）
}

TEST_CASE("源分类谓词：各类别成员正确，None 不属于任何类别") {
    // 世界空间 / 预计算环境
    CHECK(IsWorldSpaceSource(GISourceId::IBL));
    CHECK(IsWorldSpaceSource(GISourceId::Lightmap));
    CHECK(IsWorldSpaceSource(GISourceId::DDGI));
    // 屏幕空间 / 单次反弹光栅
    CHECK(IsScreenSpaceSource(GISourceId::SSGI));
    CHECK(IsScreenSpaceSource(GISourceId::SSR));
    CHECK(IsScreenSpaceSource(GISourceId::SSAO));
    CHECK(IsScreenSpaceSource(GISourceId::RSM));
    CHECK(IsScreenSpaceSource(GISourceId::GTAO));
    // 硬件光追
    CHECK(IsRayTracingSource(GISourceId::RTGI));
    CHECK(IsRayTracingSource(GISourceId::RTReflection));
    CHECK(IsRayTracingSource(GISourceId::RTAO));

    CHECK_FALSE(IsWorldSpaceSource(GISourceId::None));
    CHECK_FALSE(IsScreenSpaceSource(GISourceId::None));
    CHECK_FALSE(IsRayTracingSource(GISourceId::None));

    // 交叉不成立（类别互斥）
    CHECK_FALSE(IsScreenSpaceSource(GISourceId::IBL));
    CHECK_FALSE(IsWorldSpaceSource(GISourceId::SSGI));
    CHECK_FALSE(IsScreenSpaceSource(GISourceId::RTGI));
    CHECK_FALSE(IsRayTracingSource(GISourceId::GTAO));
}

TEST_CASE("源分类：SS 源与其光追对应源属不同类别（删除 GIBand 的核心动因）") {
    // 二者估**同一个物理量**（间接漫反射），却不在同一类别——
    // 说明原枚举表达的是「估计器类别」而非「空间频段」。
    CHECK(IsScreenSpaceSource(GISourceId::SSGI));
    CHECK(IsRayTracingSource(GISourceId::RTGI));
    CHECK_FALSE(IsScreenSpaceSource(GISourceId::RTGI));
    CHECK_FALSE(IsRayTracingSource(GISourceId::SSGI));
    CHECK(GIRegistry::RTCounterpartOf(GISourceId::SSGI) == GISourceId::RTGI);
}

TEST_CASE("源名称与类别名称：每个已知源都有非空名称") {
    // 说明：断言一律先归约为 bool 再 CHECK —— 直接与字符串字面量比较会让
    // doctest 去字符串化 const char[N]（见文件顶部注释）。
    for (GISourceId id : kAllSources) {
        const char* name = GISourceName(id);
        const char* cls  = GISourceClassName(id);
        const bool hasName  = (name != nullptr && name[0] != '\0');
        const bool notNone  = (name != nullptr && StringView(name) != StringView("None"));
        const bool hasClass = (cls != nullptr && cls[0] != '\0'
                               && StringView(cls) != StringView("?"));
        CHECK(hasName);
        CHECK(notNone);
        CHECK(hasClass);
    }

    const bool noneIsNone =
        (StringView(GISourceName(GISourceId::None)) == StringView("None"));
    CHECK(noneIsNone);
    // None 无类别（返回 "?"）
    const bool noneHasNoClass =
        (StringView(GISourceClassName(GISourceId::None)) == StringView("?"));
    CHECK(noneHasNoClass);
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
    // 管线不提供 → 不可用。前向着色没有 GBuffer，故屏幕空间源、探针与光追源都不可用
    CHECK_FALSE(GIRegistry::IsAvailable(GISourceId::SSGI, PipelineCaps::Forward, true));
    CHECK_FALSE(GIRegistry::IsAvailable(GISourceId::DDGI, PipelineCaps::Forward, true));
    CHECK_FALSE(GIRegistry::IsAvailable(GISourceId::SSAO, PipelineCaps::Forward, true));
    CHECK_FALSE(GIRegistry::IsAvailable(GISourceId::SSR, PipelineCaps::Forward, true));
    CHECK_FALSE(GIRegistry::IsAvailable(GISourceId::RTGI, PipelineCaps::Forward, true));
    // **世界空间源在 Forward 可用**（任务 26）：PBR.frag 现在逐通道遍历层栈的源数组做
    // 归一化合成，ForwardPipeline 每帧填同一份 GIBlendParams UBO。任务 8 当时声明它们
    // 是"配置说谎"（没有消费者），任务 26 补上了消费者 ⇒ 这三条从"必须为假"变成"必须为真"。
    // 这条断言是这两个任务之间的**分界线**：谁把 Forward 的 IBL/RSM 消费者删掉，它就红。
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

    // None 与 **刻意未实现**的 Lightmap 恒不可用（任务 18：把「文档说可用、实际不可用」
    // 这处配置说谎改成明确的声明；原因见 ToPipelineCap 的注释与文档 §10.2 任务 18
    //（承接它的任务 31 已取消，但"未实现的源不给能力位"这条声明与下面的断言继续有效）
    CHECK_FALSE(GIRegistry::IsAvailable(GISourceId::None, PipelineCaps::Deferred, true));
    CHECK_FALSE(GIRegistry::IsAvailable(GISourceId::Lightmap, PipelineCaps::Deferred, true));
}

TEST_CASE("GIRegistry::Degrade：层栈里的未实现源（Lightmap）会被裁掉，不会留下空承诺") {
    // 这是 Lightmap「不给能力位」这条决定的**安全性质**：本仓库最怕的失效形态是
    // 「源在归一化里计权重、却没有任何 pass 产出它」（§9.2-G/L）—— 单测把这个性质锁住：
    // 无论谁把 Lightmap 塞进层栈（预设、配置文件、面板），Degrade 都必须把它摘掉，
    // 而同通道里真正可用的源一个都不能少。
    GIConfig c;
    c.diffuse.Set(GISourceId::Lightmap, 1.0f);
    c.diffuse.Set(GISourceId::IBL, 1.0f);
    c.specular.Set(GISourceId::IBL, 1.0f);
    REQUIRE(c.diffuse.Has(GISourceId::Lightmap));

    const GIConfig d = GIRegistry::Degrade(c, PipelineCaps::Deferred, /*rtSupported=*/true);
    CHECK_FALSE(d.diffuse.Has(GISourceId::Lightmap));   // 摘掉
    CHECK(d.diffuse.Has(GISourceId::IBL));              // 同通道可用源不受影响
    CHECK(d.diffuse.count == 1u);
    CHECK(d.specular.Has(GISourceId::IBL));
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

TEST_CASE("GIRegistry::Degrade：Ultra + Forward 无光追 → 只留世界空间源与光栅阴影") {
    const GIConfig ultra = GIConfigFromPreset(GIQualityPreset::Ultra);
    const GIConfig d = GIRegistry::Degrade(ultra, PipelineCaps::Forward, false);

    // 任务 26 起 Forward 声明并**消费**三个世界空间源位（漫反射 IBL/RSM、镜面 IBL）：
    // Ultra 预设的 diffuse = {IBL, RTGI, DDGI}、specular = {IBL, RTReflection}、ao = {SSAO, RTAO}
    // ⇒ 降级后只剩 IBL（漫反射与镜面各一份），屏幕空间/探针/光追源全部被裁掉。
    CHECK(d.diffuse.count == 1u);
    CHECK(d.diffuse.Has(GISourceId::IBL));
    CHECK(d.specular.count == 1u);
    CHECK(d.specular.Has(GISourceId::IBL));
    CHECK(d.ao.count == 0u);            // Forward 没有 AO 源（无 GBuffer / 无屏幕空间）

    CHECK_FALSE(d.ShouldRunSSGI());
    CHECK_FALSE(d.ShouldRunDDGI());
    CHECK_FALSE(d.ShouldRunRSM());      // Ultra 的 diffuse 里本来就没有 RSM
    CHECK_FALSE(d.ShouldRunAO());
    // 注：`ShouldRunSpecular()` 只覆盖 SSR/RTReflection（屏幕空间/光追的特例谓词），
    // 不含 IBL —— 所以"镜面通道还在"要看层栈本身，而不是这个谓词。
    CHECK(d.specular.Has(GISourceId::IBL));

    // 阴影通道独立于层栈，Forward 的光栅阴影保留
    CHECK(d.shadow == ShadowChannel::Raster);
}

TEST_CASE("GIRegistry::Degrade：Forward 的漫反射层栈可以同时留 IBL 与 RSM") {
    // 任务 26 的验收前提：`pipeline_mode=0` 下把 diffuse 从 {IBL} 改成 {IBL,RSM} 必须**真的**
    // 走到着色器里（两者都在 Forward 的能力位内），而不是被 Degrade 悄悄裁掉。
    GIConfig c;
    c.diffuse.Set(GISourceId::IBL, 1.0f);
    c.diffuse.Set(GISourceId::RSM, 1.0f);
    c.specular.Set(GISourceId::IBL, 1.0f);

    const GIConfig d = GIRegistry::Degrade(c, PipelineCaps::Forward, /*rtSupported=*/false);
    CHECK(d.diffuse.count == 2u);
    CHECK(d.diffuse.Has(GISourceId::IBL));
    CHECK(d.diffuse.Has(GISourceId::RSM));
    CHECK(d.ShouldRunRSM());
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

TEST_CASE("GIRegistry::Degrade：只裁不加 —— 空通道保持空（同一份配置在任何路径上同义）") {
    // 任务 28 / §9.2-Y 的核心性质：`Degrade` 只移除不可用源，**不补源**。
    // 这条性质保证「同一份配置 → 同一个有效层栈」与调用路径无关：
    // 配置加载路径不经过 Degrade、面板的预设/阴影下拉经过它，两边必须同义。
    // 反例（改前的兜底行为）：全 0 权重的配置在加载路径上是空栈（做差实验的基线），
    // 一旦经过 Degrade 就被补成 {IBL}，而示例退出时又把内存那份回写成文件 ⇒
    // 下一次运行读到的是被补过的配置，读数差 32%。
    GIConfig empty;                                    // 三通道全空
    const GIConfig d = GIRegistry::Degrade(empty, PipelineCaps::Deferred, true);
    CHECK(d.diffuse.count  == 0u);
    CHECK(d.specular.count == 0u);
    CHECK(d.ao.count       == 0u);

    // 只含不可用源的通道：裁完之后同样保持空，而不是被补上 IBL/SSAO
    GIConfig c;
    c.diffuse.Set(GISourceId::Lightmap, 1.0f);         // 恒不可用
    c.specular.Set(GISourceId::Lightmap, 1.0f);
    c.ao.Set(GISourceId::RTAO, 1.0f);                  // 无光追设备不可用
    const GIConfig d2 = GIRegistry::Degrade(c, PipelineCaps::Deferred, false);
    CHECK(d2.diffuse.count  == 0u);
    CHECK(d2.specular.count == 0u);
    CHECK(d2.ao.count       == 0u);

    // 幂等：再裁一次结果不变（这正是"任何路径都同义"的可操作表述）
    const GIConfig d3 = GIRegistry::Degrade(d2, PipelineCaps::Deferred, false);
    CHECK(d3.diffuse.count  == 0u);
    CHECK(d3.specular.count == 0u);
    CHECK(d3.ao.count       == 0u);
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
                    // 3) 只要该通道存在可用源，降级后就必须非空
                    //（注意：这不依赖任何"兜底"——四档预设的每个通道本来就带有可用源，
                    //  `Degrade` 只是把不可用的裁掉；任务 28 起它不再补源）
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

// ── 逐像素置信度判据（§3.2，任务 9）──
// 判据掩码由 C++ 逐槽写进 UBO，着色器按位计算 —— 两侧不再各有一份 id 列表。
TEST_CASE("ToConfidenceMask：屏幕空间源带 CAMERA_COVERAGE 位，DDGI 带 PROBE_GRID 位") {
    // 屏幕空间 + 光追：入射方向从本像素出发，视口外/边缘无数据 ⇒ 受屏幕覆盖限制
    const GISourceId limited[] = {
        GISourceId::SSGI, GISourceId::SSR, GISourceId::SSAO, GISourceId::GTAO,
        GISourceId::RTGI, GISourceId::RTReflection, GISourceId::RTAO,
    };
    for (GISourceId id : limited) {
        CHECK(IsCameraViewLimitedSource(id));
        CHECK(ToConfidenceMask(id) == kGIConfCameraCoverage);
    }
    // 世界空间源不受相机屏幕覆盖限制
    const GISourceId world[] = { GISourceId::IBL, GISourceId::Lightmap };
    for (GISourceId id : world) {
        CHECK_FALSE(IsCameraViewLimitedSource(id));
        CHECK(ToConfidenceMask(id) == kGIConfNone);
    }
    // DDGI 也是世界空间源，但它另有一条**探针网格覆盖**判据（任务 14 / §9.2-K）：
    // 网格 AABB 之外 SampleDDGI 只能贴边常数外推 ⇒ 该处必须判为不可信。
    CHECK_FALSE(IsCameraViewLimitedSource(GISourceId::DDGI));
    CHECK(ToConfidenceMask(GISourceId::DDGI) == kGIConfProbeGrid);
    // 两条判据是不同的位，可独立开关（将来若有既受屏幕限制、又受网格限制的源，按位取或）
    CHECK((kGIConfCameraCoverage & kGIConfProbeGrid) == 0u);
    // RSM 是**故意**不在这个谓词里的：它的产物是光源视锥下的 VPL 图，着色器按世界空间
    // 求和，与相机视口无关。它与 IsScreenSpaceSource 的分类不同，两个谓词不可互相替代。
    CHECK_FALSE(IsCameraViewLimitedSource(GISourceId::RSM));
    CHECK(IsScreenSpaceSource(GISourceId::RSM));
    CHECK(ToConfidenceMask(GISourceId::RSM) == kGIConfNone);
    CHECK(ToConfidenceMask(GISourceId::None) == kGIConfNone);
}

TEST_CASE("GIChannelBlendData::Add：置信度掩码由源 id 统一推导") {
    GIChannelBlendData b;
    b.Add(static_cast<u32>(GISourceId::IBL),  1.0f);
    b.Add(static_cast<u32>(GISourceId::SSGI), 1.0f);
    b.Add(static_cast<u32>(GISourceId::RSM),  1.0f);
    b.Add(static_cast<u32>(GISourceId::DDGI), 1.0f);
    CHECK(b.count == 4u);
    // 调用方不填置信度，Add 自己推导 —— 少一个"忘记填"的机会
    CHECK(b.sources[0].confidence == kGIConfNone);
    CHECK(b.sources[1].confidence == kGIConfCameraCoverage);
    CHECK(b.sources[2].confidence == kGIConfNone);
    CHECK(b.sources[3].confidence == kGIConfProbeGrid);
    // 边缘淡出带宽有非零默认值（UBO 里不再是硬编码 5%）
    CHECK(b.edgeFade > 0.0f);
}

// ============================================================
// 8. 配置诊断与冗余去重（P0 · REDUNDANCY）
//
// 动机：本架构要求每个已启用的源都整幅、每帧产出完整通道缓冲，
// 故「启用一个没有增益的源」= 白付一份全量成本。层栈当前允许这类配置且无提示。
// ============================================================

namespace {

/// 统计某类诊断出现的次数
u32 CountKind(const std::vector<GIDiagnostic>& d, GIDiagnosticKind k) {
    u32 n = 0;
    for (const GIDiagnostic& e : d) if (e.kind == k) n++;
    return n;
}

/// 某类诊断是否针对指定源对（无序）
bool HasDiag(const std::vector<GIDiagnostic>& d, GIDiagnosticKind k,
             GISourceId x, GISourceId y) {
    for (const GIDiagnostic& e : d) {
        if (e.kind != k) continue;
        if ((e.a == x && e.b == y) || (e.a == y && e.b == x)) return true;
    }
    return false;
}

/// 模拟 shader 的**归一化加权**合成（用于证明去重的逐像素等价性）。
///
/// 关键前提：AO 通道的合成分支对 SSAO 与 GTAO **都采样同一张 u_SSAO 纹理**，
/// 故两者取到的「源值」是同一个 v。
float NormalizedBlend(const GIChannelStack& st, float sourceValue) {
    float num = 0.0f, den = 0.0f;
    for (u32 i = 0; i < st.count; i++) {
        const float w = st.sources[i].weight;
        if (w <= 0.0f) continue;
        num += sourceValue * w;
        den += w;
    }
    return (den > 0.0f) ? (num / den) : 1.0f;   // 无源 → 不遮蔽
}

} // namespace

TEST_CASE("GIRegistry::IsStrictlyRedundant：只对共用输出纹理的源对为真") {
    CHECK(GIRegistry::IsStrictlyRedundant(GISourceId::SSAO, GISourceId::GTAO));
    CHECK(GIRegistry::IsStrictlyRedundant(GISourceId::GTAO, GISourceId::SSAO));   // 对称

    // 其余任意组合都不算严格冗余（它们各有独立 pass / 纹理）
    for (GISourceId x : kAllSources) {
        for (GISourceId y : kAllSources) {
            const bool expect = (x == GISourceId::SSAO && y == GISourceId::GTAO) ||
                                (x == GISourceId::GTAO && y == GISourceId::SSAO);
            CHECK(GIRegistry::IsStrictlyRedundant(x, y) == expect);
        }
    }
    CHECK_FALSE(GIRegistry::IsStrictlyRedundant(GISourceId::SSAO, GISourceId::SSAO));
    CHECK_FALSE(GIRegistry::IsStrictlyRedundant(GISourceId::None, GISourceId::None));
}

TEST_CASE("GIRegistry::RTCounterpartOf：屏幕空间源 → 同一物理量的光追源") {
    CHECK(GIRegistry::RTCounterpartOf(GISourceId::SSGI) == GISourceId::RTGI);
    CHECK(GIRegistry::RTCounterpartOf(GISourceId::SSR)  == GISourceId::RTReflection);
    CHECK(GIRegistry::RTCounterpartOf(GISourceId::SSAO) == GISourceId::RTAO);
    CHECK(GIRegistry::RTCounterpartOf(GISourceId::GTAO) == GISourceId::RTAO);

    // 无对应关系者 → None
    CHECK(GIRegistry::RTCounterpartOf(GISourceId::IBL)  == GISourceId::None);
    CHECK(GIRegistry::RTCounterpartOf(GISourceId::DDGI) == GISourceId::None);
    CHECK(GIRegistry::RTCounterpartOf(GISourceId::RSM)  == GISourceId::None);
    CHECK(GIRegistry::RTCounterpartOf(GISourceId::RTGI) == GISourceId::None);
    CHECK(GIRegistry::RTCounterpartOf(GISourceId::None) == GISourceId::None);

    // 二者属不同**估计器类别**（前者屏幕空间、后者光追），但估同一物理量——
    // 这正是「重复估计」诊断的依据（原 GIBand 把它们标成不同"频段"，语义是错的）
    CHECK(IsScreenSpaceSource(GISourceId::SSGI));
    CHECK(IsRayTracingSource(GISourceId::RTGI));
}

TEST_CASE("GIRegistry::IsEffectiveSource：严格冗余组内 GTAO 优先") {
    GIChannelStack st;
    st.Set(GISourceId::SSAO, 1.0f);
    CHECK(GIRegistry::IsEffectiveSource(st, GISourceId::SSAO));    // 只有 SSAO → 它生效

    st.Set(GISourceId::GTAO, 1.0f);
    CHECK_FALSE(GIRegistry::IsEffectiveSource(st, GISourceId::SSAO)); // 两者在场 → SSAO 让位
    CHECK(GIRegistry::IsEffectiveSource(st, GISourceId::GTAO));

    CHECK(GIRegistry::IsEffectiveSource(st, GISourceId::IBL));     // 无关源恒为生效
}

TEST_CASE("GIRegistry::Analyze：空配置不产生任何诊断") {
    const std::vector<GIDiagnostic> d = GIRegistry::Analyze(GIConfig{});
    CHECK(d.empty());
}

TEST_CASE("GIRegistry::Analyze：SSAO + GTAO → 判为严格冗余") {
    GIConfig c;
    c.ao.Set(GISourceId::SSAO, 1.0f);
    c.ao.Set(GISourceId::GTAO, 1.0f);

    const std::vector<GIDiagnostic> d = GIRegistry::Analyze(c);
    CHECK(CountKind(d, GIDiagnosticKind::RedundantDuplicate) == 1u);
    CHECK(HasDiag(d, GIDiagnosticKind::RedundantDuplicate,
                  GISourceId::SSAO, GISourceId::GTAO));
    CHECK(d[0].channel == GIChannelId::AO);
}

TEST_CASE("GIRegistry::Analyze：SSAO + GTAO + RTAO → 重复估计只报一条") {
    GIConfig c;
    c.ao.Set(GISourceId::SSAO, 1.0f);
    c.ao.Set(GISourceId::GTAO, 1.0f);
    c.ao.Set(GISourceId::RTAO, 1.0f);

    const std::vector<GIDiagnostic> d = GIRegistry::Analyze(c);
    // 严格冗余一条（SSAO↔GTAO）
    CHECK(CountKind(d, GIDiagnosticKind::RedundantDuplicate) == 1u);
    // 重复估计只报「实际生效」的那个（GTAO↔RTAO），不因 SSAO 也对应 RTAO 而报两条
    CHECK(CountKind(d, GIDiagnosticKind::DuplicateEstimate) == 1u);
    CHECK(HasDiag(d, GIDiagnosticKind::DuplicateEstimate,
                  GISourceId::GTAO, GISourceId::RTAO));
}

TEST_CASE("GIRegistry::Analyze：SS 源与其光追对应源 → 判为重复估计") {
    struct Case { GISourceId ss; GISourceId rt; GIChannelId ch; };
    const Case cases[] = {
        { GISourceId::SSGI, GISourceId::RTGI,         GIChannelId::Diffuse  },
        { GISourceId::SSR,  GISourceId::RTReflection, GIChannelId::Specular },
        { GISourceId::SSAO, GISourceId::RTAO,         GIChannelId::AO       },
    };
    for (const Case& cs : cases) {
        GIConfig c;
        GIChannelStack* st = (cs.ch == GIChannelId::Diffuse)  ? &c.diffuse
                           : (cs.ch == GIChannelId::Specular) ? &c.specular
                                                              : &c.ao;
        st->Set(cs.ss, 1.0f);
        st->Set(cs.rt, 1.0f);

        const std::vector<GIDiagnostic> d = GIRegistry::Analyze(c);
        CHECK(HasDiag(d, GIDiagnosticKind::DuplicateEstimate, cs.ss, cs.rt));
    }
}

TEST_CASE("GIRegistry::Analyze：RTGI + DDGI → 判为相关估计（miss 回退）") {
    GIConfig c;
    c.diffuse.Set(GISourceId::RTGI, 1.0f);
    c.diffuse.Set(GISourceId::DDGI, 1.0f);

    const std::vector<GIDiagnostic> d = GIRegistry::Analyze(c);
    CHECK(HasDiag(d, GIDiagnosticKind::CorrelatedEstimates,
                  GISourceId::RTGI, GISourceId::DDGI));
    // 二者并非 SS↔RT 对应关系，故不应被误判为重复估计
    CHECK(CountKind(d, GIDiagnosticKind::DuplicateEstimate) == 0u);
}

TEST_CASE("GIRegistry::Analyze：多源通道产生成本提示") {
    GIConfig c;
    c.diffuse.Set(GISourceId::IBL, 1.0f);
    c.diffuse.Set(GISourceId::DDGI, 1.0f);
    c.diffuse.Set(GISourceId::SSGI, 1.0f);
    c.ao.Set(GISourceId::SSAO, 1.0f);      // 单源通道

    const std::vector<GIDiagnostic> d = GIRegistry::Analyze(c);
    // 只有 diffuse（3 源）与…… ao 是单源 → 不报
    CHECK(CountKind(d, GIDiagnosticKind::MultiSourceCost) == 1u);
    for (const GIDiagnostic& e : d) {
        if (e.kind == GIDiagnosticKind::MultiSourceCost) {
            CHECK(e.channel == GIChannelId::Diffuse);
        }
    }
}

TEST_CASE("GIRegistry::Analyze：四档预设都不含严格冗余（预设本身是干净的）") {
    const GIQualityPreset presets[] = {
        GIQualityPreset::Low, GIQualityPreset::Medium,
        GIQualityPreset::High, GIQualityPreset::Ultra,
    };
    for (GIQualityPreset p : presets) {
        const std::vector<GIDiagnostic> d =
            GIRegistry::Analyze(GIConfigFromPreset(p));
        CHECK(CountKind(d, GIDiagnosticKind::RedundantDuplicate) == 0u);
    }
}

TEST_CASE("GIRegistry::DeduplicateRedundant：移除 SSAO、保留 GTAO、幂等") {
    GIConfig c;
    c.ao.Set(GISourceId::SSAO, 0.7f);
    c.ao.Set(GISourceId::GTAO, 1.3f);

    CHECK(GIRegistry::DeduplicateRedundant(c));           // 发生改动
    CHECK_FALSE(c.ao.Has(GISourceId::SSAO));
    CHECK(c.ao.Has(GISourceId::GTAO));
    CHECK(c.ao.WeightOf(GISourceId::GTAO) == doctest::Approx(1.3f));
    CHECK(c.ao.count == 1u);

    CHECK_FALSE(GIRegistry::DeduplicateRedundant(c));     // 幂等
    CHECK(c.ao.count == 1u);

    // 去重后不再报严格冗余
    const std::vector<GIDiagnostic> d = GIRegistry::Analyze(c);
    CHECK(CountKind(d, GIDiagnosticKind::RedundantDuplicate) == 0u);
}

TEST_CASE("GIRegistry::DeduplicateRedundant：无可去重时不改动、返回 false") {
    GIConfig c = GIConfigFromPreset(GIQualityPreset::Medium);
    const GIConfig before = c;
    CHECK_FALSE(GIRegistry::DeduplicateRedundant(c));
    CHECK(c.ao.count == before.ao.count);
    CHECK(c.diffuse.count == before.diffuse.count);
    CHECK(c.ao.Has(GISourceId::SSAO));
}

TEST_CASE("GIRegistry::DeduplicateRedundant：去重前后合成结果逐值等价") {
    // 这是去重「可证等价」的核心断言：
    // shader 的 AO 合成分支对 SSAO/GTAO 都采样同一张 u_SSAO 纹理，
    // 故归一化平均 Σ(v·w)/Σw ≡ v —— 与只留 GTAO 的结果完全相同。
    GIConfig c;
    c.ao.Set(GISourceId::SSAO, 0.7f);
    c.ao.Set(GISourceId::GTAO, 1.3f);

    // 在若干不同的「纹理值」上比较（含 0 / 1 / 中间值）
    const float kSamples[] = { 0.0f, 0.25f, 0.5f, 1.0f, 0.8f };
    for (float v : kSamples) {
        const float before = NormalizedBlend(c.ao, v);
        GIConfig deduped = c;
        CHECK(GIRegistry::DeduplicateRedundant(deduped));
        const float after = NormalizedBlend(deduped.ao, v);
        CHECK(after == doctest::Approx(before));
        CHECK(after == doctest::Approx(v));      // 且等于源值本身（归一化平均取自身）
    }
}

TEST_CASE("GIRegistry::DeduplicateRedundant：只影响 SSAO/GTAO，不动其他源") {
    GIConfig c;
    c.diffuse.Set(GISourceId::IBL, 1.0f);
    c.diffuse.Set(GISourceId::SSGI, 1.0f);
    c.ao.Set(GISourceId::SSAO, 1.0f);
    c.ao.Set(GISourceId::GTAO, 1.0f);

    CHECK(GIRegistry::DeduplicateRedundant(c));
    CHECK(c.diffuse.count == 2u);
    CHECK(c.diffuse.Has(GISourceId::IBL));
    CHECK(c.diffuse.Has(GISourceId::SSGI));
    CHECK(c.ao.count == 1u);
    CHECK(c.ao.Has(GISourceId::GTAO));
}
