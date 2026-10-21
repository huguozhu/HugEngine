#pragma once

#include "Shadow/IShadowTechnique.h"
#include "RHI/Shader.h"
#include <memory>

namespace he::render {

// 方向光 Cascaded Shadow Map 默认分辨率
constexpr u32 kDefaultCSMShadowSize = 2048;

// ============================================================================
// CSMTechnique — 方向光 Cascaded Shadow Maps
//
// 3 级联 × kDefaultCSMShadowSize D32_FLOAT 阴影贴图
// 混合分割（λ=0.5），正交投影
// ============================================================================
class CSMTechnique : public IShadowTechnique {
public:
    CSMTechnique()=default;
    ~CSMTechnique()override=default;

    const char* GetName()const override{return"CSMTechnique";}

    bool Initialize(rhi::IRHIDevice* device)override;
    void Shutdown()override;
    void SetRenderResources(rhi::IRHIBuffer* objBuf,rhi::DescriptorSetHandle descSet)override;

    u32 CollectLights(const FrameSceneSnapshot& snapshot,const CameraData& camera,
                      std::vector<GPUShadowData>& outData,
                      std::vector<he::Entity>& outEntities)override;

    void Render(rhi::IRHICommandList* cmd,const FrameSceneSnapshot& snapshot,
                const MeshRegistry& registry,
                const std::vector<GPUShadowData>& shadowData,u32 dataStartIndex)override;

    u32 GetShadowMapCount()const override{return CASCADE_COUNT;}
    rhi::IRHITexture* GetShadowMap(u32 i)const override;
    rhi::IRHITexture* GetShadowMapAt(u32 slot,u32 i)const override;
    rhi::IRHISampler* GetShadowSampler()const override{   // A-3
        return m_Device?m_Device->ResolveSampler(m_ShadowSampler):nullptr;}
    // 逐帧重画的贴图按飞行帧槽位取用（见 IShadowTechnique::SetFrameSlot）
    void SetFrameSlot(u32 slot)override{m_FrameSlot=(slot<MAX_FRAMES_IN_FLIGHT)?slot:0u;}

    // PSO 延迟创建
    void CreatePSO(rhi::DescriptorSetLayoutHandle layout);

    // 暴露 PSO（供 PointShadowTechnique 复用）
    rhi::IRHIPipelineState* GetPSO()const{   // A-3：PointShadowTechnique 复用本 PSO，按需解析
        return m_Device?m_Device->ResolvePipelineState(m_ShadowPSO):nullptr;}

    // 获取 cascade i 的光源 VP 矩阵（供 RSM 等 GI 技术复用）
    float4x4 GetLightViewProj(u32 cascade) const { return (cascade < CASCADE_COUNT) ? m_LightVPs[cascade] : float4x4(1.0f); }
    u32      GetShadowMapSize()    const { return m_ShadowMapSize; }

private:
    void RenderCascade(rhi::IRHICommandList* cmd,u32 ci,const FrameSceneSnapshot& snapshot,
                       const MeshRegistry& registry,const GPUShadowData& sd);
    static float4x4 ComputeCascadeViewProj(const float3& ld,const CameraData& cam,float sn,float sf);

    rhi::ShaderBytecode m_ShadowVS,m_ShadowFS;
    rhi::RHIPipelineStateHandle m_ShadowPSO{};   // A-3：句柄持有
    // 【为什么是二维数组】级联贴图每帧"清空→重画"且同帧被采样，而 GPU 最多落后 CPU 三个飞行帧：
    // 单份时第 N+1 帧的清空/重画会盖掉第 N 帧正在采样的内容（相机移动时表现为阴影闪动/丢失）。
    // 按 [飞行帧][级联] 分份后本帧只写本槽位、采样也只读本槽位。
    rhi::RHITextureHandle m_ShadowMaps[MAX_FRAMES_IN_FLIGHT][CASCADE_COUNT]{};   // A-3：句柄持有
    rhi::RHISamplerHandle m_ShadowSampler{};   // A-3：句柄持有
    u32 m_ShadowMapSize = kDefaultCSMShadowSize;
    u32 m_FrameSlot = 0;

    rhi::IRHIBuffer*        m_ExternalObjectBuffer=nullptr;
    rhi::DescriptorSetHandle m_ExternalDescSet=rhi::kInvalidSet;
    rhi::IRHIDevice* m_Device=nullptr;
    float4x4 m_LightVPs[CASCADE_COUNT]{};  // 缓存最近一次的 cascade VP（供 RSM 查询）
};

} // namespace he::render
