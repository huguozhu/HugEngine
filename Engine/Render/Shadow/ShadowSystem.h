#pragma once

#include "Shadow/IShadowSystem.h"
#include "Pipeline/Material.h"
#include "RHI/Shader.h"
#include <memory>
#include <vector>

namespace he { class World; class SceneGraph; }

namespace he::render {

// ============================================================================
// ShadowSystem — CSM 级联阴影 + 点光源 Cubemap 阴影
// ============================================================================
class ShadowSystem : public IShadowSystem {
    HE_DECLARE_NON_COPYABLE(ShadowSystem);
public:
    ShadowSystem()=default;
    ~ShadowSystem()override=default;

    // IRenderSubsystem
    bool Initialize(rhi::IRHIDevice* device,u32 width,u32 height)override;
    void Shutdown()override;
    void Update(const SubsystemContext& ctx)override;
    void Render(rhi::IRHICommandList* cmdList)override;
    void Bind(rhi::IRHICommandList* cmdList)const override;
    void OnResize(u32 width,u32 height)override;
    const char* GetName()const override{return"ShadowSystem";}
    bool IsReady()const override{return m_Ready;}
    bool IsEnabled()const override{return m_Enabled;}
    void SetEnabled(bool e)override{m_Enabled=e;}

    // IShadowSystem
    Mode GetMode()const override{return Mode::Traditional;}
    void NextFrame()override;
    void SetRenderResources(rhi::IRHIBuffer* objBuf,rhi::IRHIBuffer* shadowBuf,rhi::DescriptorSetHandle descSet)override;
    void CreateShadowPSO(rhi::DescriptorSetLayoutHandle layout)override;
    u32 GetShadowMapCount()const override{return CASCADE_COUNT;}
    rhi::IRHITexture* GetShadowMap(u32 i)const override;
    rhi::IRHISampler* GetShadowSampler()const override{return m_ShadowSampler.get();}
    rhi::IRHITexture* GetPointShadowMap()const override{return m_PointShadowMap.get();}
    rhi::IRHISampler* GetPointShadowSampler()const override{return m_PointShadowSampler.get();}
    i32 GetShadowIndex(Entity light)const override;
    bool HasActiveShadows()const override{return m_HasDirectionalShadows||m_HasPointShadows;}

    // ---- ShadowSystem 特有查询 ----
    bool HasDirectionalShadows()const{return m_HasDirectionalShadows;}
    bool HasPointShadows()const{return m_HasPointShadows;}
    u32  GetCurrentFrameSlot()const{return m_CurrentFrameSlot;}

private:
    void CollectDirectionalShadows(he::World& w,he::SceneGraph& sg,const CameraData& cam);
    void CollectPointShadows(he::World& w,he::SceneGraph& sg);
    void RenderCSMCascade(rhi::IRHICommandList* cmd,u32 ci,he::World& w,he::SceneGraph& sg);
    void RenderPointShadows(rhi::IRHICommandList* cmd,he::World& w,he::SceneGraph& sg);
    static float4x4 ComputeCascadeViewProj(const float3& ld,const CameraData& cam,float sn,float sf);

    rhi::ShaderBytecode m_ShadowVS,m_ShadowFS;
    std::unique_ptr<rhi::IRHIPipelineState> m_ShadowPSO;
    std::unique_ptr<rhi::IRHITexture> m_ShadowMaps[CASCADE_COUNT];
    std::unique_ptr<rhi::IRHISampler> m_ShadowSampler;
    u32 m_ShadowMapSize=2048;
    std::unique_ptr<rhi::IRHITexture> m_PointShadowMap;
    std::unique_ptr<rhi::IRHISampler> m_PointShadowSampler;
    u32 m_PointShadowMapSize=512;

    rhi::IRHIBuffer*        m_ExternalObjectBuffer=nullptr;
    rhi::IRHIBuffer*        m_ExternalShadowBuffer=nullptr;
    rhi::DescriptorSetHandle m_ExternalDescSet=rhi::kInvalidSet;

    std::vector<GPUShadowData> m_ShadowGPUData,m_PointShadowData;
    std::vector<he::Entity>    m_ShadowEntities,m_PointShadowEntities;
    u32 m_CurrentFrameSlot=0;
    bool m_Ready=false,m_HasDirectionalShadows=false,m_HasPointShadows=false;
    he::World* m_CachedWorld=nullptr;
    he::SceneGraph* m_CachedSceneGraph=nullptr;
};

} // namespace he::render
