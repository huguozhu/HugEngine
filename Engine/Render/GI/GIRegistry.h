#pragma once

// ============================================================
// GI/GIRegistry.h — GI Provider 注册表 + 自动降级（M3）
//
// 可用性判断 = 管线能力（PipelineCaps：该管线是否提供此通道技术）
//            ∧ 设备能力（rtSupported：RT 系列需硬件光追）
//
//   1. IsAvailable：查询某通道技术在当前管线 + 设备下是否可用
//   2. FallbackOf：降级链（RTGI→SSGI、RTAO→SSAO、RT→SSR、RT 阴影→Raster）
//   3. Degrade：把 GIConfig 中不可用的通道技术自动降级到可用组合
// ============================================================

#include "Pipeline/LightingPass.h"
#include "GI/GIConfig.h"

namespace he::render {

class GIRegistry {
public:
    /// 各通道技术是否可用（管线能力 ∧ 设备能力）
    static bool IsAvailable(ShadowChannel s, u32 pipelineCaps, bool rtSupported) {
        if (s == ShadowChannel::None) return true;
        if ((pipelineCaps & ToPipelineCap(s)) == 0) return false;   // 管线不提供
        if (s == ShadowChannel::RT && !rtSupported) return false;   // 设备无光追
        return true;
    }
    static bool IsAvailable(AOChannel s, u32 pipelineCaps, bool rtSupported) {
        if (s == AOChannel::None) return true;
        if ((pipelineCaps & ToPipelineCap(s)) == 0) return false;
        if (s == AOChannel::RTAO && !rtSupported) return false;
        return true;
    }
    static bool IsAvailable(SpecularChannel s, u32 pipelineCaps, bool rtSupported) {
        if (s == SpecularChannel::None) return true;
        if ((pipelineCaps & ToPipelineCap(s)) == 0) return false;
        if (s == SpecularChannel::RT && !rtSupported) return false;
        return true;
    }
    static bool IsAvailable(DiffuseChannel s, u32 pipelineCaps, bool rtSupported) {
        if (s == DiffuseChannel::None) return true;
        if ((pipelineCaps & ToPipelineCap(s)) == 0) return false;
        if (s == DiffuseChannel::RTGI && !rtSupported) return false;
        return true;
    }

    /// 降级链：不可用的技术 → 可用的替代
    static ShadowChannel FallbackOf(ShadowChannel s) {
        return s == ShadowChannel::RT ? ShadowChannel::Raster : s;
    }
    static AOChannel FallbackOf(AOChannel s) {
        return s == AOChannel::RTAO ? AOChannel::SSAO : s;
    }
    static SpecularChannel FallbackOf(SpecularChannel s) {
        return s == SpecularChannel::RT ? SpecularChannel::SSR : s;
    }
    static DiffuseChannel FallbackOf(DiffuseChannel s) {
        // RTGI→SSGI→None（DDGI 由 ddgiOverlay 独立叠加）
        if (s == DiffuseChannel::RTGI) return DiffuseChannel::SSGI;
        return DiffuseChannel::None;
    }

    /// 把 GIConfig 中不可用的通道技术自动降级（直至可用）
    static GIConfig Degrade(const GIConfig& c, u32 pipelineCaps, bool rtSupported) {
        GIConfig out = c;
        for (int i = 0; i < 4 && !IsAvailable(out.diffuse, pipelineCaps, rtSupported); ++i)
            out.diffuse = FallbackOf(out.diffuse);
        for (int i = 0; i < 4 && !IsAvailable(out.shadow, pipelineCaps, rtSupported); ++i)
            out.shadow = FallbackOf(out.shadow);
        for (int i = 0; i < 4 && !IsAvailable(out.ao, pipelineCaps, rtSupported); ++i)
            out.ao = FallbackOf(out.ao);
        for (int i = 0; i < 4 && !IsAvailable(out.specular, pipelineCaps, rtSupported); ++i)
            out.specular = FallbackOf(out.specular);
        // DDGI 仅 Deferred/HybridRT 提供，管线不支持时关闭叠加
        if ((pipelineCaps & kPipelineGIDiffDDGI) == 0) {
            out.ddgiOverlay = false;
        }
        return out;
    }
};

} // namespace he::render
