#pragma once

#include "RHI/RHI.h"
#include "Math/Math.h"
#include <memory>

namespace he::render {

// ============================================================
// ColorGradingPass — LDR 色彩分级（ToneMap 之后、FXAA 之前）
// ============================================================
class ColorGradingPass {
public:
    bool Initialize(rhi::IRHIDevice* device, u32 width, u32 height);
    void Shutdown();
    void OnResize(u32 w, u32 h);

    void SetInput(rhi::IRHITexture* color, rhi::IRHISampler* sampler);
    void Render(rhi::IRHICommandList* cmd);

    rhi::IRHITexture* GetOutput() const { return m_Output.get(); }
    rhi::IRHISampler* GetOutputSampler() const { return m_OutSampler.get(); }
    void PreBind(rhi::IRHICommandList* cmd) const { if (m_Ready) cmd->SetPipeline(m_PSO.get()); }
    bool IsEnabled() const { return m_Enabled; }
    void SetEnabled(bool e) { m_Enabled = e; if (e && !m_Ready) EnsureInitialized(); }

    // 参数
    float GetSaturation() const { return m_Saturation; }
    void  SetSaturation(float s) { m_Saturation = s; }
    float GetContrast() const { return m_Contrast; }
    void  SetContrast(float c) { m_Contrast = c; }
    float GetVibrance() const { return m_Vibrance; }
    void  SetVibrance(float v) { m_Vibrance = v; }

private:
    void EnsureInitialized() { if (!m_Ready && m_Device) Initialize(m_Device, m_Width, m_Height); }

    rhi::IRHIDevice* m_Device = nullptr;
    u32 m_Width = 0, m_Height = 0;
    bool m_Ready = false, m_Enabled = false;
    float m_Saturation = 1.0f;
    float m_Contrast   = 1.0f;
    float m_Vibrance   = 0.0f;

    std::unique_ptr<rhi::IRHIPipelineState> m_PSO;
    std::unique_ptr<rhi::IRHITexture> m_Output;
    std::unique_ptr<rhi::IRHISampler> m_OutSampler;
    rhi::DescriptorSetLayoutHandle m_Layout = rhi::kInvalidLayout;
    rhi::DescriptorSetHandle       m_Set    = rhi::kInvalidSet;
    rhi::IRHITexture* m_Input = nullptr;
    rhi::IRHISampler* m_InputSampler = nullptr;
};

} // namespace he::render
