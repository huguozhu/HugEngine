#pragma once

// ============================================================
// GI/GIRegistry.h — GI Provider 注册表 + 自动降级（M3）
//
// 提供：
//   1. IsAvailable：查询某通道技术在当前设备/管线是否可用
//      （MVP：RT 系列需硬件光追；Deferred 光栅管线不提供 → 不可用）
//   2. FallbackOf：降级链（RTGI→DDGI、RTAO→SSAO、RT→SSR、RT 阴影→CSM）
//   3. Degrade：把 GIConfig 中不可用的通道技术自动降级到可用组合
// ============================================================

#include "Pipeline/LightingPass.h"
#include "GI/GIConfig.h"

namespace he::render {

class GIRegistry {
public:
    /// 各通道技术是否可用（MVP：RT 系列需硬件光追，Deferred 光栅管线不提供）
    static bool IsAvailable(ShadowChannel s)   { return s != ShadowChannel::RT; }
    static bool IsAvailable(AOChannel s)       { return s != AOChannel::RTAO; }
    static bool IsAvailable(SpecularChannel s) { return s != SpecularChannel::RT; }
    static bool IsAvailable(DiffuseChannel s)  { return s != DiffuseChannel::RTGI; }

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
        return s == DiffuseChannel::RTGI ? DiffuseChannel::SSGI : s;   // RTGI→SSGI（DDGI 由 ddgiOverlay 独立叠加）
    }

    /// 把 GIConfig 中不可用的通道技术自动降级（直至可用）
    static GIConfig Degrade(const GIConfig& c) {
        GIConfig out = c;
        for (int i = 0; i < 4 && !IsAvailable(out.diffuse); ++i)
            out.diffuse = FallbackOf(out.diffuse);
        for (int i = 0; i < 4 && !IsAvailable(out.shadow); ++i)
            out.shadow = FallbackOf(out.shadow);
        for (int i = 0; i < 4 && !IsAvailable(out.ao); ++i)
            out.ao = FallbackOf(out.ao);
        for (int i = 0; i < 4 && !IsAvailable(out.specular); ++i)
            out.specular = FallbackOf(out.specular);
        return out;
    }
};

} // namespace he::render
