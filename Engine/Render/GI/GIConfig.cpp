// ============================================================
// GI/GIConfig.cpp — 质量档位预设（M2）
// ============================================================

#include "GI/GIConfig.h"

namespace he::render {

GIConfig GIConfigFromPreset(GIQualityPreset p) {
    GIConfig c;
    switch (p) {
    case GIQualityPreset::Low:
        c.diffuse  = LightingSource::Diffuse_SSGI;
        c.useDDGI  = false;
        c.halfRes  = true;   // 半分辨率 SSGI（性能优先）
        c.giIntensity = 0.6f;
        break;
    case GIQualityPreset::High:
        c.diffuse  = LightingSource::Diffuse_SSGI;
        c.useDDGI  = true;
        c.halfRes  = false;
        c.giIntensity = 1.0f;
        break;
    case GIQualityPreset::Ultra:
        c.diffuse  = LightingSource::Diffuse_SSGI;
        c.useDDGI  = true;
        c.halfRes  = false;
        c.giIntensity = 1.2f;
        break;
    case GIQualityPreset::Medium:
    default:
        c.diffuse  = LightingSource::Diffuse_SSGI;
        c.useDDGI  = true;
        c.halfRes  = false;
        c.giIntensity = 0.8f;
        break;
    }
    return c;
}

} // namespace he::render
