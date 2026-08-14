#pragma once

#include "RHI/RHI.h"
#include "PostProcess/LazyPostProcessPass.h"
#include <memory>

namespace he::render {

// ============================================================
// MotionBlurPass — 速度向量方向采样运动模糊
// ============================================================
class MotionBlurPass : public LazyPostProcessPass {
public:
    static constexpr u32 kDefaultSamples = 12;  // 方向采样数
    bool Initialize(rhi::IRHIDevice* device, u32 width, u32 height) override;
    void Shutdown() override;
    void OnResize(u32 w, u32 h) override;

    void SetInputs(rhi::IRHITexture* hdr, rhi::IRHISampler* hdrSampler,
                   rhi::IRHITexture* velocity, rhi::IRHISampler* velSampler);
    void Render(rhi::IRHICommandList* cmd);

    rhi::IRHITexture* GetOutput()        const { return m_Output.get(); }
    rhi::IRHISampler* GetOutputSampler() const { return m_OutSampler.get(); }
    void SetIntensity(float i)           { m_Intensity = i; }
    float GetIntensity() const           { return m_Intensity; }

private:
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
