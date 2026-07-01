#include "Shadow/ShadowSystem.h"
#include "Shadow.vert.spv.h"
#include "Shadow.frag.spv.h"
#include "Scene/World.h"
#include "Scene/SceneGraph.h"
#include "Scene/MeshComponent.h"
#include "Scene/LightComponent.h"
#include "Scene/Transform.h"
#include "Scene/CubeComponent.h"
#include "Scene/SphereComponent.h"
#include "Math/Math.h"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/ext/matrix_clip_space.hpp>
#include "Core/Log.h"
#include "Core/Assert.h"

namespace he::render {

float4x4 ShadowSystem::ComputeCascadeViewProj(
    const float3& lightDir, const CameraData& camera, float subNear, float subFar)
{
    float3 f = glm::normalize(camera.forward);
    float3 r = glm::normalize(glm::cross(f, camera.up));
    float3 u = glm::cross(r, f);
    float tanHalfFov = glm::tan(glm::radians(camera.fov) * 0.5f);
    float farH = tanHalfFov * subFar;
    float farW = farH * camera.aspectRatio;
    float3 farC = camera.position + f * subFar;
    float3 subCorners[4] = {
        farC - r*farW - u*farH, farC + r*farW - u*farH,
        farC - r*farW + u*farH, farC + r*farW + u*farH,
    };
    float3 sceneCenter(0,400,0);
    float3 lightUp = (abs(lightDir.y) < 0.999f) ? float3(0,1,0) : float3(1,0,0);
    float4x4 lightView = glm::lookAtRH(sceneCenter - lightDir*4000.f, sceneCenter, lightUp);
    float mnX=FLT_MAX,mxX=-FLT_MAX,mnY=FLT_MAX,mxY=-FLT_MAX;
    for(auto& c:subCorners){float4 ls=lightView*float4(c,1.f);mnX=glm::min(mnX,ls.x);mxX=glm::max(mxX,ls.x);mnY=glm::min(mnY,ls.y);mxY=glm::max(mxY,ls.y);}
    float h=glm::max(glm::max(-mnX,mxX),glm::max(-mnY,mxY));h=glm::max(h,200.f);
    return glm::orthoRH_ZO(-h,h,-h,h,0.1f,8000.f)*lightView;
}

struct CubemapFace{float3 dir,up;};
static const CubemapFace kCubeFaces[6]={
    {{ 1, 0, 0},{0,-1, 0}},{{-1, 0, 0},{0,-1, 0}},{{ 0, 1, 0},{0, 0, 1}},
    {{ 0,-1, 0},{0, 0,-1}},{{ 0, 0, 1},{0,-1, 0}},{{ 0, 0,-1},{0,-1, 0}},
};

// ============================================================================
// 生命周期
// ============================================================================

bool ShadowSystem::Initialize(rhi::IRHIDevice* device, u32, u32) {
    m_Device = device;
    HE_ASSERT(m_Device, "ShadowSystem: device null");

    m_ShadowVS.stage      = rhi::ShaderStage::Vertex;
    m_ShadowVS.spirv      = k_Shadow_vert_spv;
    m_ShadowVS.entryPoint = "main";
    m_ShadowFS.stage      = rhi::ShaderStage::Pixel;
    m_ShadowFS.spirv      = k_Shadow_frag_spv;
    m_ShadowFS.entryPoint = "main";

    for (u32 c = 0; c < CASCADE_COUNT; ++c) {
        rhi::TextureDesc d;
        d.format = rhi::Format::D32_FLOAT;
        d.width = m_ShadowMapSize; d.height = m_ShadowMapSize;
        d.depth = 1; d.mipLevels = 1; d.arrayLayers = 1;
        d.usage = rhi::TextureUsage::DepthStencil | rhi::TextureUsage::ShaderResource;
        m_ShadowMaps[c] = device->CreateTexture(d);
    }
    {
        rhi::SamplerDesc d;
        d.minFilter = rhi::FilterMode::Linear; d.magFilter = rhi::FilterMode::Linear;
        d.addressU = rhi::AddressMode::ClampToEdge; d.addressV = rhi::AddressMode::ClampToEdge;
        d.addressW = rhi::AddressMode::ClampToEdge;
        m_ShadowSampler = device->CreateSampler(d);
    }
    {
        rhi::TextureDesc d;
        d.format = rhi::Format::D32_FLOAT;
        d.width = m_PointShadowMapSize; d.height = m_PointShadowMapSize;
        d.depth = 1; d.mipLevels = 1; d.arrayLayers = 1;
        d.usage = rhi::TextureUsage::DepthStencil | rhi::TextureUsage::ShaderResource | rhi::TextureUsage::Cubemap;
        m_PointShadowMap = device->CreateTexture(d);
    }
    {
        rhi::SamplerDesc d;
        d.minFilter = rhi::FilterMode::Linear; d.magFilter = rhi::FilterMode::Linear;
        d.addressU = rhi::AddressMode::ClampToEdge; d.addressV = rhi::AddressMode::ClampToEdge;
        d.addressW = rhi::AddressMode::ClampToEdge;
        m_PointShadowSampler = device->CreateSampler(d);
    }

    m_Ready = true;
    HE_CORE_INFO("ShadowSystem init (CSM {}x{} x{}, Point {}x{} x6)",
        m_ShadowMapSize, m_ShadowMapSize, CASCADE_COUNT, m_PointShadowMapSize, m_PointShadowMapSize);
    return true;
}

void ShadowSystem::CreateShadowPSO(rhi::DescriptorSetLayoutHandle layout) {
    if (!m_Device) return;

    rhi::VertexInputLayout vl;
    vl.stride = sizeof(he::StaticVertex);
    vl.attributes = {{0, 0, rhi::VertexFormat::Float3, offsetof(he::StaticVertex, position)}};

    rhi::PushConstantRange pcr;
    pcr.stageMask = 1 | 16;
    pcr.offset = 0;
    pcr.size = sizeof(ShadowPushConstant);

    rhi::PipelineStateDesc psd;
    psd.vertexShader = &m_ShadowVS;
    psd.pixelShader = &m_ShadowFS;
    psd.vertexLayout = vl;
    psd.topology = rhi::PrimitiveTopology::TriangleList;
    psd.depthTest = true;
    psd.depthWrite = true;
    psd.depthCompare = rhi::CompareFunc::LessEqual;
    psd.depthFormat = rhi::Format::D32_FLOAT;
    psd.colorAttachmentCount = 0;
    psd.pushConstantRanges = {pcr};
    psd.descriptorSetLayouts = {layout};
    psd.debugName = "ShadowDepth";

    m_ShadowPSO = m_Device->CreatePipelineState(psd);
    HE_ASSERT(m_ShadowPSO, "ShadowSystem: PSO failed");
    HE_CORE_INFO("ShadowSystem: PSO created");
}

void ShadowSystem::Shutdown(){
    if(!m_Device)return;
    m_ShadowPSO.reset();
    for(u32 c=0;c<CASCADE_COUNT;++c)m_ShadowMaps[c].reset();
    m_ShadowSampler.reset();m_PointShadowMap.reset();m_PointShadowSampler.reset();
    m_Device=nullptr;m_Ready=false;
    HE_CORE_INFO("ShadowSystem shutdown");
}

void ShadowSystem::NextFrame(){m_CurrentFrameSlot=(m_CurrentFrameSlot+1)%MAX_FRAMES_IN_FLIGHT;}

void ShadowSystem::SetRenderResources(rhi::IRHIBuffer* objBuf, rhi::IRHIBuffer* shadowBuf, rhi::DescriptorSetHandle descSet){
    m_ExternalObjectBuffer=objBuf;m_ExternalShadowBuffer=shadowBuf;m_ExternalDescSet=descSet;
}

// ============================================================================
// Update
// ============================================================================

void ShadowSystem::Update(const SubsystemContext& ctx){
    if(!m_Ready||!m_Enabled)return;
    m_CachedWorld=ctx.world;m_CachedSceneGraph=ctx.sceneGraph;
    if(!ctx.world||!ctx.sceneGraph||!ctx.camera)return;
    m_ShadowGPUData.clear();m_PointShadowData.clear();
    m_ShadowEntities.clear();m_PointShadowEntities.clear();
    m_HasDirectionalShadows=m_HasPointShadows=false;
    CollectDirectionalShadows(*ctx.world,*ctx.sceneGraph,*ctx.camera);
    CollectPointShadows(*ctx.world,*ctx.sceneGraph);
    u32 total=static_cast<u32>(m_ShadowGPUData.size()+m_PointShadowData.size());
    if(total>0&&m_ExternalShadowBuffer){
        auto*dst=static_cast<GPUShadowData*>(m_ExternalShadowBuffer->Map());
        u32 idx=0;
        for(auto&sd:m_ShadowGPUData)dst[idx++]=sd;
        for(auto&sd:m_PointShadowData)dst[idx++]=sd;
        m_ExternalShadowBuffer->Unmap();
    }
}

void ShadowSystem::CollectDirectionalShadows(he::World& w,he::SceneGraph&,const CameraData& cam){
    w.ForEach<he::DirectionalLight>([&](he::Entity e,he::DirectionalLight& lc){
        if(!lc.enabled||!lc.castShadow||m_ShadowGPUData.size()>=MAX_SHADOWS)return;
        float3 ld=glm::normalize(lc.direction);
        GPUShadowData sd{};float lambda=0.5f;
        for(u32 c=0;c<CASCADE_COUNT;++c){
            float p=(c+1)/(float)CASCADE_COUNT;
            float ls=cam.nearPlane*std::pow(cam.farPlane/cam.nearPlane,p);
            float us=cam.nearPlane+(cam.farPlane-cam.nearPlane)*p;
            sd.splitDistances[c]=lambda*ls+(1-lambda)*us;
            sd.lightViewProj[c]=ComputeCascadeViewProj(ld,cam,
                c==0?cam.nearPlane:sd.splitDistances[c-1],sd.splitDistances[c]);
        }
        sd.splitDistances[3]=cam.farPlane;
        sd.cameraForward=float4(glm::normalize(cam.forward),0);
        sd.shadowParams=float4(lc.shadowBias,lc.shadowNormalBias,lc.shadowStrength,0);
        m_ShadowGPUData.push_back(sd);
        m_ShadowEntities.push_back(e);
    });
    m_HasDirectionalShadows=!m_ShadowGPUData.empty();
}

void ShadowSystem::CollectPointShadows(he::World& w,he::SceneGraph& sg){
    w.ForEach<he::PointLight>([&](he::Entity e,he::PointLight& lc){
        if(!lc.enabled||!lc.castShadow||m_ShadowGPUData.size()+m_PointShadowData.size()>=MAX_SHADOWS)return;
        float3 lp=sg.GetWorldPosition(e);
        GPUShadowData sd{};sd.pointLightData=float4(lp,lc.range);
        sd.shadowParams=float4(lc.shadowBias,lc.shadowNormalBias,lc.shadowStrength,1);
        m_PointShadowData.push_back(sd);
        m_PointShadowEntities.push_back(e);
    });
    m_HasPointShadows=!m_PointShadowData.empty();
}

// ============================================================================
// Render
// ============================================================================

void ShadowSystem::Render(rhi::IRHICommandList* cmd){
    if(!m_Ready||!m_Enabled)return;
    if(!m_HasDirectionalShadows&&!m_HasPointShadows)return;
    if(!m_CachedWorld||!m_CachedSceneGraph)return;
    if(!m_ExternalObjectBuffer||m_ExternalDescSet==rhi::kInvalidSet)return;
    auto&w=*m_CachedWorld;auto&sg=*m_CachedSceneGraph;

    if(m_HasDirectionalShadows&&!m_ShadowGPUData.empty()){
        RenderCSMCascade(cmd,0,w,sg);RenderCSMCascade(cmd,1,w,sg);RenderCSMCascade(cmd,2,w,sg);
        for(u32 c=0;c<CASCADE_COUNT;++c)
            cmd->PipelineBarrier(rhi::PipelineStage::LateFragmentTests,rhi::PipelineStage::FragmentShader,
                rhi::ResourceState::DepthStencilRead,rhi::ResourceState::ShaderResource,m_ShadowMaps[c].get());
    }
    if(m_HasPointShadows&&!m_PointShadowData.empty())RenderPointShadows(cmd,w,sg);
}

void ShadowSystem::RenderCSMCascade(rhi::IRHICommandList* cmd,u32 ci,he::World& w,he::SceneGraph& sg){
    void*dv=m_ShadowMaps[ci]->GetNativeHandle();if(!dv)return;
    rhi::ClearValue cv{};cv.depth=1.f;
    cmd->SetPipeline(m_ShadowPSO.get());
    cmd->BeginOffscreenPass(nullptr,dv,m_ShadowMapSize,m_ShadowMapSize,&cv);
    cmd->SetPipeline(m_ShadowPSO.get());
    cmd->SetViewport({0,(float)m_ShadowMapSize,(float)m_ShadowMapSize,-(float)m_ShadowMapSize,0,1});
    cmd->SetScissor({0,0,m_ShadowMapSize,m_ShadowMapSize});
    cmd->BindDescriptorSet(0,m_ExternalDescSet);
    const GPUShadowData&sm=m_ShadowGPUData[0];
    auto*objData=static_cast<GPUObjectData*>(m_ExternalObjectBuffer->Map());
    u32 oi=0;
    auto rm=[&](he::Entity e,he::MeshComponent& m){
        if(m.GetIndexCount()==0||oi>=MAX_OBJECTS)return;
        objData[oi].worldMatrix=sg.GetWorldMatrix(e);
        ShadowPushConstant pc{};pc.lightViewProj=sm.lightViewProj[ci];pc.objectIndex=oi++;
        cmd->SetPushConstants(0,sizeof(ShadowPushConstant),&pc);
        cmd->SetVertexBuffer(m.GetVertexBuffer().get(),0);
        cmd->SetIndexBuffer(m.GetIndexBuffer().get());
        cmd->DrawIndexed(m.GetIndexCount());
    };
    w.ForEach<he::MeshComponent>(rm);
    w.ForEach<he::CubeComponent>([&](he::Entity e,he::CubeComponent&c){rm(e,static_cast<he::MeshComponent&>(c));});
    w.ForEach<he::SphereComponent>([&](he::Entity e,he::SphereComponent&s){rm(e,static_cast<he::MeshComponent&>(s));});
    m_ExternalObjectBuffer->Unmap();
    cmd->EndOffscreenPass();
}

void ShadowSystem::RenderPointShadows(rhi::IRHICommandList* cmd,he::World& w,he::SceneGraph&){
    if(!m_PointShadowMap)return;
    for(usize li=0;li<m_PointShadowData.size();++li){
        const GPUShadowData&sd=m_PointShadowData[li];
        float3 lp(sd.pointLightData);float rng=sd.pointLightData.w;
        float4x4 proj=glm::perspectiveRH_ZO(glm::radians(90.f),1.f,.1f,rng);
        for(u32 face=0;face<6;++face){
            float4x4 view=glm::lookAtRH(lp,lp+kCubeFaces[face].dir,kCubeFaces[face].up);
            void*fv=m_PointShadowMap->GetNativeHandle(face);if(!fv)continue;
            rhi::ClearValue cv{};cv.depth=1.f;
            cmd->SetPipeline(m_ShadowPSO.get());
            cmd->BeginOffscreenPass(nullptr,fv,m_PointShadowMapSize,m_PointShadowMapSize,&cv);
            cmd->SetPipeline(m_ShadowPSO.get());
            cmd->SetViewport({0,(float)m_PointShadowMapSize,(float)m_PointShadowMapSize,-(float)m_PointShadowMapSize,0,1});
            cmd->SetScissor({0,0,m_PointShadowMapSize,m_PointShadowMapSize});
            cmd->BindDescriptorSet(0,m_ExternalDescSet);
            u32 oi=0;float4x4 vp=proj*view;
            auto rm=[&](he::Entity,he::MeshComponent& m){
                if(m.GetIndexCount()==0||oi>=MAX_OBJECTS)return;
                ShadowPushConstant pc{};pc.lightViewProj=vp;pc.objectIndex=oi++;
                cmd->SetPushConstants(0,sizeof(ShadowPushConstant),&pc);
                cmd->SetVertexBuffer(m.GetVertexBuffer().get(),0);
                cmd->SetIndexBuffer(m.GetIndexBuffer().get());
                cmd->DrawIndexed(m.GetIndexCount());
            };
            w.ForEach<he::MeshComponent>(rm);
            w.ForEach<he::CubeComponent>([&](he::Entity e,he::CubeComponent&c){rm(e,static_cast<he::MeshComponent&>(c));});
            w.ForEach<he::SphereComponent>([&](he::Entity e,he::SphereComponent&s){rm(e,static_cast<he::MeshComponent&>(s));});
            cmd->EndOffscreenPass();
            cmd->PipelineBarrier(rhi::PipelineStage::LateFragmentTests,rhi::PipelineStage::FragmentShader,
                rhi::ResourceState::DepthStencilRead,rhi::ResourceState::ShaderResource,m_PointShadowMap.get());
        }
    }
}

void ShadowSystem::Bind(rhi::IRHICommandList*)const{}
void ShadowSystem::OnResize(u32,u32){}

rhi::IRHITexture* ShadowSystem::GetShadowMap(u32 i)const{
    HE_ASSERT(i<CASCADE_COUNT,"shadow map index out of range");
    return m_ShadowMaps[i].get();
}

i32 ShadowSystem::GetShadowIndex(he::Entity lightEntity)const{
    // 先在方向光列表中查找
    for(usize i=0;i<m_ShadowEntities.size();++i){
        if(m_ShadowEntities[i]==lightEntity)return static_cast<i32>(i);
    }
    // 再在点光源列表中查找（索引偏移方向光数量）
    for(usize i=0;i<m_PointShadowEntities.size();++i){
        if(m_PointShadowEntities[i]==lightEntity)return static_cast<i32>(m_ShadowEntities.size()+i);
    }
    return -1; // 该光源未投射阴影
}

} // namespace he::render
