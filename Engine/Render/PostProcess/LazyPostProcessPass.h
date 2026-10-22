#pragma once

#include "Threading/RenderThreadContext.h"   // T2.6：帧上下文（唯一 RHI 出口）

#include "RHI/RHI.h"

namespace he::render {

// ============================================================
// LazyPostProcessPass — 懒初始化后处理 Pass 基类
//
// 封装懒初始化机制：
//   - SetDevice()   注入设备/尺寸（不分配 GPU 资源）
//   - SetEnabled()  首次开启时才触发真正的 Initialize（EnsureInitialized 守卫）
//
// 派生类只需实现 Initialize/Shutdown/OnResize 三个纯虚函数，
// 以及各自特有的渲染接口（SetInput/Render/GetOutput 等）。
// ============================================================
class LazyPostProcessPass {
public:
    virtual ~LazyPostProcessPass() = default;

    virtual bool Initialize(rhi::IRHIDevice* device, u32 width, u32 height) = 0;
    virtual void Shutdown() = 0;
    virtual void OnResize(u32 width, u32 height) = 0;

    // 注入设备/尺寸（懒初始化前由 PostProcessChain 调用，使 SetEnabled 能触发真正的 Initialize）
    void SetDevice(rhi::IRHIDevice* device, u32 width, u32 height) {
        m_Device = device; m_Width = width; m_Height = height;
    }

    bool IsEnabled() const { return m_Enabled; }
    void SetEnabled(bool e) {
        m_Enabled = e;
        if (e && !m_Ready) EnsureInitialized();
    }
    bool IsReady() const { return m_Ready; }

protected:
    // 懒初始化守卫：未初始化且已注入设备时才真正初始化
    void EnsureInitialized() {
        if (m_Ready || !m_Device) return;
        Initialize(m_Device, m_Width, m_Height);
    }

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
    bool m_Ready = false, m_Enabled = false;
};

} // namespace he::render
