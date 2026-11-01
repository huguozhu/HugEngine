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

    // 设置当前**飞行帧槽位**（`[0, kMaxFramesInFlight)`）；调用顺序：SetFrameSlot → SetInputs → Render。
    // 【为什么必须有】本 pass 的输入是上一级时域降噪的输出，而那张图在两张之间逐帧轮换
    // ⇒ 该描述符绑定点**每帧都在变**。只备一份描述符集时，本帧改写会改掉"仍在 GPU 上执行的
    // 上一帧"所用的那份，上一帧于是采样到本帧才绑上去的纹理（同一对纹理里的另一张）。
    // 与 RTDenoiser::SetFrameSlot 同一根因、同一修法（详见该函数的长注释）。
    void SetFrameSlot(u32 slot) { m_Slot = slot % rhi::kMaxFramesInFlight; }

    /// 本帧应使用的描述符集（每飞行帧一份）
    [[nodiscard]] rhi::DescriptorSetHandle CurrentSet() const { return m_Set[m_Slot]; }

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
    bool m_Ready = false;

    std::unique_ptr<rhi::IRHITexture> m_Output;   // 空间滤波结果
    /// 输出图是否已完成"首次使用"的 UNDEFINED → GENERAL 布局转换（重建/改尺寸后复位）。
    /// 见 Render() 里的说明：compute 的 storage image 没有 render pass 帮它初始化布局。
    bool m_FirstUseDone = false;
    rhi::RHIPipelineStateHandle m_PSO{};   // A-3：句柄持有
    rhi::DescriptorSetLayoutHandle m_Layout = rhi::kInvalidLayout;
    // 描述符集每飞行帧一份（理由见 SetFrameSlot）：输入纹理逐帧轮换，不能共用一份集
    rhi::DescriptorSetHandle       m_Set[rhi::kMaxFramesInFlight] = {};
    u32                            m_Slot = 0;   // 当前飞行帧槽位

    rhi::IRHITexture* m_Color  = nullptr;
    rhi::IRHITexture* m_Depth  = nullptr;
    rhi::IRHITexture* m_Normal = nullptr;
};

} // namespace he::render
