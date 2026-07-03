#pragma once

#include "Shadow/IShadowTechnique.h"
#include "RHI/Shader.h"
#include <memory>

namespace he::render {

// ============================================================================
// SpotShadowTechnique — 聚光灯 2D 透视阴影
//
// 1024×1024 D32_FLOAT × 1 张 2D 纹理
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

    u32 CollectLights(he::World& world,he::SceneGraph& sg,const CameraData& camera,
                      std::vector<GPUShadowData>& outData,
                      std::vector<he::Entity>& outEntities)override;

    void Render(rhi::IRHICommandList* cmd,he::World& world,he::SceneGraph& sg,
                const std::vector<GPUShadowData>& shadowData,u32 dataStartIndex)override;

    u32 GetShadowMapCount()const override{return 1;}
    rhi::IRHITexture* GetShadowMap(u32)const override{return m_SpotShadowMap.get();}
    rhi::IRHISampler* GetShadowSampler()const override{return m_SpotShadowSampler.get();}

    void CreatePSO(rhi::DescriptorSetLayoutHandle layout);

private:
    rhi::ShaderBytecode m_ShadowVS,m_ShadowFS;
    std::unique_ptr<rhi::IRHIPipelineState> m_ShadowPSO;
    std::unique_ptr<rhi::IRHITexture> m_SpotShadowMap;
    std::unique_ptr<rhi::IRHISampler> m_SpotShadowSampler;
    u32 m_MapSize=1024;

    rhi::IRHIBuffer*        m_ExternalObjectBuffer=nullptr;
    rhi::DescriptorSetHandle m_ExternalDescSet=rhi::kInvalidSet;
    rhi::IRHIDevice* m_Device=nullptr;
};

} // namespace he::render
