#pragma once

#include "GI/GlobalIllumination.h"
#include "RHI/RHI.h"
#include "Math/Math.h"
#include <memory>

namespace he::render {

// ============================================================
// GI_SSR — 屏幕空间反射
//
// 每像素沿反射向量在深度缓冲中 Ray Marching，
// 命中的像素 albedo 作为间接镜面反射贡献。
// ============================================================
class GI_SSR : public IGlobalIllumination {
public:
    GI_SSR() = default;

    bool Initialize(rhi::IRHIDevice* device, u32 width, u32 height) override;
    void Shutdown() override;
    void Update(const SubsystemContext&) override {}
    void Render(rhi::IRHICommandList* cmd) override;
    void Bind(rhi::IRHICommandList*) const override {}
    void OnResize(u32 w, u32 h) override;
    const char* GetName() const override { return "GI_SSR"; }
    bool IsReady() const override { return m_Ready; }
    bool IsEnabled() const override { return m_Settings.enabled; }
    void SetEnabled(bool e) override { m_Settings.enabled = e; }

    GIMode GetMode() const override { return GIMode::SSGI; }  // 暂用 SSGI mode
    rhi::IRHITexture* GetIndirectSpecularTexture() const override { return m_Output.get(); }

    void SetInputs(rhi::IRHITexture* depth, rhi::IRHITexture* normal, rhi::IRHITexture* albedo);
    rhi::IRHISampler* GetOutputSampler() const { return m_Sampler.get(); }
    void PreBind(rhi::IRHICommandList* cmd) const { if (m_Ready) cmd->SetPipeline(m_PSO.get()); }

    float maxSteps   = 64;
    float stepSize   = 0.5f;
    float maxDistance = 50.0f;
    float thickness   = 0.1f;

private:
    void CreateOutputTex(u32 w, u32 h);

    rhi::IRHIDevice* m_Device = nullptr;
    u32 m_Width = 0, m_Height = 0;
    bool m_Ready = false;

    std::unique_ptr<rhi::IRHIPipelineState> m_PSO;
    rhi::DescriptorSetLayoutHandle m_DescLayout = rhi::kInvalidLayout;
    rhi::DescriptorSetHandle       m_DescSet    = rhi::kInvalidSet;

    std::unique_ptr<rhi::IRHITexture> m_Output;
    std::unique_ptr<rhi::IRHISampler> m_Sampler;
    std::unique_ptr<rhi::IRHISampler> m_PointSampler;

    rhi::IRHITexture* m_Depth  = nullptr;
    rhi::IRHITexture* m_Albedo = nullptr;
    rhi::IRHITexture* m_Normal = nullptr;
};

} // namespace he::render
