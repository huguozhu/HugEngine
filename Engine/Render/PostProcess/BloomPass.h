#pragma once

#include "RHI/RHI.h"
#include "PostProcess/GaussianBlurPass.h"
#include <memory>

namespace he::render {

// ============================================================
// BloomPass — 完整 Bloom 后处理
//
// 管线：HDR → BrightPass(阈值+半分辨率) → GaussianBlur → Composite(上采样+叠加)
// 输出全分辨率 HDR+Bloom 纹理，替代原始 HDR 送入 TAA/ToneMap
// ============================================================
class BloomPass {
public:
    bool Initialize(rhi::IRHIDevice* device, u32 width, u32 height);
    void Shutdown();
    void OnResize(u32 w, u32 h);

    /// 设置 HDR 输入（每帧调用）
    void SetInput(rhi::IRHITexture* hdr, rhi::IRHISampler* sampler);
    /// 执行完整 Bloom 管线（内部 3 个 Pass），输出到自有纹理
    void Render(rhi::IRHICommandList* cmd);

    rhi::IRHITexture* GetOutput()        const { return m_Output.get(); }
    rhi::IRHISampler* GetOutputSampler() const { return m_OutSampler.get(); }
    bool IsEnabled()                     const { return m_Enabled; }
    /// 启用 Bloom（首次调用时触发懒初始化）
    void SetEnabled(bool e) {
        m_Enabled = e;
        if (e && !m_Ready) { EnsureInitialized(); }
    }
    float GetThreshold()                  const { return m_Threshold; }
    void  SetThreshold(float t)                 { m_Threshold = t; }
    float GetIntensity()                  const { return m_Intensity; }
    void  SetIntensity(float i)                 { m_Intensity = i; }

private:
    void EnsureInitialized();  // 懒初始化：首次 SetEnabled(true) 或 Render() 时调用

    rhi::IRHIDevice* m_Device  = nullptr;
    u32 m_Width  = 0;
    u32 m_Height = 0;
    bool m_Ready = false;
    bool m_Enabled = false;   // 默认关闭，SetEnabled(true) 触发懒初始化
    float m_Threshold = 3.0f;   // 亮度阈值
    float m_Intensity = 0.8f;   // Bloom 强度

    // BrightPass
    std::unique_ptr<rhi::IRHIPipelineState> m_BrightPSO;
    std::unique_ptr<rhi::IRHITexture> m_BrightTex;       // 半分辨率
    std::unique_ptr<rhi::IRHISampler> m_BrightSampler;
    rhi::DescriptorSetLayoutHandle m_BrightLayout = rhi::kInvalidLayout;
    rhi::DescriptorSetHandle       m_BrightSet    = rhi::kInvalidSet;

    // 高斯模糊（复用 GaussianBlurPass）
    GaussianBlurPass m_Blur;

    // Composite（上采样 + 叠加）
    std::unique_ptr<rhi::IRHIPipelineState> m_CompositePSO;
    std::unique_ptr<rhi::IRHITexture> m_Output;          // 全分辨率 HDR+Bloom
    std::unique_ptr<rhi::IRHISampler> m_OutSampler;
    rhi::DescriptorSetLayoutHandle m_CompositeLayout = rhi::kInvalidLayout;
    rhi::DescriptorSetHandle       m_CompositeSet    = rhi::kInvalidSet;

    rhi::IRHITexture* m_HDRInput    = nullptr;
    rhi::IRHISampler* m_HDRSampler  = nullptr;
};

} // namespace he::render
