#pragma once

#include "Threading/RenderThreadContext.h"   // T2.6：帧上下文（唯一 RHI 出口）

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
    static constexpr float kMinExposure      = 0.005f;   // 最小曝光值
    static constexpr float kMaxExposure      = 200.0f;   // 最大曝光值
    static constexpr float kDefaultDeltaTime = 0.016f;   // 默认帧间隔（1/60s）

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
    float GetDisplayWhitePoint() const { return m_DisplayWhitePoint; }
    void  SetDisplayWhitePoint(float w) { m_DisplayWhitePoint = w; }

private:
    static constexpr u32 kNumGroups = 256;  // 16×16 组

    rhi::IRHIDevice* m_Device = nullptr;

    /// 【T2.6】本帧的渲染线程上下文（可空；空时解析回退 `m_Device`）
    const RenderThreadContext* m_FrameCtx = nullptr;

public:
    /// 【T2.6】设置本帧的渲染线程上下文（搬迁过渡期用）
    void SetFrameContext(const RenderThreadContext* ctx) { m_FrameCtx = ctx; }

    // 【T2.6】统一解析入口。回退分支直接调 m_Device->Resolve*（不能被同一个扫描命中，
    // 否则会变成无限递归）；也不引入新的访问标签，避免改变后续成员的访问级别。
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
    bool m_Ready  = false;
    bool m_Enabled = true;
    float m_Exposure   = 1.0f;
    float m_AdaptSpeed = 2.0f;
    float m_TargetLum  = 0.18f;
    float m_DisplayWhitePoint = 80.0f;  // SDR 参考白点（尼特），默认 80
    float m_PrevLogLum = -2.47f;  // 历史平均对数亮度

    rhi::ShaderBytecode m_CS;
    rhi::RHIPipelineStateHandle m_PSO{};   // A-3：句柄持有
    rhi::DescriptorSetLayoutHandle m_Layout = rhi::kInvalidLayout;
    rhi::DescriptorSetHandle       m_Set    = rhi::kInvalidSet;
    std::unique_ptr<rhi::IRHIBuffer> m_ResultBuf;  // 256 floats

    rhi::IRHITexture* m_HDRInput   = nullptr;
    rhi::IRHISampler* m_HDRSampler = nullptr;
};

} // namespace he::render
