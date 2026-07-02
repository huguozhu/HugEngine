#pragma once

#include "Shadow/IShadowTechnique.h"
#include "RHI/Shader.h"
#include <memory>

namespace he::render {

// ============================================================================
// CSMTechnique — 方向光 Cascaded Shadow Maps
//
// 3 级联 × 2048×2048 D32_FLOAT 阴影贴图
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

    u32 CollectLights(he::World& world,he::SceneGraph& sg,const CameraData& camera,
                      std::vector<GPUShadowData>& outData,
                      std::vector<he::Entity>& outEntities)override;

    void Render(rhi::IRHICommandList* cmd,he::World& world,he::SceneGraph& sg,
                const std::vector<GPUShadowData>& shadowData,u32 dataStartIndex)override;

    u32 GetShadowMapCount()const override{return CASCADE_COUNT;}
    rhi::IRHITexture* GetShadowMap(u32 i)const override;
    rhi::IRHISampler* GetShadowSampler()const override{return m_ShadowSampler.get();}

    // PSO 延迟创建
    void CreatePSO(rhi::DescriptorSetLayoutHandle layout);

    // 暴露 PSO（供 PointShadowTechnique 复用）
    rhi::IRHIPipelineState* GetPSO()const{return m_ShadowPSO.get();}

    // 获取 cascade i 的光源 VP 矩阵（供 RSM 等 GI 技术复用）
    float4x4 GetLightViewProj(u32 cascade) const { return (cascade < CASCADE_COUNT) ? m_LightVPs[cascade] : float4x4(1.0f); }
    u32      GetShadowMapSize()    const { return m_ShadowMapSize; }

private:
    void RenderCascade(rhi::IRHICommandList* cmd,u32 ci,he::World& w,he::SceneGraph& sg,
                       const GPUShadowData& sd);
    static float4x4 ComputeCascadeViewProj(const float3& ld,const CameraData& cam,float sn,float sf);

    rhi::ShaderBytecode m_ShadowVS,m_ShadowFS;
    std::unique_ptr<rhi::IRHIPipelineState> m_ShadowPSO;
    std::unique_ptr<rhi::IRHITexture> m_ShadowMaps[CASCADE_COUNT];
    std::unique_ptr<rhi::IRHISampler> m_ShadowSampler;
    u32 m_ShadowMapSize=2048;

    rhi::IRHIBuffer*        m_ExternalObjectBuffer=nullptr;
    rhi::DescriptorSetHandle m_ExternalDescSet=rhi::kInvalidSet;
    rhi::IRHIDevice* m_Device=nullptr;
    float4x4 m_LightVPs[CASCADE_COUNT]{};  // 缓存最近一次的 cascade VP（供 RSM 查询）
};

} // namespace he::render
