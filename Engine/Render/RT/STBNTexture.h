#pragma once

#include "Threading/RenderThreadContext.h"   // T2.6：帧上下文（唯一 RHI 出口）
#include "RHI/RHI.h"
#include <memory>

namespace he::render {

// ============================================================
// STBNTexture — 时空蓝噪声 3D 纹理（128×128×64 RGBA8）
// 数据来自内嵌头文件 STBNData.h（一次性离线生成）。
// 供 PT/ReSTIR shader 通过 Load() 采样（无采样器，规避组合采样器坑）。
// ============================================================
class STBNTexture {
public:
    bool Initialize(rhi::IRHIDevice* device);
    void Shutdown();
    rhi::IRHITexture* GetTexture() const {   // A-3：句柄按需解析
        return m_Device ? m_Device->ResolveTexture(m_Texture) : nullptr;
    }
    bool IsValid() const { return m_Texture.IsValid(); }

private:
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

    rhi::RHITextureHandle m_Texture{};   // A-3
};

} // namespace he::render
