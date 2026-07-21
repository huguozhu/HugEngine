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

    u32 CollectLights(he::World& world,he::SceneGraph& sg,const CameraData& camera,
                      std::vector<GPUShadowData>& outData,
                      std::vector<he::Entity>& outEntities)override;

    void Render(rhi::IRHICommandList* cmd,he::World& world,he::SceneGraph& sg,
                const std::vector<GPUShadowData>& shadowData,u32 dataStartIndex)override;

    u32 GetShadowMapCount()const override{return 1;}
    rhi::IRHITexture* GetShadowMap(u32)const override{return m_PointShadowMap.get();}
    rhi::IRHISampler* GetShadowSampler()const override{return m_PointShadowSampler.get();}
    rhi::IRHITexture* GetPointShadowMap()const override{return m_PointShadowMap.get();}
    rhi::IRHISampler* GetPointShadowSampler()const override{return m_PointShadowSampler.get();}

    // PSO 延迟创建
    void CreatePSO(rhi::DescriptorSetLayoutHandle layout);

private:
    rhi::ShaderBytecode m_ShadowVS,m_ShadowFS;
    std::unique_ptr<rhi::IRHIPipelineState> m_ShadowPSO;
    std::unique_ptr<rhi::IRHITexture> m_PointShadowMap;
    std::unique_ptr<rhi::IRHISampler> m_PointShadowSampler;
    u32 m_MapSize = kDefaultPointShadowSize;

    rhi::IRHIBuffer*        m_ExternalObjectBuffer=nullptr;
    rhi::DescriptorSetHandle m_ExternalDescSet=rhi::kInvalidSet;
    rhi::IRHIDevice* m_Device=nullptr;
};

} // namespace he::render
