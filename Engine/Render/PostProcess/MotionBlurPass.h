#pragma once

#include "RHI/RHI.h"
#include <memory>

namespace he::render {

// ============================================================
// MotionBlurPass — 速度向量方向采样运动模糊
// ============================================================
class MotionBlurPass {
public:
    bool Initialize(rhi::IRHIDevice* device, u32 width, u32 height);
    void Shutdown();
    void OnResize(u32 w, u32 h);

    void SetInputs(rhi::IRHITexture* hdr, rhi::IRHISampler* hdrSampler,
                   rhi::IRHITexture* velocity, rhi::IRHISampler* velSampler);
    void Render(rhi::IRHICommandList* cmd);

    rhi::IRHITexture* GetOutput()        const { return m_Output.get(); }
    rhi::IRHISampler* GetOutputSampler() const { return m_OutSampler.get(); }
    bool IsEnabled()                     const { return m_Enabled; }
    void SetEnabled(bool e)              { m_Enabled = e; if (e && !m_Ready) EnsureInitialized(); }
    void SetIntensity(float i)           { m_Intensity = i; }
    float GetIntensity() const           { return m_Intensity; }

private:
    void EnsureInitialized();

    rhi::IRHIDevice* m_Device  = nullptr;
    u32 m_Width  = 0, m_Height = 0;
    bool m_Ready = false, m_Enabled = false;
    float m_Intensity = 0.5f;

    std::unique_ptr<rhi::IRHIPipelineState> m_PSO;
    std::unique_ptr<rhi::IRHITexture> m_Output;
    std::unique_ptr<rhi::IRHISampler> m_OutSampler;
    rhi::DescriptorSetLayoutHandle m_Layout = rhi::kInvalidLayout;
    rhi::DescriptorSetHandle       m_Set    = rhi::kInvalidSet;

    rhi::IRHITexture* m_HDRInput     = nullptr;
    rhi::IRHISampler* m_HDRSampler   = nullptr;
    rhi::IRHITexture* m_VelInput     = nullptr;
    rhi::IRHISampler* m_VelSampler   = nullptr;
};

} // namespace he::render
