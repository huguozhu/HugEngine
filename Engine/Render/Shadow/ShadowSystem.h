#pragma once
#include "Subsystem/RenderSubsystem.h"
#include "Pipeline/Material.h"
#include "RHI/RHI.h"
#include "RHI/Buffer.h"
#include "RHI/Shader.h"
#include "Core/Types.h"
#include "Scene/Entity.h"
#include <memory>
#include <vector>

namespace he { class World; class SceneGraph; }

namespace he::render {

class ShadowSystem : public IRenderSubsystem {
    HE_DECLARE_NON_COPYABLE(ShadowSystem);
public:
    ShadowSystem()=default;
    ~ShadowSystem()override=default;

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

    void NextFrame();
    void SetRenderResources(rhi::IRHIBuffer* objectBuffer,rhi::IRHIBuffer* shadowBuffer,rhi::DescriptorSetHandle descSet);
    void CreateShadowPSO(rhi::DescriptorSetLayoutHandle layout);

    rhi::IRHITexture* GetCSMShadowMap(u32 c)const;
    rhi::IRHISampler* GetShadowSampler()const{return m_ShadowSampler.get();}
    rhi::IRHITexture* GetPointShadowMap()const{return m_PointShadowMap.get();}
    rhi::IRHISampler* GetPointShadowSampler()const{return m_PointShadowSampler.get();}

    bool HasDirectionalShadows()const{return m_HasDirectionalShadows;}
    bool HasPointShadows()const{return m_HasPointShadows;}
    u32  GetCurrentFrameSlot()const{return m_CurrentFrameSlot;}

    /// 获取指定光源的 ShadowIndex（阴影数据在 SSBO 中的偏移）
    /// 返回 -1 表示该光源未投射阴影
    i32 GetShadowIndex(he::Entity lightEntity)const;

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
    std::vector<he::Entity>    m_ShadowEntities;  // 方向光 Entity 列表（与 m_ShadowGPUData 一一对应）
    std::vector<he::Entity>    m_PointShadowEntities; // 点光源 Entity 列表（与 m_PointShadowData 一一对应）
    u32 m_CurrentFrameSlot=0;
    bool m_Ready=false,m_HasDirectionalShadows=false,m_HasPointShadows=false;
    he::World* m_CachedWorld=nullptr;
    he::SceneGraph* m_CachedSceneGraph=nullptr;
};

} // namespace he::render
