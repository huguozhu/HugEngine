#pragma once

#include "RHI/RHI.h"
#include "Math/Math.h"
#include <memory>

namespace he::render {

// ============================================================
// AutoExposurePass — HDR 自动曝光
// Compute 降采样 256 个 partial sum → CPU 平均 + 时间混合
// ============================================================
class AutoExposurePass {
public:
    bool Initialize(rhi::IRHIDevice* device, u32 width, u32 height);
    void Shutdown();
    void OnResize(u32 w, u32 h);

    void SetInput(rhi::IRHITexture* hdr, rhi::IRHISampler* sampler);
    void Render(rhi::IRHICommandList* cmd);

    float GetExposure() const { return m_Exposure; }
    bool  IsEnabled()     const { return m_Enabled; }
    void  SetEnabled(bool e)   { m_Enabled = e; }
    float GetAdaptSpeed() const { return m_AdaptSpeed; }
    void  SetAdaptSpeed(float s) { m_AdaptSpeed = s; }
    float GetTargetLum()  const { return m_TargetLum; }
    void  SetTargetLum(float t) { m_TargetLum = t; }

private:
    static constexpr u32 kNumGroups = 256;  // 16×16 组

    rhi::IRHIDevice* m_Device = nullptr;
    u32 m_Width = 0, m_Height = 0;
    bool m_Ready  = false;
    bool m_Enabled = true;
    float m_Exposure   = 1.0f;
    float m_AdaptSpeed = 2.0f;
    float m_TargetLum  = 0.18f;
    float m_PrevLogLum = -2.47f;  // 历史平均对数亮度

    rhi::ShaderBytecode m_CS;
    std::unique_ptr<rhi::IRHIPipelineState> m_PSO;
    rhi::DescriptorSetLayoutHandle m_Layout = rhi::kInvalidLayout;
    rhi::DescriptorSetHandle       m_Set    = rhi::kInvalidSet;
    std::unique_ptr<rhi::IRHIBuffer> m_ResultBuf;  // 256 floats

    rhi::IRHITexture* m_HDRInput   = nullptr;
    rhi::IRHISampler* m_HDRSampler = nullptr;
};

} // namespace he::render
