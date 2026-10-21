#pragma once

#include "Shadow/IShadowTechnique.h"
#include "RHI/Shader.h"
#include <memory>

namespace he::render {

// 点光源阴影贴图默认分辨率（Cubemap 单面）
constexpr u32 kDefaultPointShadowSize = 512;

// ============================================================================
// PointShadowTechnique — 点光源 Cubemap 阴影
//
// kDefaultPointShadowSize D32_FLOAT × 6 面 Cubemap
// 90° FOV 透视投影，逐面渲染
// ============================================================================
class PointShadowTechnique : public IShadowTechnique {
public:
    PointShadowTechnique()=default;
    ~PointShadowTechnique()override=default;

    const char* GetName()const override{return"PointShadowTechnique";}

    bool Initialize(rhi::IRHIDevice* device)override;
    void Shutdown()override;
    void SetRenderResources(rhi::IRHIBuffer* objBuf,rhi::DescriptorSetHandle descSet)override;
    // 逐帧资源按飞行帧槽位取用（见 IShadowTechnique::SetFrameSlot）
    void SetFrameSlot(u32 slot)override{m_FrameSlot=(slot<MAX_FRAMES_IN_FLIGHT)?slot:0u;}

    u32 CollectLights(const FrameSceneSnapshot& snapshot,const CameraData& camera,
                      std::vector<GPUShadowData>& outData,
                      std::vector<he::Entity>& outEntities)override;

    void Render(rhi::IRHICommandList* cmd,const FrameSceneSnapshot& snapshot,
                const MeshRegistry& registry,
                const std::vector<GPUShadowData>& shadowData,u32 dataStartIndex)override;

    u32 GetShadowMapCount()const override{return 1;}
    rhi::IRHITexture* GetShadowMap(u32)const override{   // A-3：句柄按需解析
        return m_Device?m_Device->ResolveTexture(m_PointShadowMap[m_FrameSlot]):nullptr;}
    rhi::IRHISampler* GetShadowSampler()const override{   // A-3
        return m_Device?m_Device->ResolveSampler(m_PointShadowSampler):nullptr;}
    rhi::IRHITexture* GetPointShadowMap(u32 slot)const override{   // A-3
        return m_Device?m_Device->ResolveTexture(m_PointShadowMap[(slot<MAX_FRAMES_IN_FLIGHT)?slot:0u]):nullptr;}
    rhi::IRHITexture* GetShadowMapAt(u32 slot,u32)const override{   // A-3
        return m_Device?m_Device->ResolveTexture(m_PointShadowMap[(slot<MAX_FRAMES_IN_FLIGHT)?slot:0u]):nullptr;}
    rhi::IRHISampler* GetPointShadowSampler()const override{   // A-3
        return m_Device?m_Device->ResolveSampler(m_PointShadowSampler):nullptr;}

    // PSO 延迟创建
    void CreatePSO(rhi::DescriptorSetLayoutHandle layout);

private:
    rhi::ShaderBytecode m_ShadowVS,m_ShadowFS;
    rhi::RHIPipelineStateHandle m_ShadowPSO{};   // A-3：句柄持有
    // 【为什么是数组】点光立方体阴影图每帧都要"清空 → 6 面重画"，而 GPU 最多落后 CPU 三个飞行帧：
    // 只有一份时，第 N+1 帧的清空/重画会与第 N 帧"仍在执行的采样"重叠 ⇒ 采样读到被清掉（depth=1，
    // 判据判成"照亮"）或被丢弃的深度 ⇒ 静态场景整幅明暗闪烁（实测 15 帧里 2 帧变暗到 68%~87%，
    // 每帧 `WaitIdle` 后完全消失）。按飞行帧各持一份后，本帧只写本槽位、采样也只读本槽位；
    // 槽位再次被复用时其 3 帧前的提交已完成（`VulkanCommandList::Begin` 会等该槽位栅栏）。
    rhi::RHITextureHandle m_PointShadowMap[MAX_FRAMES_IN_FLIGHT]{};   // A-3：句柄持有
    rhi::RHISamplerHandle m_PointShadowSampler{};   // A-3：句柄持有
    u32 m_MapSize = kDefaultPointShadowSize;
    u32 m_FrameSlot = 0;

    rhi::IRHIBuffer*        m_ExternalObjectBuffer=nullptr;
    rhi::DescriptorSetHandle m_ExternalDescSet=rhi::kInvalidSet;
    rhi::IRHIDevice* m_Device=nullptr;
};

} // namespace he::render
