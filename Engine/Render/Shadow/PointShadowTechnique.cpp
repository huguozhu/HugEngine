#include "Shadow/PointShadowTechnique.h"
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
#include <glm/ext/matrix_clip_space.hpp>
#include "Core/Log.h"
#include "Core/Assert.h"

namespace he::render {

struct CubemapFace{float3 dir,up;};
static const CubemapFace kCubeFaces[rhi::kCubemapFaceCount]={
    {{ 1, 0, 0},{0,-1, 0}},{{-1, 0, 0},{0,-1, 0}},{{ 0, 1, 0},{0, 0, 1}},
    {{ 0,-1, 0},{0, 0,-1}},{{ 0, 0, 1},{0,-1, 0}},{{ 0, 0,-1},{0,-1, 0}},
};

bool PointShadowTechnique::Initialize(rhi::IRHIDevice* device){
    m_Device=device;
    m_ShadowVS.stage=rhi::ShaderStage::Vertex;m_ShadowVS.spirv=k_Shadow_vert_spv;m_ShadowVS.entryPoint="main";
    m_ShadowFS.stage=rhi::ShaderStage::Pixel;m_ShadowFS.spirv=k_Shadow_frag_spv;m_ShadowFS.entryPoint="main";
    rhi::TextureDesc d;d.format=rhi::Format::D32_FLOAT;d.width=m_MapSize;d.height=m_MapSize;
    d.depth=1;d.mipLevels=1;d.arrayLayers=1;
    d.usage=rhi::TextureUsage::DepthStencil|rhi::TextureUsage::ShaderResource|rhi::TextureUsage::Cubemap;
    m_PointShadowMap=device->CreateTexture(d);
    rhi::SamplerDesc sd;sd.minFilter=rhi::FilterMode::Linear;sd.magFilter=rhi::FilterMode::Linear;
    sd.addressU=sd.addressV=sd.addressW=rhi::AddressMode::ClampToEdge;
    m_PointShadowSampler=device->CreateSampler(sd);
    return true;
}

void PointShadowTechnique::CreatePSO(rhi::DescriptorSetLayoutHandle layout){
    rhi::VertexInputLayout vl;vl.stride=sizeof(he::StaticVertex);
    vl.attributes={{0,0,rhi::VertexFormat::Float3,offsetof(he::StaticVertex,position)}};
    rhi::PushConstantRange pcr; pcr.stageMask=rhi::kStageMaskVertex|rhi::kStageMaskFragment;pcr.offset=0;pcr.size=sizeof(ShadowPushConstant);
    rhi::PipelineStateDesc d;d.vertexShader=&m_ShadowVS;d.pixelShader=&m_ShadowFS;
    d.vertexLayout=vl;d.topology=rhi::PrimitiveTopology::TriangleList;
    d.depthTest=d.depthWrite=true;d.depthCompare=rhi::CompareFunc::LessEqual;
    d.depthFormat=rhi::Format::D32_FLOAT;d.colorAttachmentCount=0;
    d.pushConstantRanges={pcr};d.descriptorSetLayouts={layout};d.debugName="PointDepth";
    m_ShadowPSO=m_Device->CreatePipelineState(d);
    HE_ASSERT(m_ShadowPSO,"PointShadowTechnique: PSO failed");
}

void PointShadowTechnique::Shutdown(){m_PointShadowMap.reset();m_PointShadowSampler.reset();m_ShadowPSO.reset();}
void PointShadowTechnique::SetRenderResources(rhi::IRHIBuffer* ob,rhi::DescriptorSetHandle ds){m_ExternalObjectBuffer=ob;m_ExternalDescSet=ds;}

u32 PointShadowTechnique::CollectLights(he::World& w,he::SceneGraph& sg,const CameraData&,
                                         std::vector<GPUShadowData>& out,std::vector<he::Entity>& ent){
    u32 start=(u32)out.size();
    w.ForEach<he::PointLight>([&](he::Entity e,he::PointLight& lc){
        if(!lc.enabled||!lc.castShadow||out.size()-start>=MAX_SHADOWS)return;
        float3 lp=sg.GetWorldPosition(e);
        GPUShadowData sd{};sd.pointLightData=float4(lp,lc.range);
        sd.shadowParams=float4(lc.shadowBias,lc.shadowNormalBias,lc.shadowStrength,1);
        out.push_back(sd);ent.push_back(e);
    });
    return (u32)(out.size()-start);
}

void PointShadowTechnique::Render(rhi::IRHICommandList* cmd,he::World& w,he::SceneGraph&,
                                   const std::vector<GPUShadowData>& sd,u32 start){
    if(!m_PointShadowMap||!m_ExternalObjectBuffer||m_ExternalDescSet==rhi::kInvalidSet)return;
    for(u32 li=start;li<(u32)sd.size()&&li-start<MAX_SHADOWS;++li){
        float3 lp(sd[li].pointLightData);float rng=sd[li].pointLightData.w;
        float4x4 proj=glm::perspectiveRH_ZO(glm::radians(90.f),1.f,.1f,rng);
        for(u32 face=0;face<6;++face){
            float4x4 view=glm::lookAtRH(lp,lp+kCubeFaces[face].dir,kCubeFaces[face].up);
            void*fv=m_PointShadowMap->GetNativeHandle(face);if(!fv)continue;
            rhi::ClearValue cv{};cv.depth=1.f;
            cmd->SetPipeline(m_ShadowPSO.get());cmd->BeginOffscreenPass(nullptr,fv,m_MapSize,m_MapSize,&cv);
            cmd->SetPipeline(m_ShadowPSO.get());
            cmd->SetViewport({0,(float)m_MapSize,(float)m_MapSize,-(float)m_MapSize,0,1});
            cmd->SetScissor({0,0,m_MapSize,m_MapSize});
            cmd->BindDescriptorSet(rhi::kDescSetPerFrame,m_ExternalDescSet);
            u32 oi=0;float4x4 vp=proj*view;
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
            cmd->PipelineBarrier(rhi::PipelineStage::LateFragmentTests,rhi::PipelineStage::FragmentShader,
                rhi::ResourceState::DepthStencilRead,rhi::ResourceState::ShaderResource,m_PointShadowMap.get());
        }
    }
}

} // namespace he::render
