#pragma once

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
    rhi::IRHITexture* GetShadowMap(u32)const override{return m_SpotShadowMap[m_FrameSlot].get();}
    rhi::IRHITexture* GetShadowMapAt(u32 slot,u32)const override{
        return m_SpotShadowMap[(slot<MAX_FRAMES_IN_FLIGHT)?slot:0u].get();}
    rhi::IRHISampler* GetShadowSampler()const override{return m_SpotShadowSampler.get();}
    void SetFrameSlot(u32 slot)override{m_FrameSlot=(slot<MAX_FRAMES_IN_FLIGHT)?slot:0u;}

    void CreatePSO(rhi::DescriptorSetLayoutHandle layout);

private:
    rhi::ShaderBytecode m_ShadowVS,m_ShadowFS;
    std::unique_ptr<rhi::IRHIPipelineState> m_ShadowPSO;
    // 每飞行帧各一张（每帧清空重画 ⇒ 单份会被在飞帧采到中间态，见 IShadowTechnique::SetFrameSlot）
    std::unique_ptr<rhi::IRHITexture> m_SpotShadowMap[MAX_FRAMES_IN_FLIGHT];
    std::unique_ptr<rhi::IRHISampler> m_SpotShadowSampler;
    u32 m_MapSize = kDefaultSpotShadowSize;
    u32 m_FrameSlot = 0;

    rhi::IRHIBuffer*        m_ExternalObjectBuffer=nullptr;
    rhi::DescriptorSetHandle m_ExternalDescSet=rhi::kInvalidSet;
    rhi::IRHIDevice* m_Device=nullptr;
};

} // namespace he::render
