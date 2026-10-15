#pragma once

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
    rhi::IRHITexture* GetShadowMap(u32)const override{return m_RectShadowMap[m_FrameSlot].get();}
    rhi::IRHITexture* GetShadowMapAt(u32 slot,u32)const override{
        return m_RectShadowMap[(slot<MAX_FRAMES_IN_FLIGHT)?slot:0u].get();}
    rhi::IRHISampler* GetShadowSampler()const override{return m_RectShadowSampler.get();}
    void SetFrameSlot(u32 slot)override{m_FrameSlot=(slot<MAX_FRAMES_IN_FLIGHT)?slot:0u;}

    void CreatePSO(rhi::DescriptorSetLayoutHandle layout);

private:
    rhi::ShaderBytecode m_ShadowVS,m_ShadowFS;
    std::unique_ptr<rhi::IRHIPipelineState> m_ShadowPSO;
    // 每飞行帧各一张（每帧清空重画 ⇒ 单份会被在飞帧采到中间态）
    std::unique_ptr<rhi::IRHITexture> m_RectShadowMap[MAX_FRAMES_IN_FLIGHT];
    std::unique_ptr<rhi::IRHISampler> m_RectShadowSampler;
    u32 m_MapSize = kDefaultRectShadowSize;
    u32 m_FrameSlot = 0;

    rhi::IRHIBuffer*        m_ExternalObjectBuffer=nullptr;
    rhi::DescriptorSetHandle m_ExternalDescSet=rhi::kInvalidSet;
    rhi::IRHIDevice* m_Device=nullptr;
};

} // namespace he::render
