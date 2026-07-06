#pragma once

#include "RHI/RHI.h"
#include "PostProcess/GaussianBlurPass.h"
#include <memory>

namespace he::render {

// ============================================================
// DOFPass — 景深后处理（CoC + GaussianBlur + Composite）
// ============================================================
class DOFPass {
public:
    bool Initialize(rhi::IRHIDevice* device, u32 width, u32 height);
    void Shutdown();
    void OnResize(u32 w, u32 h);

    /// 设置输入（每帧调用）
    void SetInputs(rhi::IRHITexture* hdr, rhi::IRHISampler* hdrSampler,
                   rhi::IRHITexture* depth);
    /// 执行完整 DOF 管线
    void Render(rhi::IRHICommandList* cmd);

    rhi::IRHITexture* GetOutput()        const { return m_Output.get(); }
    rhi::IRHISampler* GetOutputSampler() const { return m_OutSampler.get(); }
    bool IsEnabled()                     const { return m_Enabled; }
    void SetEnabled(bool e)              { m_Enabled = e; if (e && !m_Ready) EnsureInitialized(); }
    void SetFocusDepth(float d)          { m_FocusDepth = d; }
    void SetFocusRange(float r)          { m_FocusRange = r; }
    void SetIntensity(float i)           { m_Intensity = i; }
    float GetFocusDepth() const          { return m_FocusDepth; }
    float GetFocusRange() const          { return m_FocusRange; }
    float GetIntensity()  const          { return m_Intensity; }

private:
    void EnsureInitialized();

    rhi::IRHIDevice* m_Device  = nullptr;
    u32 m_Width  = 0, m_Height = 0;
    bool m_Ready = false, m_Enabled = false;
    float m_FocusDepth = 0.5f;
    float m_FocusRange = 0.1f;
    float m_Intensity  = 1.0f;

    // CoC Pass
    std::unique_ptr<rhi::IRHIPipelineState> m_CoCPSO;
    std::unique_ptr<rhi::IRHITexture> m_CoCTex;
    rhi::DescriptorSetLayoutHandle m_CoCLayout = rhi::kInvalidLayout;
    rhi::DescriptorSetHandle       m_CoCSet    = rhi::kInvalidSet;

    // 模糊（复用 GaussianBlurPass，固定半径）
    GaussianBlurPass m_Blur;

    // Composite
    std::unique_ptr<rhi::IRHIPipelineState> m_CompositePSO;
    std::unique_ptr<rhi::IRHITexture> m_Output;
    std::unique_ptr<rhi::IRHISampler> m_OutSampler;
    rhi::DescriptorSetLayoutHandle m_CompositeLayout = rhi::kInvalidLayout;
    rhi::DescriptorSetHandle       m_CompositeSet    = rhi::kInvalidSet;

    rhi::IRHITexture* m_HDRInput   = nullptr;
    rhi::IRHISampler* m_HDRSampler = nullptr;
    rhi::IRHITexture* m_DepthInput = nullptr;
};

} // namespace he::render
