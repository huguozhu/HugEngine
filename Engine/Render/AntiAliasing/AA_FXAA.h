#pragma once

#include "Threading/RenderThreadContext.h"   // T2.6：帧上下文（唯一 RHI 出口）

#include "AntiAliasing.h"
#include "RHI/RHI.h"
#include <memory>

namespace he::render {

// ============================================================
// AA_FXAA — 快速近似抗锯齿
//
// 单 Pass LDR 后处理，无需 GBuffer 或历史帧数据，
// 同时支持前向和延迟渲染。管线位置：ToneMap → FXAA → Present
// ============================================================
class AA_FXAA final : public IAntiAliasing {
    HE_DECLARE_NON_COPYABLE(AA_FXAA);

public:
    AA_FXAA()  = default;
    ~AA_FXAA() override = default;

    bool Initialize(rhi::IRHIDevice* device, u32 width, u32 height) override;
    void Shutdown() override;
    void Update(const SubsystemContext&) override {}
    void Render(rhi::IRHICommandList* cmd) override;
    void Bind(rhi::IRHICommandList* cmd) const override {}
    void OnResize(u32 width, u32 height) override;

    [[nodiscard]] const char* GetName()  const override { return "AA_FXAA"; }
    [[nodiscard]] bool        IsReady()  const override { return m_Ready; }
    [[nodiscard]] bool        IsEnabled() const override { return m_Enabled; }
    void SetEnabled(bool e) override { m_Enabled = e; }

    // ── IPostProcessPass ──
    void SetInput(rhi::IRHITexture* color, rhi::IRHISampler* sampler) override;
    [[nodiscard]] rhi::Format GetOutputFormat() const override { return rhi::Format::BGRA8_UNORM; }
    [[nodiscard]] bool OwnsOutput() const override { return false; }
    [[nodiscard]] rhi::IRHITexture* GetOutputTexture() const override { return m_Input; }
    [[nodiscard]] rhi::IRHISampler* GetOutputSampler() const override { return m_InputSampler; }

    // ── IAntiAliasing ──
    [[nodiscard]] AAMode GetMode() const override { return AAMode::FXAA; }

private:
    void CreatePSO();

    rhi::IRHIDevice* m_Device = nullptr;

    /// 【T2.6】本帧的渲染线程上下文（可空；空时解析回退 `m_Device`）
    const RenderThreadContext* m_FrameCtx = nullptr;

public:
    /// 【T2.6】设置本帧的渲染线程上下文（搬迁过渡期用）
    void SetFrameContext(const RenderThreadContext* ctx) { m_FrameCtx = ctx; }

private:
    // 【T2.6】统一解析入口（回退分支直接调 m_Device->Resolve*，避免被批量替换成递归）
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

    u32 m_Width = 0, m_Height = 0;
    bool m_Ready = false, m_Enabled = true;

    rhi::RHIPipelineStateHandle m_PSO{};   // A-3
    rhi::DescriptorSetLayoutHandle m_DescLayout = rhi::kInvalidLayout;
    rhi::DescriptorSetHandle       m_DescSet    = rhi::kInvalidSet;

    rhi::IRHITexture* m_Input        = nullptr;
    rhi::IRHISampler* m_InputSampler = nullptr;
    rhi::RHITextureHandle m_Placeholder{};   // A-3
    rhi::RHISamplerHandle m_PlaceholderSamp{};   // A-3
};

} // namespace he::render
