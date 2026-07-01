#pragma once

#include "Subsystem/RenderSubsystem.h"

namespace rhi {
    class IRHITexture;
}

namespace he::render {

// ============================================================================
// GI 模式枚举 — 覆盖长期路线图中所有技术
// ============================================================================
enum class GIMode : u8 {
    None    = 0,   // 无 GI（仅直接光照）
    IBL     = 1,   // 基于图像的光照（环境贴图 → 辐照度 + 预滤波 + BRDF LUT）
    SSGI    = 2,   // 屏幕空间 GI（深度缓冲 Ray Marching）
    VXGI    = 3,   // 体素锥追踪 GI（3D Clipmap → Cone Tracing）
    DDGI    = 4,   // 动态漫反射 GI（探针网格 + SH 辐照度）
    ReSTIR  = 5,   // 基于重采样的 GI（时空重要性重采样 + 硬件光追）
};

// ============================================================================
// GI 质量等级 — 设备分级渲染
// ============================================================================
enum class GIQuality : u8 {
    Low     = 0,   // 移动端 / 入门级（半分辨率、低采样数）
    Medium  = 1,   // 中端 PC（标准分辨率、中等采样数）
    High    = 2,   // 高端 PC / 主机（全分辨率、高采样数）
    Ultra   = 3,   // 参考级（离线品质）
};

// ============================================================================
// GISettings — 所有 GI 技术共享的公共参数
// ============================================================================
struct GISettings {
    GIMode     mode          = GIMode::None;
    GIQuality  quality       = GIQuality::Medium;
    bool       enabled       = true;
    float      intensity     = 1.0f;    // GI 贡献强度倍率
    u32        maxBounces    = 2;       // 最大间接反弹次数（受具体技术限制）
    bool       halfRes       = false;   // 半分辨率计算（性能优先）
    bool       debugView     = false;   // 调试可视化模式
};

// ============================================================================
// GIDebugData — 每帧运行时统计（供 ImGui 面板展示）
// ============================================================================
struct GIDebugData {
    float  avgRenderTimeMs  = 0.0f;  // 最近 N 帧平均耗时
    float  peakRenderTimeMs = 0.0f;  // 峰值耗时
    u32    probeCount       = 0;     // 探针 / 体素 / 样本总数
    u32    activeRays       = 0;     // 活跃光线 / 锥数
    u32    bounceCount       = 0;     // 实际反弹次数
    float  memoryMB         = 0.0f;  // GPU 总内存占用（MB）
    bool   needsRecompute   = false; // 脏标记：是否需要重新计算
};

// ============================================================================
// IGlobalIllumination — GI 技术公共接口
//
// 继承 IRenderSubsystem，在此之上扩展 GI 特有功能：
//   - 模式 / 质量运行时切换
//   - 间接光照纹理暴露（供 PBR Shader 采样）
//   - 调试统计
//
// 继承树：
//   IRenderSubsystem
//     └── IGlobalIllumination
//           ├── GI_None     空实现（Null Object，默认关闭）
//           ├── GI_IBL      环境贴图 (Phase A)
//           ├── GI_SSGI     屏幕空间 (Phase B1)
//           ├── GI_VXGI     体素锥追踪 (Phase B2)
//           ├── GI_DDGI     探针网格 (Phase C1)
//           └── GI_ReSTIR   时空重采样 (Phase C2)
// ============================================================================
class IGlobalIllumination : public IRenderSubsystem {
public:
    IGlobalIllumination() = default;
    virtual ~IGlobalIllumination() = default;

    // ---- 模式 / 质量 ----

    [[nodiscard]] virtual GIMode GetMode() const = 0;
    [[nodiscard]] virtual GIQuality GetQuality() const { return m_Settings.quality; }
    virtual void SetQuality(GIQuality quality) { m_Settings.quality = quality; }

    // ---- 配置 ----

    [[nodiscard]] const GISettings& GetSettings() const { return m_Settings; }
    void SetSettings(const GISettings& s) { m_Settings = s; }

    // ---- 调试 ----

    [[nodiscard]] virtual GIDebugData GetDebugData() const { return m_DebugData; }

    // ---- 可选：间接光照纹理访问 ----

    /// 间接漫反射贴图（如辐照度图）。
    /// 返回 nullptr 表示该技术不暴露独立纹理。
    [[nodiscard]] virtual rhi::IRHITexture* GetIndirectDiffuseTexture() const { return nullptr; }

    /// 间接镜面反射贴图（如预滤波环境图）。
    [[nodiscard]] virtual rhi::IRHITexture* GetIndirectSpecularTexture() const { return nullptr; }

protected:
    GISettings   m_Settings;     // 公共配置
    GIDebugData  m_DebugData;    // 每帧调试统计
};

} // namespace he::render
