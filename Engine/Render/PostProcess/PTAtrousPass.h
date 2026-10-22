#pragma once

#include "Threading/RenderThreadContext.h"   // T2.6：帧上下文（唯一 RHI 出口）
#include "RHI/RHI.h"
#include "Math/Math.h"
#include <memory>

namespace he::render {

// ============================================================
// PTAtrousPass — SVGF 风格 A-Trous 空间滤波（compute）
// 对时域降噪后的 PT 输出做多迭代边缘感知空间滤波：
//   输入：denoised HDR + depth + normal
//   输出：空间滤波结果（ToneMap 输入）
// 仿 RTDenoiser 模式：输出纹理 + PSO + set0 描述符集。
// ============================================================
class PTAtrousPass {
    HE_DECLARE_NON_COPYABLE(PTAtrousPass);

public:
    struct Config {
        u32   width  = 0;
        u32   height = 0;
        u32   iterations      = 4;
        float sigmaDepth      = 0.05f;   // 深度边权重 σ（米）
        float normalPower     = 128.0f;  // 法线边权重指数
        float sigmaColor      = 0.5f;    // 颜色边 σ 系数
        float clampThreshold  = 0.0f;    // 火萤钳制阈值（0=关）
    };

    PTAtrousPass()  = default;
    ~PTAtrousPass() = default;

    bool Initialize(rhi::IRHIDevice* device, const Config& cfg);
    void Shutdown();
    void OnResize(u32 w, u32 h);

    // 每帧绑定输入（Render 前调用，外部不持有所有权）
    void SetInputs(rhi::IRHITexture* color, rhi::IRHITexture* depth, rhi::IRHITexture* normal);

    // 运行时参数更新（CVar 热更新；Render 时写入 push constant）
    void SetParams(u32 iterations, float sigmaDepth, float normalPower,
                   float sigmaColor, float clampThreshold);

    void Render(rhi::IRHICommandList* cmd);

    rhi::IRHITexture* GetOutput() const { return m_Output.get(); }
    bool IsReady() const { return m_Ready; }

private:
    void CreateTextures(u32 w, u32 h);

    Config m_Cfg;
    rhi::IRHIDevice* m_Device = nullptr;

    /// 【T2.6】本帧的渲染线程上下文（可空；空时解析回退 `m_Device`）
    const RenderThreadContext* m_FrameCtx = nullptr;

public:
    /// 【T2.6】设置本帧的渲染线程上下文（搬迁过渡期用）
    void SetFrameContext(const RenderThreadContext* ctx) { m_FrameCtx = ctx; }

    // 【T2.6】统一解析入口。回退分支直接调 m_Device->Resolve*（不能被同一个扫描命中，
    // 否则会变成无限递归）；也不引入新的访问标签，避免改变后续成员的访问级别。
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

    u32 m_Width = 0, m_Height = 0;
    bool m_Ready = false;

    std::unique_ptr<rhi::IRHITexture> m_Output;   // 空间滤波结果
    rhi::RHIPipelineStateHandle m_PSO{};   // A-3：句柄持有
    rhi::DescriptorSetLayoutHandle m_Layout = rhi::kInvalidLayout;
    rhi::DescriptorSetHandle       m_Set    = rhi::kInvalidSet;

    rhi::IRHITexture* m_Color  = nullptr;
    rhi::IRHITexture* m_Depth  = nullptr;
    rhi::IRHITexture* m_Normal = nullptr;
};

} // namespace he::render
