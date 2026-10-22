#pragma once

#include "Threading/RenderThreadContext.h"   // T2.6：帧上下文（唯一 RHI 出口）

#include "RT/RTEffectPass.h"
#include <memory>

namespace he::render {

// ============================================================
// RTShadowPass — 硬件 Ray Tracing 阴影 Pass
//
// 对每个像素向每个阴影光源发射 shadow ray，输出单通道阴影遮罩。
// 替代传统 CSM + Spot Shadow Maps。
//
// 特性:
//   - 无级联过渡伪影 (cascade blending artifacts)
//   - 无 shadow acne / peter panning
//   - 天然支持点光源 / 聚光灯阴影
//   - 可扩展为面光源软阴影
//
// 输出: R16_FLOAT 半分辨率（默认）阴影遮罩（0=全影, 1=无影）
// 阴影语义: RayGen 用 ACCEPT_FIRST_HIT + AnyHit 置 blocked=true，
//           Miss 保持 blocked=false → 命中即遮挡，无最近命中开销
// ============================================================
class RTShadowPass : public RTEffectPass {

    // ── 【T2.6】帧上下文与统一解析入口（m_Device 继承自基类）──
    const RenderThreadContext* m_FrameCtx = nullptr;
    void SetFrameContext(const RenderThreadContext* ctx) { m_FrameCtx = ctx; }
    [[nodiscard]] rhi::IRHIBuffer* ResB(rhi::RHIBufferHandle h) const {
        if (m_FrameCtx) return m_FrameCtx->ResolveBuffer(h);
        return m_Device ? m_Device->ResolveBuffer(h) : nullptr;
    }
    [[nodiscard]] rhi::IRHITexture* ResT(rhi::RHITextureHandle h) const {
        if (m_FrameCtx) return m_FrameCtx->ResolveTexture(h);
        return m_Device ? m_Device->ResolveTexture(h) : nullptr;
    }
    [[nodiscard]] rhi::IRHIPipelineState* ResP(rhi::RHIPipelineStateHandle h) const {
        if (m_FrameCtx) return m_FrameCtx->ResolvePipelineState(h);
        return m_Device ? m_Device->ResolvePipelineState(h) : nullptr;
    }
    [[nodiscard]] rhi::IRHISampler* ResS(rhi::RHISamplerHandle h) const {
        if (m_FrameCtx) return m_FrameCtx->ResolveSampler(h);
        return m_Device ? m_Device->ResolveSampler(h) : nullptr;
    }

    HE_DECLARE_NON_COPYABLE(RTShadowPass);

public:
    RTShadowPass()  = default;
    ~RTShadowPass() override = default;

    // 初始化（halfRes=true 时输出半分辨率遮罩）
    bool Initialize(rhi::IRHIDevice* device, u32 fullWidth, u32 fullHeight,
                    bool halfRes = true);

    // 每帧执行：更新 set0 描述符 + 填充光源 UB + push constants + TraceRays
    void Execute(rhi::IRHICommandList* cmd,
                 rhi::IRHIAccelerationStructure* tlas,
                 const RTExecuteContext& ctx) override;

    // ── 访问器 ──
    bool IsHalfRes() const { return m_HalfRes; }
    void SetHalfRes(bool hr) { m_HalfRes = hr; }

private:
    // 从 GPULight[] SSBO 显式抽取阴影光源数据（48B/light，匹配 shader ShadowLight 布局）
    void FillLightBuffer(const RTExecuteContext& ctx);

    rhi::RHIBufferHandle m_LightUB{};   // A-3：阴影光源 Uniform Buffer（48B × 16）
    u32 m_FullWidth = 0, m_FullHeight = 0;
    bool m_HalfRes = true;       // 默认半分辨率（阴影是低频效果）
};

} // namespace he::render
