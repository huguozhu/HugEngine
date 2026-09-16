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

// ============================================================
// 源分类 —— 用**谓词**表达，不用「频段」枚举
//
// 历史：这里原有一个 `GIBand { Low, Mid, High }`（显示为「低频/中频/高频」）。
// 它被删除，原因是**名字承诺了一个它并未表达的东西**：该枚举实际是按
// **估计器类别**划分，而且混了两个正交维度——
//   · Low ↔ Mid 的分界是【尺度】：世界空间大范围（环境图 / 3m 探针网格）
//                               vs 屏幕空间像素级
//   · Mid ↔ High 的分界是【精度】：屏幕空间近似 vs 光追精确
// 反证：RTGI 输出为 1/4 分辨率，其**空间分辨率低于**全分辨率 SSGI，却被标成「高频」——
//       可见 `High` 表达的是精度而非空间频率。
// 直接后果：`GIBandOf(SSGI)=Mid` 而 `GIBandOf(RTGI)=High`，尽管二者估的是
// **同一个物理量**（间接漫反射）。这也使该枚举无法充当 P5 的频率边界。
//
// 现在改为三个**互斥且完备**的谓词（对 11 个源构成一个无歧义的划分），
// 面板标签由它们推导（见 GISourceClassName）。
// ============================================================

/// 世界空间 / 预计算环境类源：IBL · Lightmap · DDGI
/// （不依赖屏幕覆盖，屏外依然有效）
inline bool IsWorldSpaceSource(GISourceId id) {
    return id == GISourceId::IBL || id == GISourceId::Lightmap
        || id == GISourceId::DDGI;
}

/// 屏幕空间（或单次反弹光栅）类源：SSGI · SSR · SSAO · RSM · GTAO
/// （逐屏幕像素估计，受屏幕覆盖限制）
inline bool IsScreenSpaceSource(GISourceId id) {
    return id == GISourceId::SSGI || id == GISourceId::SSR
        || id == GISourceId::SSAO || id == GISourceId::RSM
        || id == GISourceId::GTAO;
}

/// 硬件光追类源：RTGI · RTReflection · RTAO
/// （定义在此处而非能力位一节：它是分类三谓词之一，且 `GISourceClassName` 依赖它）
inline bool IsRayTracingSource(GISourceId id) {
    return id == GISourceId::RTGI || id == GISourceId::RTReflection
        || id == GISourceId::RTAO;
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

/// 源的**估计器类别**名称（面板显示用）。
/// 由上述三个谓词推导，保证与谓词同源、不会再出现「名字与语义脱节」。
inline const char* GISourceClassName(GISourceId id) {
    if (IsWorldSpaceSource(id))   return "环境（世界空间）";
    if (IsScreenSpaceSource(id))  return "屏幕空间";
    if (IsRayTracingSource(id))   return "光追";
    return "?";
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

/// 是否为「需要硬件光追」的源（分类三谓词之一；另两个见文件上方）
///
/// 定义在此处会晚于 `GISourceClassName` 的使用，故实际定义已上移至
/// 源分类一节（那里的三个谓词共同构成对 11 个源的划分）。
/// 此处仅保留注释作为交叉索引，避免后来者重复定义。

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
// GI 配置诊断（P0 · REDUNDANCY）
//
// 动机（文档 §3.5）：本架构要求**每个已启用的源都整幅、每帧**产出完整通道缓冲，
// 因此「启用一个没有增益的源」等于**白付一份全量成本**；而层栈当前**允许**
// 这类配置且不给任何提示。
//
// 本组设施只做**静态诊断**，不改变渲染行为——唯一例外是
// GIRegistry::DeduplicateRedundant（去重，且可证与去重前逐像素等价）。
// ============================================================

/// 通道标识（诊断报告用）
enum class GIChannelId : u8 { Diffuse = 0, Specular = 1, AO = 2 };

inline const char* GIChannelName(GIChannelId ch) {
    switch (ch) {
    case GIChannelId::Diffuse:  return "Diffuse（间接漫反射）";
    case GIChannelId::Specular: return "Specular（间接镜面）";
    case GIChannelId::AO:       return "AO（环境光遮蔽）";
    default:                    return "?";
    }
}

/// 诊断类别
enum class GIDiagnosticKind : u8 {
    None = 0,
    /// 严格冗余：两个源在 shader 合成端解析到**同一张纹理**，
    /// 归一化平均等于取自身 → 零增益。**可安全去重**。
    RedundantDuplicate,
    /// 重复估计：同一物理量的「屏幕空间」与「光追」两份估计。
    /// 成本翻倍且归一化会互相稀释——但这是 S1.5 的**有意设计**，故仅提示、不强制剔除。
    DuplicateEstimate,
    /// 相关性：一方以另一方为 miss 回退来源 → 非独立估计，归一化失去无偏性。
    CorrelatedEstimates,
    /// 成本提示：通道含多个源 → 每帧为每个源各跑一遍整幅 pass。
    MultiSourceCost,
};

/// 一条诊断结果
struct GIDiagnostic {
    GIChannelId      channel = GIChannelId::Diffuse;
    GIDiagnosticKind kind    = GIDiagnosticKind::None;
    GISourceId       a       = GISourceId::None;   // 涉及源（主）
    GISourceId       b       = GISourceId::None;   // 涉及源（无则 None）
    const char*      detail  = "";                 // 静态字符串，不做堆分配
};

inline const char* GIDiagnosticKindName(GIDiagnosticKind k) {
    switch (k) {
    case GIDiagnosticKind::RedundantDuplicate: return "严格冗余";
    case GIDiagnosticKind::DuplicateEstimate:  return "重复估计";
    case GIDiagnosticKind::CorrelatedEstimates:return "相关估计";
    case GIDiagnosticKind::MultiSourceCost:    return "成本提示";
    default:                                   return "无";
    }
}

// ============================================================
// GIRegistry — 可用性判断 + 自动降级 + 配置诊断
//
// 可用性判断 = 管线能力（PipelineCaps：该管线是否提供此 GI 源）
//            ∧ 设备能力（rtSupported：光追源需硬件光追）
//
//   1. IsAvailable：查询某 GI 源在当前管线 + 设备下是否可用
//   2. Degrade：对 GIConfig 的四个通道层栈逐源裁剪（移除不可用源），
//               并保证每通道至少保留一个可用兜底源
//   3. Analyze / DeduplicateRedundant：冗余配置诊断与可证等价去重（P0）
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

    // ────────────────────────────────────────────────────────
    // 配置诊断（P0 · REDUNDANCY）
    // ────────────────────────────────────────────────────────

    /// 两个源是否「严格冗余」——即它们在 shader 合成端解析到**同一张纹理**。
    ///
    /// 目前只有 SSAO / GTAO：
    ///   · `ScreenAOProvider` 用**同一个 SSAO pass**（`SyncToStack` 只切换片段着色器）；
    ///   · `DeferredLighting.frag` 的 AO 合成分支对二者都采样 `u_SSAO`。
    /// 故二者若同时入栈，归一化平均 Σ(v·w)/Σw ≡ v —— 与只留其一**逐像素等价**。
    static bool IsStrictlyRedundant(GISourceId a, GISourceId b) {
        return (a == GISourceId::SSAO && b == GISourceId::GTAO) ||
               (a == GISourceId::GTAO && b == GISourceId::SSAO);
    }

    /// 屏幕空间源所对应的「同一物理量的光追源」（无对应则返回 None）
    ///
    /// 注意：二者属于**不同的估计器类别**（前者 `IsScreenSpaceSource`、
    /// 后者 `IsRayTracingSource`），但估的是**同一个物理量**——
    /// 这正是本对应关系存在的意义（用于诊断「重复估计」）。
    static GISourceId RTCounterpartOf(GISourceId ss) {
        switch (ss) {
        case GISourceId::SSGI: return GISourceId::RTGI;         // 间接漫反射
        case GISourceId::SSR:  return GISourceId::RTReflection; // 间接镜面
        case GISourceId::SSAO: return GISourceId::RTAO;         // 环境光遮蔽
        case GISourceId::GTAO: return GISourceId::RTAO;         // 环境光遮蔽
        default:               return GISourceId::None;
        }
    }

    /// 在该层栈内，该源是否为「实际生效」的那个。
    /// 严格冗余组内取**高质量者**：GTAO 优先于 SSAO（GTAO 是 SSAO 的高质量替代，
    /// 且 pass 模式由 stack.Has(GTAO) 决定）。
    static bool IsEffectiveSource(const GIChannelStack& st, GISourceId id) {
        if (id == GISourceId::SSAO && st.Has(GISourceId::GTAO)) return false;
        return true;
    }

    /// 诊断单个通道层栈（结果**追加**到 out，不清空）
    static void AnalyzeStack(GIChannelId ch, const GIChannelStack& st,
                             std::vector<GIDiagnostic>& out) {
        const auto has = [&st](GISourceId id) { return st.Has(id); };

        // 1) 严格冗余：解析到同一纹理的重复源（当前仅 SSAO/GTAO）
        for (u32 i = 0; i < st.count; i++) {
            for (u32 j = i + 1; j < st.count; j++) {
                if (!IsStrictlyRedundant(st.sources[i].id, st.sources[j].id)) continue;
                out.push_back(GIDiagnostic{
                    ch, GIDiagnosticKind::RedundantDuplicate,
                    st.sources[i].id, st.sources[j].id,
                    "两者共用同一 pass 与同一输出纹理，归一化平均等于取自身 → 零增益，可安全去重"});
            }
        }

        // 2) 重复估计：屏幕空间源与其「同一物理量的光追源」同时启用
        //    （只对实际生效的源报告，避免 SSAO/GTAO 同时在场时重复报两条）
        for (u32 i = 0; i < st.count; i++) {
            const GISourceId ss = st.sources[i].id;
            if (!IsEffectiveSource(st, ss)) continue;
            const GISourceId rt = RTCounterpartOf(ss);
            if (rt == GISourceId::None || !has(rt)) continue;
            out.push_back(GIDiagnostic{
                ch, GIDiagnosticKind::DuplicateEstimate, ss, rt,
                "同一物理量的两份逐屏幕像素估计：成本翻倍，且归一化会互相稀释"
                "（S1.5 有意允许同时参与，故仅提示、不建议强制剔除）"});
        }

        // 3) 相关性：RTGI 的 miss 回退来源即 DDGI → 非独立估计
        if (has(GISourceId::RTGI) && has(GISourceId::DDGI)) {
            out.push_back(GIDiagnostic{
                ch, GIDiagnosticKind::CorrelatedEstimates,
                GISourceId::RTGI, GISourceId::DDGI,
                "RTGI 的 miss 回退即 DDGI，两者非独立估计 → 归一化平均失去无偏性"});
        }

        // 4) 成本提示：多源 = 每帧多份整幅 pass（§3.5）
        if (st.count > 1) {
            out.push_back(GIDiagnostic{
                ch, GIDiagnosticKind::MultiSourceCost,
                GISourceId::None, GISourceId::None,
                "通道含多个源：每帧将为每个源各跑一遍整幅 pass（成本随源数线性增长，见文档 §3.5）"});
        }
    }

    /// 诊断整份配置（三个能量通道）
    static std::vector<GIDiagnostic> Analyze(const GIConfig& c) {
        std::vector<GIDiagnostic> out;
        AnalyzeStack(GIChannelId::Diffuse,  c.diffuse,  out);
        AnalyzeStack(GIChannelId::Specular, c.specular, out);
        AnalyzeStack(GIChannelId::AO,       c.ao,       out);
        return out;
    }

    /// 去掉「严格冗余」的重复源 —— **可证与去重前逐像素等价**。
    ///
    /// 规则：SSAO / GTAO 同时在场时保留 **GTAO**，移除 SSAO。
    /// 等价性依据（两条同时成立才成立）：
    ///   1. shader 的 AO 合成分支对二者都采样**同一张 `u_SSAO` 纹理**，
    ///      故 Σ(v·w)/Σw ≡ v —— 权重取值不影响结果；
    ///   2. pass 模式由 `stack.Has(GTAO)` 决定，去重前后都是 GTAO 模式。
    ///
    /// @return 是否发生了改动（幂等：再次调用返回 false）
    static bool DeduplicateRedundant(GIConfig& c) {
        bool changed = false;
        GIChannelStack* stacks[] = { &c.diffuse, &c.specular, &c.ao };
        for (GIChannelStack* st : stacks) {
            if (st->Has(GISourceId::SSAO) && st->Has(GISourceId::GTAO)) {
                st->Remove(GISourceId::SSAO);   // 保留 GTAO（高质量替代）
                changed = true;
            }
        }
        return changed;
    }
};

} // namespace he::render
