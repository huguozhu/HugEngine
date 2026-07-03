#include "Shadow/SpotShadowTechnique.h"
#include "Pipeline/Camera.h"
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
#include "Core/Log.h"
#include "Core/Assert.h"

namespace he::render {

bool SpotShadowTechnique::Initialize(rhi::IRHIDevice* device){
    m_Device=device;
    m_ShadowVS.stage=rhi::ShaderStage::Vertex;m_ShadowVS.spirv=k_Shadow_vert_spv;m_ShadowVS.entryPoint="main";
    m_ShadowFS.stage=rhi::ShaderStage::Pixel;m_ShadowFS.spirv=k_Shadow_frag_spv;m_ShadowFS.entryPoint="main";

    // 单张 2D 深度纹理
    rhi::TextureDesc d;
    d.format=rhi::Format::D32_FLOAT;d.width=m_MapSize;d.height=m_MapSize;
    d.depth=1;d.mipLevels=1;d.arrayLayers=1;
    d.usage=rhi::TextureUsage::DepthStencil|rhi::TextureUsage::ShaderResource;
    m_SpotShadowMap=device->CreateTexture(d);

    rhi::SamplerDesc sd;
    sd.minFilter=rhi::FilterMode::Linear;sd.magFilter=rhi::FilterMode::Linear;
    sd.addressU=sd.addressV=rhi::AddressMode::ClampToEdge;
    m_SpotShadowSampler=device->CreateSampler(sd);

    HE_CORE_INFO("SpotShadowTechnique init ({}×{})", m_MapSize, m_MapSize);
    return true;
}

void SpotShadowTechnique::CreatePSO(rhi::DescriptorSetLayoutHandle layout){
    rhi::VertexInputLayout vl;vl.stride=sizeof(he::StaticVertex);
    vl.attributes={{0,0,rhi::VertexFormat::Float3,offsetof(he::StaticVertex,position)}};
    rhi::PushConstantRange pcr; pcr.stageMask=1|16;pcr.offset=0;pcr.size=sizeof(ShadowPushConstant);
    rhi::PipelineStateDesc d;d.vertexShader=&m_ShadowVS;d.pixelShader=&m_ShadowFS;
    d.vertexLayout=vl;d.topology=rhi::PrimitiveTopology::TriangleList;
    d.depthTest=d.depthWrite=true;d.depthCompare=rhi::CompareFunc::LessEqual;
    d.depthFormat=rhi::Format::D32_FLOAT;d.colorAttachmentCount=0;
    d.pushConstantRanges={pcr};d.descriptorSetLayouts={layout};d.debugName="SpotDepth";
    m_ShadowPSO=m_Device->CreatePipelineState(d);
    HE_ASSERT(m_ShadowPSO,"SpotShadowTechnique: PSO failed");
}

void SpotShadowTechnique::Shutdown(){
    m_SpotShadowMap.reset();m_SpotShadowSampler.reset();m_ShadowPSO.reset();
    m_Device=nullptr;
}

void SpotShadowTechnique::SetRenderResources(rhi::IRHIBuffer* ob,rhi::DescriptorSetHandle ds){
    m_ExternalObjectBuffer=ob;m_ExternalDescSet=ds;
}

u32 SpotShadowTechnique::CollectLights(he::World& w,he::SceneGraph& sg,const CameraData&,
                                        std::vector<GPUShadowData>& out,std::vector<he::Entity>& ent){
    u32 start=(u32)out.size();
    w.ForEach<he::SpotLight>([&](he::Entity e,he::SpotLight& lc){
        if(!lc.enabled||!lc.castShadow||out.size()-start>=MAX_SHADOWS)return;
        float3 lp=sg.GetWorldPosition(e);
        float3 dir=glm::normalize(lc.direction);
        float fov=lc.outerConeAngle*2.0f;
        float4x4 proj=glm::perspectiveRH_ZO(glm::radians(glm::degrees(fov)),1.0f,.1f,std::max(lc.range,.2f));
        float4x4 view=glm::lookAtRH(lp,lp+dir,float3(0,1,0));
        GPUShadowData sd{};
        sd.lightViewProj[0]=proj*view; // Spot 透视 VP
        sd.pointLightData=float4(lp,lc.range);
        sd.shadowParams=float4(lc.shadowBias,lc.shadowNormalBias,lc.shadowStrength, 2.0f); // 2=Spot
        out.push_back(sd);ent.push_back(e);
    });
    return (u32)(out.size()-start);
}

void SpotShadowTechnique::Render(rhi::IRHICommandList* cmd,he::World& w,he::SceneGraph&,
                                   const std::vector<GPUShadowData>& sd,u32 start){
    if(!m_SpotShadowMap||!m_ExternalObjectBuffer||m_ExternalDescSet==rhi::kInvalidSet)return;
    for(u32 li=start;li<(u32)sd.size()&&li-start<MAX_SHADOWS;++li){
        const auto& sm=sd[li];
        float3 lp(sm.pointLightData.x,sm.pointLightData.y,sm.pointLightData.z);
        float rng=sm.pointLightData.w;
        float outerAngle=sm.splitDistances.w;
        float3 dir=glm::normalize(float3(sm.splitDistances.x,sm.splitDistances.y,sm.splitDistances.z));

        // 聚光灯透视投影：FOV = outerConeAngle × 2
        float fov=outerAngle*2.0f;
        float4x4 proj=glm::perspectiveRH_ZO(glm::radians(glm::degrees(fov)),1.0f,.1f,std::max(rng,.2f));
        float4x4 view=glm::lookAtRH(lp,lp+dir,float3(0,1,0));
        float4x4 vp=proj*view;

        rhi::ClearValue cv{};cv.depth=1.f;
        void* depthView=m_SpotShadowMap->GetNativeHandle();
        cmd->SetPipeline(m_ShadowPSO.get());
        cmd->BeginOffscreenPass(nullptr,depthView,m_MapSize,m_MapSize,&cv);
        cmd->SetPipeline(m_ShadowPSO.get());
        cmd->SetViewport({0,(float)m_MapSize,(float)m_MapSize,-(float)m_MapSize,0,1});
        cmd->SetScissor({0,0,m_MapSize,m_MapSize});
        cmd->BindDescriptorSet(0,m_ExternalDescSet);
        u32 oi=0;
        auto rm=[&](he::Entity,he::MeshComponent& m){
            if(m.GetIndexCount()==0||oi>=MAX_OBJECTS)return;
            ShadowPushConstant pc{};pc.lightViewProj=vp;pc.objectIndex=oi++;
            cmd->SetPushConstants(0,sizeof(ShadowPushConstant),&pc);
            cmd->SetVertexBuffer(m.GetVertexBuffer().get(),0);cmd->SetIndexBuffer(m.GetIndexBuffer().get());
            cmd->DrawIndexed(m.GetIndexCount());
        };
        w.ForEach<he::MeshComponent>(rm);
        w.ForEach<he::CubeComponent>([&](he::Entity e,he::CubeComponent&c){rm(e,static_cast<he::MeshComponent&>(c));});
        w.ForEach<he::SphereComponent>([&](he::Entity e,he::SphereComponent&s){rm(e,static_cast<he::MeshComponent&>(s));});
        cmd->EndOffscreenPass();
        // barrier: depth write → shader read
        cmd->PipelineBarrier(rhi::PipelineStage::LateFragmentTests,rhi::PipelineStage::FragmentShader,
            rhi::ResourceState::DepthStencilWrite,rhi::ResourceState::DepthStencilRead,m_SpotShadowMap.get());
    }
}

} // namespace he::render
