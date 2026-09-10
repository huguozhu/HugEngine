// ============================================================
// GI/GIConfig.cpp — 质量档位预设（M2 → P3 层栈形态）
//
// 每档位 = 一组「源层栈」配置 + 精度参数；融合方式（归一化）不随档位变化。
// ============================================================

#include "GI/GIConfig.h"

namespace he::render {

GIConfig GIConfigFromPreset(GIQualityPreset p) {
    GIConfig c;

    // ── 公共基线：低频环境源（所有档位的兜底）──
    c.diffuse.Set(GISourceId::IBL, 1.0f);              // 环境辐照度（远场兜底）
    c.specular.Set(GISourceId::IBL, 1.0f);             // 环境镜面（预滤波）
    c.ao.Set(GISourceId::SSAO, 1.0f);
    c.shadow.Set(GISourceId::RasterShadow, 1.0f);

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
        c.shadow.Set(GISourceId::RTShadow, 1.0f);
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

} // namespace he::render
