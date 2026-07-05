#pragma once

#include "AntiAliasing.h"
#include "RHI/RHI.h"
#include <memory>

namespace he::render {

// ============================================================
// AA_FXAA — 快速近似抗锯齿
//
// 单 Pass LDR 后处理，无需 GBuffer 或历史帧数据，
// 同时支持前向和延迟渲染。管线位置：ToneMap → FXAA → Present
// ============================================================
class AA_FXAA final : public IAntiAliasing {
    HE_DECLARE_NON_COPYABLE(AA_FXAA);

public:
    AA_FXAA()  = default;
    ~AA_FXAA() override = default;

    bool Initialize(rhi::IRHIDevice* device, u32 width, u32 height) override;
    void Shutdown() override;
    void Update(const SubsystemContext&) override {}
    void Render(rhi::IRHICommandList* cmd) override;
    void Bind(rhi::IRHICommandList* cmd) const override {}
    void OnResize(u32 width, u32 height) override;

    [[nodiscard]] const char* GetName()  const override { return "AA_FXAA"; }
    [[nodiscard]] bool        IsReady()  const override { return m_Ready; }
    [[nodiscard]] bool        IsEnabled() const override { return m_Enabled; }
    void SetEnabled(bool e) override { m_Enabled = e; }

    // ── IPostProcessPass ──
    void SetInput(rhi::IRHITexture* color, rhi::IRHISampler* sampler) override;
    [[nodiscard]] rhi::Format GetOutputFormat() const override { return rhi::Format::BGRA8_UNORM; }
    [[nodiscard]] bool OwnsOutput() const override { return false; }
    [[nodiscard]] rhi::IRHITexture* GetOutputTexture() const override { return m_Input; }
    [[nodiscard]] rhi::IRHISampler* GetOutputSampler() const override { return m_InputSampler; }

    // ── IAntiAliasing ──
    [[nodiscard]] AAMode GetMode() const override { return AAMode::FXAA; }

private:
    void CreatePSO();

    rhi::IRHIDevice* m_Device = nullptr;
    u32 m_Width = 0, m_Height = 0;
    bool m_Ready = false, m_Enabled = true;

    std::unique_ptr<rhi::IRHIPipelineState> m_PSO;
    rhi::DescriptorSetLayoutHandle m_DescLayout = rhi::kInvalidLayout;
    rhi::DescriptorSetHandle       m_DescSet    = rhi::kInvalidSet;

    rhi::IRHITexture* m_Input        = nullptr;
    rhi::IRHISampler* m_InputSampler = nullptr;
    std::unique_ptr<rhi::IRHITexture> m_Placeholder;
    std::unique_ptr<rhi::IRHISampler> m_PlaceholderSamp;
};

} // namespace he::render
