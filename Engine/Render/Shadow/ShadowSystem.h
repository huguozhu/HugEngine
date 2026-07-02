#pragma once

#include "Shadow/IShadowSystem.h"
#include "Shadow/IShadowTechnique.h"
#include <memory>
#include <vector>

namespace he { class World; class SceneGraph; }

namespace he::render {

// ============================================================================
// ShadowSystem — 阴影子系统组合器
//
// 组合多个 IShadowTechnique（CSM / Point / RT），统一管理：
//   - ShadowData SSBO（三缓冲）
//   - Entity → ShadowIndex 映射
//   - 对外暴露 IShadowSystem 接口
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
    u32 GetShadowMapCount()const override;
    rhi::IRHITexture* GetShadowMap(u32 i)const override;
    rhi::IRHISampler* GetShadowSampler()const override;
    rhi::IRHITexture* GetPointShadowMap()const override;
    rhi::IRHISampler* GetPointShadowSampler()const override;
    i32 GetShadowIndex(Entity light)const override;
    bool HasActiveShadows()const override{return m_ActiveCount>0;}
    float4x4 GetLightViewProj(u32 cascade)const override;

private:
    std::vector<std::unique_ptr<IShadowTechnique>> m_Techniques;

    // ShadowData SSBO（三缓冲，ForwardPipeline 自有）
    rhi::IRHIBuffer* m_ExternalShadowBuffer=nullptr;

    // 所有光源的 Entity→Index 映射（每帧 Update 重建）
    std::vector<he::Entity>    m_AllEntities;
    std::vector<GPUShadowData> m_AllShadowData;       // 缓存（供 Render 复用）
    std::vector<u32>           m_PerTechniqueCounts;  // 每个 Technique 的条目数

    u32 m_CurrentFrameSlot=0,m_ActiveCount=0;
    bool m_Ready=false;
    he::World* m_CachedWorld=nullptr;
    he::SceneGraph* m_CachedSceneGraph=nullptr;
};

} // namespace he::render
