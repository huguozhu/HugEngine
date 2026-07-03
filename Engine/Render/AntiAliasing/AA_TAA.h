#pragma once

#include "AntiAliasing.h"
#include "RHI/RHI.h"
#include <memory>

namespace he::render {

// ============================================================================
// AA_TAA — 时域抗锯齿（Temporal Anti-Aliasing）
//
// 依赖 Velocity Buffer（GBuffer MRT3）做精确重投影。
// 仅支持延迟渲染（SupportsForward=false）。
//
// 管线位置：Lighting(HDR) → TAA_Resolve → ToneMap → BackBuffer
// 自拥有输出：double-buffered history（OwnsOutput=true）
// ============================================================================
class AA_TAA final : public IAntiAliasing {
    HE_DECLARE_NON_COPYABLE(AA_TAA);

public:
    AA_TAA()  = default;
    ~AA_TAA() override = default;

    // ── IRenderSubsystem ──
    bool Initialize(rhi::IRHIDevice* device, u32 width, u32 height) override;
    void Shutdown() override;
    void Update(const SubsystemContext&) override {}
    void Render(rhi::IRHICommandList* cmd) override;
    void Bind(rhi::IRHICommandList* cmd) const override {}
    void OnResize(u32 width, u32 height) override;

    [[nodiscard]] const char* GetName()  const override { return "AA_TAA"; }
    [[nodiscard]] bool        IsReady()  const override { return m_Ready; }
    [[nodiscard]] bool        IsEnabled() const override { return m_Enabled; }
    void SetEnabled(bool e) override { m_Enabled = e; }

    // ── IPostProcessPass ──

    /// 绑定当前帧 HDR 颜色输入
    void SetInput(rhi::IRHITexture* color, rhi::IRHISampler* sampler) override;

    /// TAA 输出 HDR 格式（ToneMap 前）
    [[nodiscard]] rhi::Format GetOutputFormat() const override { return rhi::Format::RGBA16_FLOAT; }

    /// TAA 自拥有历史缓冲
    [[nodiscard]] bool OwnsOutput() const override { return true; }
    [[nodiscard]] rhi::IRHITexture* GetOutputTexture() const override;
    [[nodiscard]] rhi::IRHISampler* GetOutputSampler() const override;

    // ── IAntiAliasing ──

    [[nodiscard]] AAMode GetMode() const override { return AAMode::TAA; }

    /// 当前帧 Halton 抖动偏移（NDC 空间，调用方写入投影矩阵）
    [[nodiscard]] float2 GetJitterOffset() const override;

    /// 推进抖动序列 + 交换历史 read/write index
    void OnBeginFrame() override;

    [[nodiscard]] bool SupportsForward()  const override { return false; }
    [[nodiscard]] bool SupportsDeferred() const override { return true; }

    // ── TAA 特有：绑定 GBuffer 辅助输入 ──

    /// 设置 GBuffer 深度 / 法线 / velocity 纹理（Render 前调用）
    /// @param depth     GBuffer Depth (D32_FLOAT)
    /// @param normal    GBuffer B (normal*0.5+0.5 + roughness)
    /// @param velocity  GBuffer D (RG16_FLOAT, UV 空间运动矢量)
    void SetGBufferInputs(rhi::IRHITexture* depth,
                          rhi::IRHITexture* normal,
                          rhi::IRHITexture* velocity);

    // ── Uniform 更新（管线每帧调用） ──

    /// 更新 TAA uniform buffer（prevViewProj、invCurrViewProj、resolution）
    /// @param prevViewProj    上一帧 ViewProj 矩阵
    /// @param invCurrViewProj 当前帧 InvViewProj（用于深度反算世界坐标）
    /// @param width, height   视口尺寸
    void UpdateUniforms(const float4x4& prevViewProj,
                        const float4x4& invCurrViewProj,
                        u32 width, u32 height);

private:
    void CreateHistoryTextures(u32 w, u32 h);
    void CreatePSO();
    [[nodiscard]] static float2 HaltonSample(u32 index);

    rhi::IRHIDevice* m_Device = nullptr;
    u32 m_Width = 0, m_Height = 0;
    bool m_Ready = false;

    // PSO + 描述符
    std::unique_ptr<rhi::IRHIPipelineState> m_PSO;
    rhi::DescriptorSetLayoutHandle m_DescLayout = rhi::kInvalidLayout;
    rhi::DescriptorSetHandle       m_DescSet    = rhi::kInvalidSet;
    std::unique_ptr<rhi::IRHIBuffer> m_UniformBuffer;

    // 输入纹理（外部注入，不持有所有权）
    rhi::IRHITexture* m_InputColor      = nullptr;
    rhi::IRHISampler* m_InputSampler    = nullptr;
    rhi::IRHITexture* m_DepthTexture    = nullptr;
    rhi::IRHITexture* m_NormalTexture   = nullptr;
    rhi::IRHITexture* m_VelocityTexture = nullptr;

    // 历史缓冲（double-buffered，自拥有）
    std::unique_ptr<rhi::IRHITexture> m_HistoryColor[2];
    std::unique_ptr<rhi::IRHISampler> m_HistorySampler;
    u32 m_HistoryRead  = 0;
    u32 m_HistoryWrite = 1;

    // 抖动序列
    u32 m_FrameIndex   = 0;
    u32 m_JitterIndex  = 0;
    float2 m_CurrentJitter = {0.0f, 0.0f};

    // 前一帧 VP 矩阵（TAA 内部缓存，用于 uniform buffer）
    float4x4 m_PrevViewProj = float4x4(1.0f);
};

} // namespace he::render
