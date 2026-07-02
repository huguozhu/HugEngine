#include "Shadow/ShadowSystem.h"
#include "Shadow/CSMTechnique.h"
#include "Shadow/PointShadowTechnique.h"
#include "Core/Log.h"
#include "Core/Assert.h"

namespace he::render {

bool ShadowSystem::Initialize(rhi::IRHIDevice* device,u32,u32){
    m_Device=device;HE_ASSERT(m_Device,"ShadowSystem: null device");

    // 注册默认技术
    auto csm=std::make_unique<CSMTechnique>();
    csm->Initialize(device);
    m_Techniques.push_back(std::move(csm));

    auto pt=std::make_unique<PointShadowTechnique>();
    pt->Initialize(device);
    m_Techniques.push_back(std::move(pt));

    m_Ready=true;
    HE_CORE_INFO("ShadowSystem init ({} techniques)",m_Techniques.size());
    return true;
}

void ShadowSystem::CreateShadowPSO(rhi::DescriptorSetLayoutHandle layout){
    for(auto& t:m_Techniques){
        // 每个 Technique 创建自己的 PSO
        if(auto* csm=dynamic_cast<CSMTechnique*>(t.get())) csm->CreatePSO(layout);
        if(auto* pt=dynamic_cast<PointShadowTechnique*>(t.get())) pt->CreatePSO(layout);
    }
}

void ShadowSystem::Shutdown(){
    for(auto& t:m_Techniques)t->Shutdown();
    m_Techniques.clear();
    m_Device=nullptr;m_Ready=false;
    HE_CORE_INFO("ShadowSystem shutdown");
}

void ShadowSystem::NextFrame(){m_CurrentFrameSlot=(m_CurrentFrameSlot+1)%MAX_FRAMES_IN_FLIGHT;}
void ShadowSystem::Bind(rhi::IRHICommandList*)const{}
void ShadowSystem::OnResize(u32,u32){}

void ShadowSystem::SetRenderResources(rhi::IRHIBuffer* objBuf,rhi::IRHIBuffer* shadowBuf,rhi::DescriptorSetHandle descSet){
    m_ExternalShadowBuffer=shadowBuf;
    for(auto& t:m_Techniques)t->SetRenderResources(objBuf,descSet);
}

// ============================================================================
// Update — 遍历 Techniques 收集光源，合并上传 SSBO
// ============================================================================

void ShadowSystem::Update(const SubsystemContext& ctx){
    if(!m_Ready||!m_Enabled)return;
    m_CachedWorld=ctx.world;m_CachedSceneGraph=ctx.sceneGraph;
    if(!ctx.world||!ctx.sceneGraph||!ctx.camera)return;

    m_AllShadowData.clear();m_AllEntities.clear();
    m_PerTechniqueCounts.clear();

    for(auto& t:m_Techniques){
        u32 n=t->CollectLights(*ctx.world,*ctx.sceneGraph,*ctx.camera,m_AllShadowData,m_AllEntities);
        m_PerTechniqueCounts.push_back(n);
    }

    m_ActiveCount=(u32)m_AllShadowData.size();

    if(m_ActiveCount>0&&m_ExternalShadowBuffer){
        auto*dst=static_cast<GPUShadowData*>(m_ExternalShadowBuffer->Map());
        for(u32 i=0;i<m_ActiveCount;++i)dst[i]=m_AllShadowData[i];
        m_ExternalShadowBuffer->Unmap();
    }
}

// ============================================================================
// Render — 用 Update 中缓存的数据分别渲染每个 Technique
// ============================================================================

void ShadowSystem::Render(rhi::IRHICommandList* cmd){
    if(!m_Ready||!m_Enabled||!m_ActiveCount)return;
    if(!m_CachedWorld||!m_CachedSceneGraph)return;

    u32 offset=0;
    for(usize i=0;i<m_Techniques.size();++i){
        m_Techniques[i]->Render(cmd,*m_CachedWorld,*m_CachedSceneGraph,m_AllShadowData,offset);
        offset+=m_PerTechniqueCounts[i];
    }
}

// ============================================================================
// 访问器 — 聚合所有 Techniques
// ============================================================================

u32 ShadowSystem::GetShadowMapCount()const{
    u32 n=0;for(auto& t:m_Techniques)n+=t->GetShadowMapCount();return n;
}

rhi::IRHITexture* ShadowSystem::GetShadowMap(u32 i)const{
    u32 off=0;
    for(auto& t:m_Techniques){
        u32 n=t->GetShadowMapCount();
        if(i<off+n)return t->GetShadowMap(i-off);
        off+=n;
    }
    return nullptr;
}

rhi::IRHISampler* ShadowSystem::GetShadowSampler()const{
    for(auto& t:m_Techniques)if(auto*s=t->GetShadowSampler())return s;
    return nullptr;
}

rhi::IRHITexture* ShadowSystem::GetPointShadowMap()const{
    for(auto& t:m_Techniques)if(auto*m=t->GetPointShadowMap())return m;
    return nullptr;
}

rhi::IRHISampler* ShadowSystem::GetPointShadowSampler()const{
    for(auto& t:m_Techniques)if(auto*s=t->GetPointShadowSampler())return s;
    return nullptr;
}

i32 ShadowSystem::GetShadowIndex(Entity light)const{
    for(usize i=0;i<m_AllEntities.size();++i)
        if(m_AllEntities[i]==light)return (i32)i;
    return -1;
}

float4x4 ShadowSystem::GetLightViewProj(u32 cascade)const{
    for(auto& t:m_Techniques){
        if(auto* csm=dynamic_cast<CSMTechnique*>(t.get()))
            return csm->GetLightViewProj(cascade);
    }
    return float4x4(1.0f);
}

} // namespace he::render
