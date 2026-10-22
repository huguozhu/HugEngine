#pragma once

#include "Threading/RenderThreadContext.h"   // T2.6：帧上下文（唯一 RHI 出口）

#include "Shadow/IShadowTechnique.h"
#include "RHI/Shader.h"
#include <memory>

namespace he::render {

// 聚光灯阴影贴图默认分辨率
constexpr u32 kDefaultSpotShadowSize = 1024;

// ============================================================================
// SpotShadowTechnique — 聚光灯 2D 透视阴影
//
// kDefaultSpotShadowSize D32_FLOAT × 1 张 2D 纹理
// FOV = outerConeAngle × 2，透视投影
// ============================================================================
class SpotShadowTechnique : public IShadowTechnique {
public:
    SpotShadowTechnique()=default;
    ~SpotShadowTechnique()override=default;

    const char* GetName()const override{return"SpotShadowTechnique";}

    bool Initialize(rhi::IRHIDevice* device)override;
    void Shutdown()override;
    void SetRenderResources(rhi::IRHIBuffer* objBuf,rhi::DescriptorSetHandle descSet)override;

    u32 CollectLights(const FrameSceneSnapshot& snapshot,const CameraData& camera,
                      std::vector<GPUShadowData>& outData,
                      std::vector<he::Entity>& outEntities)override;

    void Render(rhi::IRHICommandList* cmd,const FrameSceneSnapshot& snapshot,
                const MeshRegistry& registry,
                const std::vector<GPUShadowData>& shadowData,u32 dataStartIndex)override;

    u32 GetShadowMapCount()const override{return 1;}
    rhi::IRHITexture* GetShadowMap(u32)const override{   // A-3：句柄按需解析
        return m_Device?m_Device->ResolveTexture(m_SpotShadowMap[m_FrameSlot]):nullptr;}
    rhi::IRHITexture* GetShadowMapAt(u32 slot,u32)const override{   // A-3
        return m_Device?m_Device->ResolveTexture(m_SpotShadowMap[(slot<MAX_FRAMES_IN_FLIGHT)?slot:0u]):nullptr;}
    rhi::IRHISampler* GetShadowSampler()const override{   // A-3
        return m_Device?m_Device->ResolveSampler(m_SpotShadowSampler):nullptr;}
    void SetFrameSlot(u32 slot)override{m_FrameSlot=(slot<MAX_FRAMES_IN_FLIGHT)?slot:0u;}

    void CreatePSO(rhi::DescriptorSetLayoutHandle layout);

private:
    rhi::ShaderBytecode m_ShadowVS,m_ShadowFS;
    rhi::RHIPipelineStateHandle m_ShadowPSO{};   // A-3：句柄持有
    // 每飞行帧各一张（每帧清空重画 ⇒ 单份会被在飞帧采到中间态，见 IShadowTechnique::SetFrameSlot）
    rhi::RHITextureHandle m_SpotShadowMap[MAX_FRAMES_IN_FLIGHT]{};   // A-3：句柄持有
    rhi::RHISamplerHandle m_SpotShadowSampler{};   // A-3：句柄持有
    u32 m_MapSize = kDefaultSpotShadowSize;
    u32 m_FrameSlot = 0;

    rhi::IRHIBuffer*        m_ExternalObjectBuffer=nullptr;
    rhi::DescriptorSetHandle m_ExternalDescSet=rhi::kInvalidSet;
    rhi::IRHIDevice* m_Device=nullptr;

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

};

} // namespace he::render
