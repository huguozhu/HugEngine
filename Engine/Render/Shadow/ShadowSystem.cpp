#include "Shadow/ShadowSystem.h"
#include "Shadow/CSMTechnique.h"
#include "Shadow/PointShadowTechnique.h"
#include "Shadow/SpotShadowTechnique.h"
#include "Shadow/RectLightShadowTechnique.h"
#include "Core/Log.h"
#include "Core/Assert.h"

namespace he::render {

bool ShadowSystem::Initialize(rhi::IRHIDevice* device,u32,u32){
    m_Device=device;
    HE_ASSERT(m_Device,"ShadowSystem: null device");

    // 注册默认技术
    auto csm=std::make_unique<CSMTechnique>();
    csm->Initialize(device);
    m_Techniques.push_back(std::move(csm));

    auto pt=std::make_unique<PointShadowTechnique>();
    pt->Initialize(device);
    m_Techniques.push_back(std::move(pt));

    auto spot=std::make_unique<SpotShadowTechnique>();
    spot->Initialize(device);
    m_Techniques.push_back(std::move(spot));

    auto rect=std::make_unique<RectLightShadowTechnique>();
    rect->Initialize(device);
    m_Techniques.push_back(std::move(rect));

    m_Ready=true;
    HE_CORE_INFO("ShadowSystem init ({} techniques)",m_Techniques.size());
    return true;
}

void ShadowSystem::CreateShadowPSO(rhi::DescriptorSetLayoutHandle layout){
    for(auto& t:m_Techniques){
        // 每个 Technique 创建自己的 PSO
        if(auto* csm=dynamic_cast<CSMTechnique*>(t.get())) csm->CreatePSO(layout);
        if(auto* pt=dynamic_cast<PointShadowTechnique*>(t.get())) pt->CreatePSO(layout);
        if(auto* spot=dynamic_cast<SpotShadowTechnique*>(t.get())) spot->CreatePSO(layout);
        if(auto* rect=dynamic_cast<RectLightShadowTechnique*>(t.get())) rect->CreatePSO(layout);
    }
}

void ShadowSystem::Shutdown(){
    for(auto& t:m_Techniques)t->Shutdown();
    m_Techniques.clear();
    m_Device=nullptr;
    m_Ready=false;
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
    m_CachedWorld=ctx.world;
    m_CachedSceneGraph=ctx.sceneGraph;
    // 【第③段第 2 批】收集改吃快照：`shadowLights` 由收集侧（`BuildShadowLights`）按
    // "enabled && castShadow" 一次取齐，四个技术只做"过滤自己那一类"。
    m_CachedSnapshot = ctx.snapshot;
    m_CachedRegistry = ctx.meshRegistry;
    if(!ctx.snapshot||!ctx.camera)return;

    m_AllShadowData.clear();
    m_AllEntities.clear();
    m_PerTechniqueCounts.clear();

    for(auto& t:m_Techniques){
        u32 n=t->CollectLights(*ctx.snapshot,*ctx.camera,m_AllShadowData,m_AllEntities);
        m_PerTechniqueCounts.push_back(n);
    }

    // 【容量钳制（必修）】阴影 SSBO 的容量是 `MAX_SHADOWS` 条（`Pipeline/Material.h` 的
    // `MAX_SHADOWS = kGPUMaxShadows`，默认 4），而**每个技术各自**最多收 `MAX_SHADOWS` 条
    // （见各技术的 `if(out.size()-start>=MAX_SHADOWS)break;`）⇒ 四个技术合计可达 4×容量。
    // 不钳制会有两处越界：① 本函数末尾的整段拷贝写越界（映射缓冲最多溢出 3×256 B）；
    // ② `GetShadowIndex` 用 `m_AllEntities` 的下标当 shader 的 `shadowIndex`，超容量的光源
    // 会让着色器的 `u_ShadowData[idx]`（固定 4 元素数组）越界读。
    // 截断策略：从**最后一个**技术开始扣，保证 `m_PerTechniqueCounts` 的前缀和仍与
    // `m_AllShadowData` 的分段一一对应（`Render` 的逐技术 offset 依赖这个不变量）。
    if(m_AllShadowData.size()>MAX_SHADOWS){
        u32 over=static_cast<u32>(m_AllShadowData.size())-MAX_SHADOWS;
        for(usize t=m_PerTechniqueCounts.size();t-->0&&over>0;){
            u32&n=m_PerTechniqueCounts[t];
            const u32 take=(n<over)?n:over;
            n-=take;
            over-=take;
        }
        m_AllShadowData.resize(MAX_SHADOWS);
        m_AllEntities.resize(MAX_SHADOWS);
        // 只提醒一次：这是"投影光源总数超过容量"的配置问题，不该每帧刷屏
        static bool s_warnedOverCapacity=false;
        if(!s_warnedOverCapacity){
            s_warnedOverCapacity=true;
            HE_CORE_WARN("ShadowSystem: 投影光源数超过阴影容量（上限 {}），已截断；"
                         "请减少投影光源数或提高 kGPUMaxShadows",MAX_SHADOWS);
        }
    }

    m_ActiveCount=(u32)m_AllShadowData.size();

    if(m_ActiveCount>0&&m_ExternalShadowBuffer){
        auto*dst=static_cast<GPUShadowData*>(m_ExternalShadowBuffer->Map());
        // 防御性再夹一次：即便上面的钳制被将来的改动绕过，也不会写越界
        const u32 n=(m_ActiveCount<MAX_SHADOWS)?m_ActiveCount:MAX_SHADOWS;
        for(u32 i=0;i<n;++i)dst[i]=m_AllShadowData[i];
        m_ExternalShadowBuffer->Unmap();
    }
}

// ============================================================================
// Render — 用 Update 中缓存的数据分别渲染每个 Technique
// ============================================================================

void ShadowSystem::SetFrameSnapshot(const FrameSceneSnapshot& snapshot, const MeshRegistry& registry){
    // 本帧快照与网格注册表：每帧无条件覆盖（与 Update 是否被调用无关）⇒ 不会留上一帧/上一槽的指针
    m_CachedSnapshot = &snapshot;
    m_CachedRegistry = &registry;
}

void ShadowSystem::Render(rhi::IRHICommandList* cmd){
    if(!m_Ready||!m_Enabled||!m_ActiveCount)return;
    if(!m_CachedSnapshot||!m_CachedRegistry)return;

    u32 offset=0;
    for(usize i=0;i<m_Techniques.size();++i){
        m_Techniques[i]->Render(cmd,*m_CachedSnapshot,*m_CachedRegistry,m_AllShadowData,offset);
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

// 本帧该阴影图是否真的被写入过。
// 每个 Technique 的 Render 只遍历「自己 CollectLights 收集到的那一段」，
// 因此「该技术本帧收集到光源数 > 0」就是「它的图本帧被写过」的充要条件。
bool ShadowSystem::WasShadowMapWritten(u32 index)const{
    // 关闭或未就绪时 Update 直接返回、不会重填计数，此时不能拿上一帧的计数当作本次已产出
    if(!m_Ready||!m_Enabled)return false;
    u32 off=0;
    for(usize i=0;i<m_Techniques.size();++i){
        u32 n=m_Techniques[i]->GetShadowMapCount();
        if(index<off+n)
            return i<m_PerTechniqueCounts.size()&&m_PerTechniqueCounts[i]>0;
        off+=n;
    }
    return false;
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

rhi::IRHITexture* ShadowSystem::GetSpotShadowMap()const{
    for(auto& t:m_Techniques)if(auto*s=dynamic_cast<SpotShadowTechnique*>(t.get()))return s->GetShadowMap(0);
    return nullptr;
}

rhi::IRHISampler* ShadowSystem::GetSpotShadowSampler()const{
    for(auto& t:m_Techniques)if(auto*s=dynamic_cast<SpotShadowTechnique*>(t.get()))return s->GetShadowSampler();
    return nullptr;
}

rhi::IRHITexture* ShadowSystem::GetRectShadowMap()const{
    for(auto& t:m_Techniques)if(auto*s=dynamic_cast<RectLightShadowTechnique*>(t.get()))return s->GetShadowMap(0);
    return nullptr;
}

rhi::IRHISampler* ShadowSystem::GetRectShadowSampler()const{
    for(auto& t:m_Techniques)if(auto*s=dynamic_cast<RectLightShadowTechnique*>(t.get()))return s->GetShadowSampler();
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
