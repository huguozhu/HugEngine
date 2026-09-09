#pragma once

// ============================================================
// GI/GIConfig.h — GI 配置 + 质量档位（M2 数据驱动）
//
// 单一数据结构驱动所有 GI 通道（shadow/ao/specular/diffuse）：
//   - 4 质量档位（Low/Medium/High/Ultra）一键切换
//   - ImGui 面板任意组合 4 通道 + 每通道强度
//   - 帧图按 shouldRun 判定注册/分配 pass（未选中的不注册）
// 依赖 M1 的 GIChannel/GIChannels（LightingPass.h）。
// ============================================================

#include "Pipeline/LightingPass.h"
#include "GI/GlobalIllumination.h"

namespace he::render {

/// 质量档位
enum class GIQualityPreset : u8 {
    Low    = 0,   // 性能优先（半分辨率 SSGI，无 DDGI）
    Medium = 1,   // 平衡（SSGI + DDGI）
    High   = 2,   // 高质量（全分辨率 SSGI + DDGI）
    Ultra  = 3,   // 参考级（预留更高采样/更强间接）
};

// ============================================================
// 管线 GI 能力位 — 各渲染管线支持的通道子集
//
// 同一份 GIConfig 在不同管线下可用性不同（如 RT 系列仅 HybridRT 提供），
// GIRegistry 据此做「管线能力 + 设备能力」双重判断与降级。
// ============================================================
enum PipelineGICap : u32 {
    kPipelineGINone         = 0,
    kPipelineGIShadowRaster = 1u << 0,   // 光栅阴影（CSM/点光/聚光/矩形）
    kPipelineGIShadowRT     = 1u << 1,   // 硬件光追阴影
    kPipelineGIAOSSAO       = 1u << 2,   // 屏幕空间 AO
    kPipelineGIAORTAO       = 1u << 3,   // 硬件光追 AO
    kPipelineGISpecSSR      = 1u << 4,   // 屏幕空间反射
    kPipelineGISpecRT       = 1u << 5,   // 硬件光追反射
    kPipelineGIDiffSSGI     = 1u << 6,   // 屏幕空间 GI
    kPipelineGIDiffDDGI     = 1u << 7,   // DDGI 探针 GI
    kPipelineGIDiffRTGI     = 1u << 8,   // 硬件光追 GI
    kPipelineGIDiffIBL      = 1u << 9,   // IBL 环境辐照度
    kPipelineGIDiffRSM      = 1u << 10,  // RSM 间接光
};

/// 通道技术 → 管线能力位
inline u32 ToPipelineCap(ShadowChannel s) {
    switch (s) {
    case ShadowChannel::Raster: return kPipelineGIShadowRaster;
    case ShadowChannel::RT:     return kPipelineGIShadowRT;
    default:                    return kPipelineGINone;
    }
}
inline u32 ToPipelineCap(AOChannel s) {
    switch (s) {
    case AOChannel::SSAO: return kPipelineGIAOSSAO;
    case AOChannel::RTAO: return kPipelineGIAORTAO;
    default:              return kPipelineGINone;
    }
}
inline u32 ToPipelineCap(SpecularChannel s) {
    switch (s) {
    case SpecularChannel::SSR: return kPipelineGISpecSSR;
    case SpecularChannel::RT:  return kPipelineGISpecRT;
    default:                   return kPipelineGINone;
    }
}
inline u32 ToPipelineCap(DiffuseChannel s) {
    switch (s) {
    case DiffuseChannel::SSGI: return kPipelineGIDiffSSGI;
    case DiffuseChannel::RTGI: return kPipelineGIDiffRTGI;
    default:                   return kPipelineGINone;
    }
}

/// 各管线能力预设
namespace PipelineCaps {
    constexpr u32 Forward  = kPipelineGIShadowRaster | kPipelineGIAOSSAO | kPipelineGISpecSSR
                           | kPipelineGIDiffIBL | kPipelineGIDiffRSM;
    constexpr u32 Deferred = Forward | kPipelineGIDiffSSGI | kPipelineGIDiffDDGI;
    constexpr u32 HybridRT = Deferred | kPipelineGIShadowRT | kPipelineGIAORTAO
                           | kPipelineGISpecRT | kPipelineGIDiffRTGI;
}

/// GI 配置（单一数据源，面板与帧图共用）
struct GIConfig {
    // 4 通道技术选型（独立枚举，类型安全）
    ShadowChannel   shadow   = ShadowChannel::Raster;
    AOChannel       ao       = AOChannel::SSAO;
    SpecularChannel specular = SpecularChannel::SSR;
    DiffuseChannel  diffuse  = DiffuseChannel::SSGI;

    // 通道强度（与 M1.2 push constant giIntensity/aoIntensity 对齐）
    float giIntensity = 1.0f;   // 间接漫反射 GI 总强度
    float aoIntensity = 1.0f;   // AO 强度
    bool  ddgiOverlay     = true;   // DDGI 是否叠加（可与 SSGI/RT GI 组合）
    bool  halfRes     = false;  // 半分辨率计算（性能优先）

    /// 生成 GIChannels（M1 接口，供 LightingPass 消费）
    GIChannels ToInputSources() const {
        GIChannels s;
        s.shadow   = shadow;
        s.ao       = ao;
        s.specular = specular;
        s.diffuse  = diffuse;
        s.ddgiOverlay  = ddgiOverlay;
        return s;
    }

    /// 各通道是否应注册 pass（未选中的不注册不分配）
    bool ShouldRunAO()       const { return ao != AOChannel::None; }
    bool ShouldRunSpecular() const { return specular != SpecularChannel::None; }
    bool ShouldRunSSGI()     const { return diffuse == DiffuseChannel::SSGI; }
    bool ShouldRunDDGI()     const { return ddgiOverlay && diffuse != DiffuseChannel::None; }
};

/// 按档位生成默认配置（4 档预设）
GIConfig GIConfigFromPreset(GIQualityPreset p);

} // namespace he::render
