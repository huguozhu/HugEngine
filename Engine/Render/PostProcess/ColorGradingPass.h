#pragma once

#include "Threading/RenderThreadContext.h"   // T2.6：帧上下文（唯一 RHI 出口）

#include "RHI/RHI.h"
#include "Math/Math.h"
#include "PostProcess/LazyPostProcessPass.h"
#include <memory>

namespace he::render {

// ============================================================
// ColorGradingPass — LDR 色彩分级（ToneMap 之后、FXAA 之前）
// ============================================================
class ColorGradingPass : public LazyPostProcessPass {

    // ── 【T2.6】帧上下文与统一解析入口（m_Device 继承自基类）──
    const RenderThreadContext* m_FrameCtx = nullptr;
    void SetFrameContext(const RenderThreadContext* ctx) { m_FrameCtx = ctx; }
    [[nodiscard]] rhi::IRHIBuffer* ResB(rhi::RHIBufferHandle h) const {
        const RenderThreadContext* c = m_FrameCtx ? m_FrameCtx : GetFrameContext();
        if (c) return c->ResolveBuffer(h);
        return m_Device ? m_Device->ResolveBuffer(h) : nullptr;
    }
    [[nodiscard]] rhi::IRHITexture* ResT(rhi::RHITextureHandle h) const {
        const RenderThreadContext* c = m_FrameCtx ? m_FrameCtx : GetFrameContext();
        if (c) return c->ResolveTexture(h);
        return m_Device ? m_Device->ResolveTexture(h) : nullptr;
    }
    [[nodiscard]] rhi::IRHIPipelineState* ResP(rhi::RHIPipelineStateHandle h) const {
        const RenderThreadContext* c = m_FrameCtx ? m_FrameCtx : GetFrameContext();
        if (c) return c->ResolvePipelineState(h);
        return m_Device ? m_Device->ResolvePipelineState(h) : nullptr;
    }
    [[nodiscard]] rhi::IRHISampler* ResS(rhi::RHISamplerHandle h) const {
        const RenderThreadContext* c = m_FrameCtx ? m_FrameCtx : GetFrameContext();
        if (c) return c->ResolveSampler(h);
        return m_Device ? m_Device->ResolveSampler(h) : nullptr;
    }

public:
    bool Initialize(rhi::IRHIDevice* device, u32 width, u32 height) override;
    void Shutdown() override;
    void OnResize(u32 w, u32 h) override;

    void SetInput(rhi::IRHITexture* color, rhi::IRHISampler* sampler);
    void Render(rhi::IRHICommandList* cmd);

    rhi::IRHITexture* GetOutput() const { return m_Output.get(); }
    rhi::IRHISampler* GetOutputSampler() const { return m_OutSampler.get(); }
    void PreBind(rhi::IRHICommandList* cmd) const { if (m_Ready) cmd->SetPipeline(m_Device->ResolvePipelineState(m_PSO)); }

    // 参数
    float GetSaturation() const { return m_Saturation; }
    void  SetSaturation(float s) { m_Saturation = s; }
    float GetContrast() const { return m_Contrast; }
    void  SetContrast(float c) { m_Contrast = c; }
    float GetVibrance() const { return m_Vibrance; }
    void  SetVibrance(float v) { m_Vibrance = v; }

private:
    float m_Saturation = 1.0f;
    float m_Contrast   = 1.0f;
    float m_Vibrance   = 0.0f;

    rhi::RHIPipelineStateHandle m_PSO{};   // A-3：句柄持有
    std::unique_ptr<rhi::IRHITexture> m_Output;
    std::unique_ptr<rhi::IRHISampler> m_OutSampler;
    rhi::DescriptorSetLayoutHandle m_Layout = rhi::kInvalidLayout;
    rhi::DescriptorSetHandle       m_Set    = rhi::kInvalidSet;
    rhi::IRHITexture* m_Input = nullptr;
    rhi::IRHISampler* m_InputSampler = nullptr;
};

} // namespace he::render
