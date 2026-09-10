#pragma once

// ============================================================
// GI/GIConfig.h — GI 配置 + 质量档位（M2 数据驱动 / P3 层栈架构）
//
// 单一数据结构驱动所有 GI 通道（shadow/ao/specular/diffuse）：
//   - 4 质量档位（Low/Medium/High/Ultra）一键切换
//   - 每通道一个「源层栈」：可同时启用多个 GI 源（低频探针 / 屏幕空间 / 光追）
//   - 帧图按各源是否参与（weight>0）注册 pass
// 依赖 M1 的 GIChannel/GIChannels（LightingPass.h）。
//
// 为什么用层栈而不是「单值枚举」：
//   多个 GI 源描述的是同一个物理量（间接入射辐射度），"选一个"无法表达
//   "远场探针管低频 + 屏幕空间管中频 + 光追管高频"的分工；层栈让每通道可
//   同时持有多个源，并按权重归一化合成（无双重计数）。
// ============================================================

#include "Pipeline/LightingPass.h"
#include "GI/GlobalIllumination.h"

namespace he::render {

/// 质量档位
enum class GIQualityPreset : u8 {
    Low    = 0,   // 性能优先（半分辨率 SSGI，无 DDGI）
    Medium = 1,   // 平衡（SSGI + DDGI）
    High   = 2,   // 高质量（全分辨率 SSGI + DDGI）
    Ultra  = 3,   // 参考级（RTGI 优先，不可用则降级）
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
    RTShadow      = 11,  // 硬件光追阴影
    // 光栅阴影
    RasterShadow  = 12,  // CSM + 点光/聚光/矩形阴影贴图
};

/// GI 源的天然频段
enum class GIBand : u8 {
    Low  = 0,   // 低频、大范围（IBL / Lightmap / DDGI）
    Mid  = 1,   // 中频、近处细节（SSGI / SSR / SSAO / RSM）
    High = 2,   // 高频、精确（RTGI / RTReflection / RTAO / RTShadow）
};

/// 源 → 频段
inline GIBand GIBandOf(GISourceId id) {
    switch (id) {
    case GISourceId::IBL:
    case GISourceId::Lightmap:
    case GISourceId::DDGI:          return GIBand::Low;
    case GISourceId::RTGI:
    case GISourceId::RTReflection:
    case GISourceId::RTAO:
    case GISourceId::RTShadow:      return GIBand::High;
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
    case GISourceId::RSM:          return "RSM";
    case GISourceId::RTGI:         return "RTGI";
    case GISourceId::RTReflection: return "RT Reflection";
    case GISourceId::RTAO:         return "RTAO";
    case GISourceId::RTShadow:     return "RT Shadow";
    case GISourceId::RasterShadow: return "Raster Shadow";
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
    GIBlendMode  mode  = GIBlendMode::Normalized;   // 合成方式（定义于 LightingPass.h）

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
    case GISourceId::RasterShadow:  return kPipelineGIShadowRaster;
    case GISourceId::RTShadow:      return kPipelineGIShadowRT;
    case GISourceId::SSAO:          return kPipelineGIAOSSAO;
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

/// 是否为「需要硬件光追」的源
inline bool IsRayTracingSource(GISourceId id) {
    return id == GISourceId::RTGI || id == GISourceId::RTReflection
        || id == GISourceId::RTAO || id == GISourceId::RTShadow;
}

/// 各管线能力预设
namespace PipelineCaps {
    // Forward：光栅阴影 + IBL 环境 + RSM 间接（无屏幕空间 SSGI/SSR/SSAO，无 DDGI）
    constexpr u32 Forward  = kPipelineGIShadowRaster | kPipelineGIDiffIBL | kPipelineGISpecIBL
                           | kPipelineGIDiffRSM;
    constexpr u32 Deferred = Forward | kPipelineGIAOSSAO | kPipelineGISpecSSR
                           | kPipelineGIDiffSSGI | kPipelineGIDiffDDGI;
    constexpr u32 HybridRT = Deferred | kPipelineGIShadowRT | kPipelineGIAORTAO
                           | kPipelineGISpecRT | kPipelineGIDiffRTGI;
}

// ============================================================
// GIConfig — 单一数据源（面板与帧图共用）
//
// P3：4 个通道各持一个「源层栈」，取代原先的单值枚举；
//     "选哪个技术" = 该源 weight > 0，"融合" = 多个源 weight > 0。
// ============================================================
struct GIConfig {
    // ── 4 个通道的源层栈 ──
    GIChannelStack diffuse;    // 间接漫反射（IBL/DDGI/SSGI/RSM/RTGI）
    GIChannelStack specular;   // 间接镜面（IBL/SSR/RTReflection）
    GIChannelStack ao;         // 环境光遮蔽（SSAO/RTAO）
    GIChannelStack shadow;     // 阴影（RasterShadow/RTShadow）

    // ── 强度与全局开关 ──
    float giIntensity = 1.0f;   // 间接漫反射 GI 总强度（与 push constant 对齐）
    float aoIntensity = 1.0f;   // AO 强度
    bool  rsmIndirect = true;   // RSM 间接光（Forward 管线的间接漫反射来源）
    bool  halfRes     = false;  // 半分辨率计算（性能优先）

    // ── 帧图门控（从层栈派生——不再有单值枚举）──
    [[nodiscard]] bool ShouldRunSSGI()    const { return diffuse.Has(GISourceId::SSGI); }
    [[nodiscard]] bool ShouldRunDDGI()    const { return diffuse.Has(GISourceId::DDGI); }
    [[nodiscard]] bool ShouldRunRTGI()    const { return diffuse.Has(GISourceId::RTGI); }
    [[nodiscard]] bool ShouldRunRSM()     const { return diffuse.Has(GISourceId::RSM) && rsmIndirect; }
    [[nodiscard]] bool ShouldRunSSR()     const { return specular.Has(GISourceId::SSR); }
    [[nodiscard]] bool ShouldRunRTReflection() const { return specular.Has(GISourceId::RTReflection); }
    [[nodiscard]] bool ShouldRunSpecular() const { return ShouldRunSSR() || ShouldRunRTReflection(); }
    [[nodiscard]] bool ShouldRunSSAO()    const { return ao.Has(GISourceId::SSAO); }
    [[nodiscard]] bool ShouldRunRTAO()    const { return ao.Has(GISourceId::RTAO); }
    [[nodiscard]] bool ShouldRunAO()      const { return ao.AnyActive(); }
    [[nodiscard]] bool ShouldRunShadow()  const { return shadow.AnyActive(); }
    /// 使用横跨屏幕空间/光追的"精确"漫反射源（决定 shader 的 rtDiffuseSource 与有效性）
    [[nodiscard]] bool UseScreenDiffuse() const { return ShouldRunSSGI() || ShouldRunRTGI(); }

    /// 生成 GIChannels（M1 接口，供 LightingPass 消费——从层栈派生）
    GIChannels ToInputSources() const {
        GIChannels s;
        s.shadow   = shadow.Has(GISourceId::RTShadow) ? ShadowChannel::RT : ShadowChannel::Raster;
        s.ao       = ShouldRunRTAO() ? AOChannel::RTAO : (ShouldRunSSAO() ? AOChannel::SSAO : AOChannel::None);
        s.specular = ShouldRunRTReflection() ? SpecularChannel::RT
                   : (ShouldRunSSR() ? SpecularChannel::SSR : SpecularChannel::None);
        s.diffuse  = ShouldRunRTGI() ? DiffuseChannel::RTGI
                   : (ShouldRunSSGI() ? DiffuseChannel::SSGI : DiffuseChannel::None);
        return s;
    }
};

/// 按档位生成默认配置（4 档预设——层栈形态）
GIConfig GIConfigFromPreset(GIQualityPreset p);

} // namespace he::render
