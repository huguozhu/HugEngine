#pragma once

// ============================================================
// GI/GITypes.h — GI 纯数据类型与可用性规则（RHI-free）
//
// 目的：把 GI 的**数据模型**（源标识 / 频段 / 层栈 / 档位 / 管线能力 / 降级规则）
// 与 **RHI 依赖**彻底解耦，使单元测试可以只包含本头文件、**无需链接 HugEngineRender**
// （链接 Render 会连带拉入 RHI/Vulkan 以及 slangc 生成的 SPV 头，代价过高）。
//
// 约束：本头文件只允许依赖 Core/Types.h 与标准库。
//       任何 rhi:: 类型、纹理句柄、设备指针都不得出现在这里。
//
// 内容来源（迁移前的位置）：
//   · Pipeline/LightingPass.h → ShadowChannel / GIBlendMode /
//                              GISourceSlotData / GIChannelBlendData
//   · GI/GIConfig.h           → 源标识 / 频段 / 层栈 / 档位 / 管线能力 / GIConfig
//   · GI/GIRegistry.h         → 可用性与降级规则 GIRegistry
// ============================================================

#include "Core/Types.h"

namespace he::render {

// ============================================================
// 阴影通道
//
// 阴影是**可见性（乘法项）**而非能量（加法项）：
//   · 合成运算是 color *= visibility，不是加权求和
//   · 没有频段概念（不参与低频/中频/高频分工）
//   · 没有「距离让位」（阴影不该随距离让位给另一种阴影）
// 故不进 GI 层栈，用独立枚举表达。
// ============================================================
enum class ShadowChannel : u8 { None = 0, Raster, RT };

// ============================================================
// 多源间接光的合成方式
// ============================================================
enum class GIBlendMode : u8 {
    Additive   = 0,   // 直接相加（旧行为——双重计数，仅作 A/B 对照）
    Normalized = 1,   // 归一化加权：Σ(源×w)/Σw，权重和=1 → 无双重计数（推荐）
};

// ============================================================
// GI 源（跨通道通用标识）
//
// 每个源天然属于某个频段，决定它在分层合成中的角色：
//   低频（远场/环境）→ 兜底大范围光；中频（近处）→ 细节；高频（精确）→ 光追
// ============================================================
enum class GISourceId : u8 {
    None = 0,
    // 低频（远场 / 环境，无距离限制）
    IBL           = 1,   // 环境辐照度 / 预滤波（所有管线）
    Lightmap      = 2,   // 烘焙光照（预留）
    DDGI          = 3,   // 动态漫反射探针网格
    // 中频（近处细节）
    SSGI          = 4,   // 屏幕空间间接漫反射
    SSR           = 5,   // 屏幕空间反射
    SSAO          = 6,   // 屏幕空间环境光遮蔽
    RSM           = 7,   // 反射阴影贴图间接光（Forward）
    // 高频（精确，需硬件光追）
    RTGI          = 8,   // 硬件光追间接漫反射
    RTReflection  = 9,   // 硬件光追反射
    RTAO          = 10,  // 硬件光追环境光遮蔽
    GTAO          = 11,  // 地平线切片 AO（Ground Truth AO，SSAO 的高质量替代）
};

/// GI 源的天然频段
enum class GIBand : u8 {
    Low  = 0,   // 低频、大范围（IBL / Lightmap / DDGI）
    Mid  = 1,   // 中频、近处细节（SSGI / SSR / SSAO / RSM / GTAO）
    High = 2,   // 高频、精确（RTGI / RTReflection / RTAO）
};

/// 源 → 频段
///
/// 注：光追三源必须显式返回 High。此前它们与 default 共用同一条 fallthrough，
///     导致 GIBand::High 成为**不可达枚举值**、面板把 RT 源标成「中频」。
inline GIBand GIBandOf(GISourceId id) {
    switch (id) {
    case GISourceId::IBL:
    case GISourceId::Lightmap:
    case GISourceId::DDGI:          return GIBand::Low;
    case GISourceId::RTGI:
    case GISourceId::RTReflection:
    case GISourceId::RTAO:          return GIBand::High;
    default:                        return GIBand::Mid;
    }
}

/// 源名称（日志 / 面板显示）
inline const char* GISourceName(GISourceId id) {
    switch (id) {
    case GISourceId::IBL:          return "IBL";
    case GISourceId::Lightmap:     return "Lightmap";
    case GISourceId::DDGI:         return "DDGI";
    case GISourceId::SSGI:         return "SSGI";
    case GISourceId::SSR:          return "SSR";
    case GISourceId::SSAO:         return "SSAO";
    case GISourceId::GTAO:         return "GTAO";
    case GISourceId::RSM:          return "RSM";
    case GISourceId::RTGI:         return "RTGI";
    case GISourceId::RTReflection: return "RT Reflection";
    case GISourceId::RTAO:         return "RTAO";
    default:                       return "None";
    }
}

/// 频段名称
inline const char* GIBandName(GIBand b) {
    switch (b) {
    case GIBand::Low:  return "低频";
    case GIBand::Mid:  return "中频";
    case GIBand::High: return "高频";
    default:           return "?";
    }
}

// ============================================================
// 单个 GI 源描述 + 通道层栈
// ============================================================

/// 一个参与合成的 GI 源
struct GISourceDesc {
    GISourceId id = GISourceId::None;
    float      weight = 1.0f;          // 0 = 不参与合成（等价于"未选该技术"）
    float      falloffDistance = 0.0f; // 可选「距离让位」（0=不启用；性能/艺术控制）
};

/// 通道层栈：一个通道可同时持有多个源（有序：低频 → 高频）
struct GIChannelStack {
    static constexpr u32 kMaxSources = 4;

    GISourceDesc sources[kMaxSources];
    u32          count = 0;
    GIBlendMode  mode  = GIBlendMode::Normalized;   // 合成方式

    /// 该源是否存在且参与合成（weight > 0）
    [[nodiscard]] bool Has(GISourceId id) const {
        for (u32 i = 0; i < count; i++) {
            if (sources[i].id == id && sources[i].weight > 0.0f) return true;
        }
        return false;
    }

    /// 该源的权重（不存在返回 0）
    [[nodiscard]] float WeightOf(GISourceId id) const {
        for (u32 i = 0; i < count; i++) {
            if (sources[i].id == id) return sources[i].weight;
        }
        return 0.0f;
    }

    /// 该源的「距离让位」（不存在返回 0 = 不启用）
    [[nodiscard]] float FalloffOf(GISourceId id) const {
        for (u32 i = 0; i < count; i++) {
            if (sources[i].id == id) return sources[i].falloffDistance;
        }
        return 0.0f;
    }

    /// 添加或更新一个源（weight <= 0 视为移除）
    void Set(GISourceId id, float weight, float falloffDistance = 0.0f) {
        for (u32 i = 0; i < count; i++) {
            if (sources[i].id == id) {
                sources[i].weight = weight;
                sources[i].falloffDistance = falloffDistance;
                if (weight <= 0.0f) Remove(id);
                return;
            }
        }
        if (weight <= 0.0f || count >= kMaxSources) return;
        sources[count].id = id;
        sources[count].weight = weight;
        sources[count].falloffDistance = falloffDistance;
        count++;
    }

    /// 移除一个源
    void Remove(GISourceId id) {
        for (u32 i = 0; i < count; i++) {
            if (sources[i].id == id) {
                for (u32 j = i; j + 1 < count; j++) sources[j] = sources[j + 1];
                count--;
                return;
            }
        }
    }

    void Clear() { count = 0; }

    /// 是否存在任一参与合成的源
    [[nodiscard]] bool AnyActive() const {
        for (u32 i = 0; i < count; i++) {
            if (sources[i].weight > 0.0f) return true;
        }
        return false;
    }
};

// ============================================================
// shader UBO 的镜像结构
//
// 这两个结构必须与 ShaderTypes.slang 的 GISourceSlot / GIChannelBlendParams
// 布局**逐字段一致**（GITypes.h 的单测会断言 sizeof，防止 C++/slang 双侧漂移）。
// ============================================================

/// 单个 GI 源槽（与 shader 的 GISourceSlot 布局一致）
struct GISourceSlotData {
    u32   id              = 0;        // GISourceId（决定 shader 走哪条采样分支）
    float weight          = 0.0f;     // 相对权重（0 = 不参与）
    float falloffDistance = 0.0f;     // 「距离让位」（0 = 不启用）
    u32   _pad            = 0;
};
static_assert(sizeof(GISourceSlotData) == 16, "GISourceSlotData must be 16 bytes（与 shader GISourceSlot 对齐）");

/// 单通道的合成参数（与 shader 的 GIChannelBlendParams 布局一致）
///
/// Wave 1：由「三个固定语义槽」改为「源数组」——槽位是通用容器，语义由 id 决定。
/// 这样新增 GI 只需 GISourceId 加一项 + SampleSource 加一个 case，
/// 不再需要改 UBO 结构 / 帧图映射 / shader 分支。
struct GIChannelBlendData {
    static constexpr u32 kMaxSources = 4;
    GISourceSlotData sources[kMaxSources];
    u32 count       = 0;
    u32 mode        = 1;   // GIBlendMode（0=相加对照, 1=归一化加权）
    u32 furnaceMode = 0;   // 白炉数值测试
    u32 _pad        = 0;

    /// 追加一个源（weight<=0 忽略；超出容量忽略）
    void Add(u32 sourceId, float w, float falloff = 0.0f) {
        if (w <= 0.0f || count >= kMaxSources) return;
        sources[count].id              = sourceId;
        sources[count].weight          = w;
        sources[count].falloffDistance = falloff;
        count++;
    }
};
static_assert(sizeof(GIChannelBlendData) == 80, "GIChannelBlendData must be 80 bytes（与 shader GIChannelBlendParams 对齐）");

// ============================================================
// 管线 GI 能力位 — 各管线支持的 GI 源子集
//
// GIRegistry 据此做「管线能力 + 设备能力」双重判断，并对层栈逐源裁剪。
// ============================================================
enum PipelineGICap : u32 {
    kPipelineGINone         = 0,
    kPipelineGIShadowRaster = 1u << 0,   // 光栅阴影（CSM/点光/聚光/矩形）
    kPipelineGIShadowRT     = 1u << 1,   // 硬件光追阴影
    kPipelineGIAOSSAO       = 1u << 2,   // 屏幕空间 AO
    kPipelineGIAORTAO       = 1u << 3,   // 硬件光追 AO
    kPipelineGISpecSSR      = 1u << 4,   // 屏幕空间反射
    kPipelineGISpecRT       = 1u << 5,   // 硬件光追反射
    kPipelineGISpecIBL      = 1u << 6,   // IBL 环境镜面（预滤波）
    kPipelineGIDiffSSGI     = 1u << 7,   // 屏幕空间 GI
    kPipelineGIDiffDDGI     = 1u << 8,   // DDGI 探针 GI
    kPipelineGIDiffRTGI     = 1u << 9,   // 硬件光追 GI
    kPipelineGIDiffIBL      = 1u << 10,  // IBL 环境辐照度
    kPipelineGIDiffRSM      = 1u << 11,  // RSM 间接光
};

/// GI 源 → 管线能力位
inline u32 ToPipelineCap(GISourceId id) {
    switch (id) {
    case GISourceId::SSAO:          return kPipelineGIAOSSAO;
    case GISourceId::GTAO:          return kPipelineGIAOSSAO;   // 同属 AO 通道能力（与 SSAO 互为替代）
    case GISourceId::RTAO:          return kPipelineGIAORTAO;
    case GISourceId::SSR:           return kPipelineGISpecSSR;
    case GISourceId::RTReflection:  return kPipelineGISpecRT;
    case GISourceId::SSGI:          return kPipelineGIDiffSSGI;
    case GISourceId::DDGI:          return kPipelineGIDiffDDGI;
    case GISourceId::RTGI:          return kPipelineGIDiffRTGI;
    case GISourceId::IBL:           return kPipelineGIDiffIBL | kPipelineGISpecIBL;
    case GISourceId::RSM:           return kPipelineGIDiffRSM;
    default:                        return kPipelineGINone;
    }
}

/// 阴影通道 → 管线能力位（阴影独立于层栈，单独做可用性/降级判断）
inline u32 ToPipelineCap(ShadowChannel s) {
    switch (s) {
    case ShadowChannel::Raster: return kPipelineGIShadowRaster;
    case ShadowChannel::RT:     return kPipelineGIShadowRT;
    default:                    return kPipelineGINone;
    }
}

/// 是否为「需要硬件光追」的源
inline bool IsRayTracingSource(GISourceId id) {
    return id == GISourceId::RTGI || id == GISourceId::RTReflection
        || id == GISourceId::RTAO;
}

/// 各管线能力预设
///
/// 注意区分两层判断：
///   · 管线能力（此处的位）：该管线**架构上**能否承载这个源
///   · 设备能力（Degrade 的 rtSupported）：光追源还需硬件支持
/// 由于光追已并入 Deferred（HybridRT 管线已移除），Deferred 的位包含全部
/// 光追源；无光追设备由 GIRegistry::Degrade 的 rtSupported 逐源裁剪。
namespace PipelineCaps {
    // Forward：光栅阴影 + IBL 环境 + RSM 间接
    //（前向着色无 GBuffer，故无屏幕空间源 SSGI/SSR/SSAO/GTAO，也无探针 DDGI）
    constexpr u32 Forward  = kPipelineGIShadowRaster | kPipelineGIDiffIBL | kPipelineGISpecIBL
                           | kPipelineGIDiffRSM;
    // Deferred：Forward + 屏幕空间源 + 探针 + 全部光追源
    constexpr u32 Deferred = Forward | kPipelineGIAOSSAO | kPipelineGISpecSSR
                           | kPipelineGIDiffSSGI | kPipelineGIDiffDDGI
                           | kPipelineGIShadowRT | kPipelineGIAORTAO
                           | kPipelineGISpecRT | kPipelineGIDiffRTGI;
    // 注：原先的 HybridRT 预设已移除——HybridRT 管线本身已删除，
    //     其光追能力位已并入 Deferred（无光追设备由 rtSupported 进一步裁剪）。
}

// ============================================================
// 质量档位
// ============================================================
enum class GIQualityPreset : u8 {
    Low    = 0,   // 性能优先（半分辨率 SSGI，无 DDGI）
    Medium = 1,   // 平衡（SSGI + DDGI）
    High   = 2,   // 高质量（全分辨率 SSGI + DDGI）
    Ultra  = 3,   // 参考级（RTGI 优先，不可用则降级）
};

// ============================================================
// GIConfig — 单一数据源（面板与帧图共用）
//
// P3：4 个通道各持一个「源层栈」，取代原先的单值枚举；
//     "选哪个技术" = 该源 weight > 0，"融合" = 多个源 weight > 0。
// ============================================================
struct GIConfig {
    // ── 3 个「能量通道」的源层栈 ──
    // （每个通道内的多个源描述的是**同一个物理量** → 归一化加权合成）
    GIChannelStack diffuse;    // 间接漫反射（IBL/DDGI/SSGI/RSM/RTGI）
    GIChannelStack specular;   // 间接镜面（IBL/SSR/RTReflection）
    GIChannelStack ao;         // 环境光遮蔽（SSAO/GTAO/RTAO）

    // ── 阴影通道：独立枚举，不进层栈 ──
    ShadowChannel shadow = ShadowChannel::Raster;

    // ── 强度与全局开关 ──
    float giIntensity = 1.0f;   // 间接漫反射 GI 总强度（与 push constant 对齐）
    float aoIntensity = 1.0f;   // AO 强度
    bool  rsmIndirect = true;   // RSM 间接光（Forward 管线的间接漫反射来源）
    bool  halfRes     = false;  // 半分辨率计算（性能优先）
    // 白炉数值测试：把白炉条件（全白环境 + albedo=1 + 关直接光）下的源真值
    // 代入**真实**合成路径，正确实现应恰好得到 1.0；>1 即存在归一化之外的双重计数。
    bool  furnaceMode = false;

    // ── 帧图门控 ──
    [[nodiscard]] bool ShouldRunSSGI()    const { return diffuse.Has(GISourceId::SSGI); }
    [[nodiscard]] bool ShouldRunDDGI()    const { return diffuse.Has(GISourceId::DDGI); }
    [[nodiscard]] bool ShouldRunRTGI()    const { return diffuse.Has(GISourceId::RTGI); }
    [[nodiscard]] bool ShouldRunRSM()     const { return diffuse.Has(GISourceId::RSM) && rsmIndirect; }
    [[nodiscard]] bool ShouldRunSSR()     const { return specular.Has(GISourceId::SSR); }
    [[nodiscard]] bool ShouldRunRTReflection() const { return specular.Has(GISourceId::RTReflection); }
    [[nodiscard]] bool ShouldRunSpecular() const { return ShouldRunSSR() || ShouldRunRTReflection(); }
    [[nodiscard]] bool ShouldRunSSAO()    const { return ao.Has(GISourceId::SSAO); }
    [[nodiscard]] bool ShouldRunGTAO()    const { return ao.Has(GISourceId::GTAO); }
    [[nodiscard]] bool ShouldRunRTAO()    const { return ao.Has(GISourceId::RTAO); }
    [[nodiscard]] bool ShouldRunAO()      const { return ao.AnyActive(); }
    // 阴影（从 ShadowChannel 枚举派生——与层栈无关）
    [[nodiscard]] bool ShouldRunShadow()  const { return shadow != ShadowChannel::None; }
    [[nodiscard]] bool ShouldRunRTShadow() const { return shadow == ShadowChannel::RT; }
    /// 任一通道是否启用了光追源（决定是否需要构建 TLAS 与 RT 效果 / 降噪链）
    [[nodiscard]] bool AnyRTSource() const {
        return ShouldRunRTGI() || ShouldRunRTReflection() || ShouldRunRTAO() || ShouldRunRTShadow();
    }
};

/// 按档位生成默认配置（4 档预设——层栈形态）
///
/// 每个档位都以 IBL / SSAO / 光栅阴影为**基线兜底**，档位只在其上叠加；
/// 切换档位只改「层栈内容 + 精度」，合成公式不变 → 无亮度跳变。
///
/// 声明为 inline：使单元测试可只包含本头文件即可验证档位与降级逻辑，
/// 无需链接 HugEngineRender。
inline GIConfig GIConfigFromPreset(GIQualityPreset p) {
    GIConfig c;

    // ── 公共基线：低频环境源（所有档位的兜底）──
    c.diffuse.Set(GISourceId::IBL, 1.0f);              // 环境辐照度（远场兜底）
    c.specular.Set(GISourceId::IBL, 1.0f);             // 环境镜面（预滤波）
    c.ao.Set(GISourceId::SSAO, 1.0f);
    c.shadow = ShadowChannel::Raster;   // 阴影独立于层栈（可见性乘法项）

    switch (p) {
    case GIQualityPreset::Low:
        // 性能优先：仅屏幕空间源（半分辨率），无探针
        c.diffuse.Set(GISourceId::SSGI, 1.0f);
        c.halfRes     = true;
        c.giIntensity = 0.6f;
        break;

    case GIQualityPreset::High:
        // 高质量：屏幕空间 + 探针（全分辨率）
        c.diffuse.Set(GISourceId::SSGI, 1.0f);
        c.diffuse.Set(GISourceId::DDGI, 1.0f);
        c.halfRes     = false;
        c.giIntensity = 1.0f;
        break;

    case GIQualityPreset::Ultra:
        // 参考级：光追源优先（不可用设备经 GIRegistry::Degrade 逐源裁剪后回退到 SSGI/DDGI）
        c.diffuse.Set(GISourceId::RTGI, 1.0f);
        c.diffuse.Set(GISourceId::DDGI, 1.0f);
        c.specular.Set(GISourceId::RTReflection, 1.0f);
        c.ao.Set(GISourceId::RTAO, 1.0f);
        c.shadow = ShadowChannel::RT;       // 参考级：光追阴影（无 RT 设备经 Degrade 回退光栅）
        c.halfRes     = false;
        c.giIntensity = 1.2f;
        break;

    case GIQualityPreset::Medium:
    default:
        // 平衡：屏幕空间 + 探针
        c.diffuse.Set(GISourceId::SSGI, 1.0f);
        c.diffuse.Set(GISourceId::DDGI, 1.0f);
        c.halfRes     = false;
        c.giIntensity = 0.8f;
        break;
    }
    return c;
}

// ============================================================
// GIRegistry — 可用性判断 + 自动降级
//
// 可用性判断 = 管线能力（PipelineCaps：该管线是否提供此 GI 源）
//            ∧ 设备能力（rtSupported：光追源需硬件光追）
//
//   1. IsAvailable：查询某 GI 源在当前管线 + 设备下是否可用
//   2. Degrade：对 GIConfig 的四个通道层栈逐源裁剪（移除不可用源），
//               并保证每通道至少保留一个可用兜底源
// ============================================================
class GIRegistry {
public:
    /// 某 GI 源是否可用（管线能力 ∧ 设备能力）
    static bool IsAvailable(GISourceId id, u32 pipelineCaps, bool rtSupported) {
        if (id == GISourceId::None) return false;
        const u32 cap = ToPipelineCap(id);
        if (cap == kPipelineGINone) return false;                 // 未注册的源
        if ((pipelineCaps & cap) != cap) return false;            // 管线不提供
        if (IsRayTracingSource(id) && !rtSupported) return false; // 设备无光追
        return true;
    }

    /// 阴影通道是否可用（阴影独立于层栈，单独判断）
    static bool IsAvailable(ShadowChannel s, u32 pipelineCaps, bool rtSupported) {
        if (s == ShadowChannel::None) return true;
        const u32 cap = ToPipelineCap(s);
        if (cap == kPipelineGINone) return false;
        if ((pipelineCaps & cap) != cap) return false;
        if (s == ShadowChannel::RT && !rtSupported) return false;
        return true;
    }

    /// 对单个通道层栈逐源裁剪（移除不可用源；weight<=0 的源也一并清理）
    static void DegradeStack(GIChannelStack& st, u32 pipelineCaps, bool rtSupported) {
        for (u32 i = 0; i < st.count; ) {
            const GISourceId id = st.sources[i].id;
            if (st.sources[i].weight <= 0.0f || !IsAvailable(id, pipelineCaps, rtSupported)) {
                st.Remove(id);   // Remove 会前移后续元素，故索引不递增
            } else {
                i++;
            }
        }
    }

    /// 把 GIConfig 四个通道层栈中的不可用源全部裁剪
    static GIConfig Degrade(const GIConfig& c, u32 pipelineCaps, bool rtSupported) {
        GIConfig out = c;
        DegradeStack(out.diffuse,  pipelineCaps, rtSupported);
        DegradeStack(out.specular, pipelineCaps, rtSupported);
        DegradeStack(out.ao,       pipelineCaps, rtSupported);
        // 阴影通道独立于层栈（可见性乘法项，不是能量源）→ 单独降级
        if (out.shadow == ShadowChannel::RT
            && (!rtSupported || (pipelineCaps & kPipelineGIShadowRT) == 0)) {
            out.shadow = ShadowChannel::Raster;   // RT 阴影不可用 → 回退光栅阴影
        }
        if (out.shadow == ShadowChannel::Raster
            && (pipelineCaps & kPipelineGIShadowRaster) == 0) {
            out.shadow = ShadowChannel::None;     // 该管线连光栅阴影都不支持
        }

        // ── 兜底：通道被裁空时补一个管线支持的源，避免该通道完全丢失 ──
        // 环境源（IBL）几乎所有管线都支持，作为最后兜底
        if (out.diffuse.count == 0 && IsAvailable(GISourceId::IBL, pipelineCaps, rtSupported)) {
            out.diffuse.Set(GISourceId::IBL, 1.0f);
        }
        if (out.specular.count == 0 && IsAvailable(GISourceId::IBL, pipelineCaps, rtSupported)) {
            out.specular.Set(GISourceId::IBL, 1.0f);
        }
        if (out.ao.count == 0 && IsAvailable(GISourceId::SSAO, pipelineCaps, rtSupported)) {
            out.ao.Set(GISourceId::SSAO, 1.0f);
        }
        return out;
    }
};

} // namespace he::render
