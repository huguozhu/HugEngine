#pragma once

#include "Threading/RenderThreadContext.h"   // T2.6：帧上下文（唯一 RHI 出口）

#include "RHI/RHI.h"
#include "PostProcess/LazyPostProcessPass.h"
#include <memory>

namespace he::render {

// ============================================================
// MotionBlurPass — 速度向量方向采样运动模糊
// ============================================================
class MotionBlurPass : public LazyPostProcessPass {

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

    rhi::RHIPipelineStateHandle m_PSO{};   // A-3：句柄持有
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
