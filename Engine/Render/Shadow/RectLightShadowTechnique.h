#pragma once

#include "Threading/RenderThreadContext.h"   // T2.6：帧上下文（唯一 RHI 出口）

#include "Shadow/IShadowTechnique.h"
#include "RHI/Shader.h"
#include <memory>

namespace he::render {

// 矩形面光阴影贴图默认分辨率
constexpr u32 kDefaultRectShadowSize = 1024;

// ============================================================================
// RectLightShadowTechnique — 矩形面光 2D 透视阴影（Phase 2）
//
// kDefaultRectShadowSize D32_FLOAT × 1 张 2D 纹理
// 从矩形中心沿发光法线方向做透视投影（近似），主 pass 侧做大半径 PCF 软阴影
// ============================================================================
class RectLightShadowTechnique : public IShadowTechnique {
public:
    RectLightShadowTechnique()=default;
    ~RectLightShadowTechnique()override=default;

    const char* GetName()const override{return"RectLightShadowTechnique";}

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
        return m_Device?m_Device->ResolveTexture(m_RectShadowMap[m_FrameSlot]):nullptr;}
    rhi::IRHITexture* GetShadowMapAt(u32 slot,u32)const override{   // A-3
        return m_Device?m_Device->ResolveTexture(m_RectShadowMap[(slot<MAX_FRAMES_IN_FLIGHT)?slot:0u]):nullptr;}
    rhi::IRHISampler* GetShadowSampler()const override{   // A-3
        return m_Device?m_Device->ResolveSampler(m_RectShadowSampler):nullptr;}
    void SetFrameSlot(u32 slot)override{m_FrameSlot=(slot<MAX_FRAMES_IN_FLIGHT)?slot:0u;}

    void CreatePSO(rhi::DescriptorSetLayoutHandle layout);

private:
    rhi::ShaderBytecode m_ShadowVS,m_ShadowFS;
    rhi::RHIPipelineStateHandle m_ShadowPSO{};   // A-3：句柄持有
    // 每飞行帧各一张（每帧清空重画 ⇒ 单份会被在飞帧采到中间态）
    rhi::RHITextureHandle m_RectShadowMap[MAX_FRAMES_IN_FLIGHT]{};   // A-3：句柄持有
    rhi::RHISamplerHandle m_RectShadowSampler{};   // A-3：句柄持有
    u32 m_MapSize = kDefaultRectShadowSize;
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

};

} // namespace he::render
