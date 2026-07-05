#pragma once

#include "RHI/RHI.h"
#include "Math/Math.h"
#include <memory>

namespace he::render {

// ============================================================
// Denoiser — 5×5 双边模糊降噪（SSGI/SSR 共用）
// ============================================================
class Denoiser {
public:
    bool Initialize(rhi::IRHIDevice* device, u32 width, u32 height);
    void Shutdown();
    void OnResize(u32 w, u32 h);

    void SetInputs(rhi::IRHITexture* color, rhi::IRHITexture* depth, rhi::IRHITexture* normal);
    void Render(rhi::IRHICommandList* cmd);

    rhi::IRHITexture* GetOutput() const { return m_Denoised.get(); }
    void PreBind(rhi::IRHICommandList* cmd) const { if(m_Ready) cmd->SetPipeline(m_PSO.get()); }

private:
    rhi::IRHIDevice* m_Device = nullptr;
    u32 m_Width=0, m_Height=0; bool m_Ready=false;
    std::unique_ptr<rhi::IRHIPipelineState> m_PSO;
    std::unique_ptr<rhi::IRHITexture> m_Denoised;
    std::unique_ptr<rhi::IRHISampler> m_Sampler;
    rhi::DescriptorSetLayoutHandle m_Layout = rhi::kInvalidLayout;
    rhi::DescriptorSetHandle       m_Set    = rhi::kInvalidSet;
    rhi::IRHITexture* m_Input=nullptr,*m_Depth=nullptr,*m_Normal=nullptr;
};

} // namespace he::render
