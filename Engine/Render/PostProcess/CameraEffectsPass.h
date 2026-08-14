#pragma once

#include "RHI/RHI.h"
#include "Math/Math.h"
#include "PostProcess/LazyPostProcessPass.h"
#include <memory>

namespace he::render {

// ============================================================
// CameraEffectsPass — LDR 镜头后处理（胶片颗粒/晕影/色差/镜头畸变）
// ToneMap/ColorGrading 之后、AA 之前执行
// ============================================================
class CameraEffectsPass : public LazyPostProcessPass {
public:
    bool Initialize(rhi::IRHIDevice* device, u32 width, u32 height) override;
    void Shutdown() override;
    void OnResize(u32 w, u32 h) override;

    void SetInput(rhi::IRHITexture* color, rhi::IRHISampler* sampler);
    void Render(rhi::IRHICommandList* cmd);

    rhi::IRHITexture* GetOutput() const { return m_Output.get(); }
    rhi::IRHISampler* GetOutputSampler() const { return m_OutSampler.get(); }
    void PreBind(rhi::IRHICommandList* cmd) const { if (m_Ready) cmd->SetPipeline(m_PSO.get()); }

    // 参数（0=关闭对应效果）
    float GetFilmGrain()           const { return m_FilmGrain; }
    void  SetFilmGrain(float g)          { m_FilmGrain = g; }
    float GetVignette()            const { return m_Vignette; }
    void  SetVignette(float v)           { m_Vignette = v; }
    float GetChromaticAberration() const { return m_CA; }
    void  SetChromaticAberration(float c){ m_CA = c; }
    float GetLensDistortion()      const { return m_Distortion; }
    void  SetLensDistortion(float d)     { m_Distortion = d; }

private:
    float m_FilmGrain = 0.0f;
    float m_Vignette  = 0.0f;
    float m_CA        = 0.0f;
    float m_Distortion = 0.0f;
    float m_Time = 0.0f;  // 累计时间（颗粒动画）

    std::unique_ptr<rhi::IRHIPipelineState> m_PSO;
    std::unique_ptr<rhi::IRHITexture> m_Output;
    std::unique_ptr<rhi::IRHISampler> m_OutSampler;
    rhi::DescriptorSetLayoutHandle m_Layout = rhi::kInvalidLayout;
    rhi::DescriptorSetHandle       m_Set    = rhi::kInvalidSet;
    rhi::IRHITexture* m_Input = nullptr;
    rhi::IRHISampler* m_InputSampler = nullptr;
};

} // namespace he::render
