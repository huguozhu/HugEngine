#include "Shadow/CSMTechnique.h"
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

float4x4 CSMTechnique::ComputeCascadeViewProj(const float3& ld,const CameraData& cam,float sn,float sf){
    // 计算相机视锥体的级联子视锥体 8 个角点（世界空间）
    float3 f=glm::normalize(cam.forward),r=glm::normalize(glm::cross(f,cam.up)),u=glm::cross(r,f);
    float th=glm::tan(glm::radians(cam.fov)*.5f);
    float fh_near=th*sn, fw_near=fh_near*cam.aspectRatio;  // 近平面半高/半宽
    float fh_far =th*sf, fw_far =fh_far *cam.aspectRatio;  // 远平面半高/半宽
    float3 nc=cam.position+f*sn;  // 近平面中心
    float3 fc=cam.position+f*sf;  // 远平面中心

    // 8 个视锥体角点
    float3 corners[8]={
        nc-r*fw_near-u*fh_near, nc+r*fw_near-u*fh_near,
        nc-r*fw_near+u*fh_near, nc+r*fw_near+u*fh_near,
        fc-r*fw_far -u*fh_far,  fc+r*fw_far -u*fh_far,
        fc-r*fw_far +u*fh_far,  fc+r*fw_far +u*fh_far,
    };

    // 场景中心 = 视锥体角点平均值（动态计算，替代硬编码）
    float3 scn=float3(0,0,0);
    for(int i=0;i<8;++i) scn+=corners[i];
    scn/=8.0f;

    // 光源视图矩阵：从光源方向观察场景中心
    float3 lu=(abs(ld.y)<.999f)?float3(0,1,0):float3(1,0,0);
    float4x4 lv=glm::lookAtRH(scn-ld*4000.f,scn,lu);

    // 将所有角点变换到光源视图空间，计算包围盒
    float mnX=FLT_MAX,mxX=-FLT_MAX,mnY=FLT_MAX,mxY=-FLT_MAX;
    for(auto&c:corners){float4 ls=lv*float4(c,1.f);mnX=glm::min(mnX,ls.x);mxX=glm::max(mxX,ls.x);mnY=glm::min(mnY,ls.y);mxY=glm::max(mxY,ls.y);}
    float h=glm::max(glm::max(-mnX,mxX),glm::max(-mnY,mxY));h=glm::max(h,200.f);
    return glm::orthoRH_ZO(-h,h,-h,h,.1f,8000.f)*lv;
}

bool CSMTechnique::Initialize(rhi::IRHIDevice* device){
    m_Device=device;
    m_ShadowVS.stage=rhi::ShaderStage::Vertex;m_ShadowVS.spirv=k_Shadow_vert_spv;m_ShadowVS.entryPoint="main";
    m_ShadowFS.stage=rhi::ShaderStage::Pixel;m_ShadowFS.spirv=k_Shadow_frag_spv;m_ShadowFS.entryPoint="main";
    for(u32 c=0;c<CASCADE_COUNT;++c){
        rhi::TextureDesc d;d.format=rhi::Format::D32_FLOAT;d.width=m_ShadowMapSize;d.height=m_ShadowMapSize;
        d.depth=1;d.mipLevels=1;d.arrayLayers=1;
        d.usage=rhi::TextureUsage::DepthStencil|rhi::TextureUsage::ShaderResource;
        m_ShadowMaps[c]=device->CreateTexture(d);
    }
    rhi::SamplerDesc sd;sd.minFilter=rhi::FilterMode::Linear;sd.magFilter=rhi::FilterMode::Linear;
    sd.addressU=sd.addressV=sd.addressW=rhi::AddressMode::ClampToEdge;
    m_ShadowSampler=device->CreateSampler(sd);
    return true;
}

void CSMTechnique::CreatePSO(rhi::DescriptorSetLayoutHandle layout){
    rhi::VertexInputLayout vl;vl.stride=sizeof(he::StaticVertex);
    vl.attributes={{0,0,rhi::VertexFormat::Float3,offsetof(he::StaticVertex,position)}};
    rhi::PushConstantRange pcr; pcr.stageMask=rhi::kStageMaskVertex|rhi::kStageMaskFragment;pcr.offset=0;pcr.size=sizeof(ShadowPushConstant);
    rhi::PipelineStateDesc d;d.vertexShader=&m_ShadowVS;d.pixelShader=&m_ShadowFS;
    d.vertexLayout=vl;d.topology=rhi::PrimitiveTopology::TriangleList;
    d.depthTest=d.depthWrite=true;d.depthCompare=rhi::CompareFunc::LessEqual;
    d.depthFormat=rhi::Format::D32_FLOAT;d.colorAttachmentCount=0;
    d.pushConstantRanges={pcr};d.descriptorSetLayouts={layout};d.debugName="CSMDepth";
    m_ShadowPSO=m_Device->CreatePipelineState(d);
    HE_ASSERT(m_ShadowPSO,"CSMTechnique: PSO failed");
}

void CSMTechnique::Shutdown(){for(u32 c=0;c<CASCADE_COUNT;++c)m_ShadowMaps[c].reset();m_ShadowSampler.reset();m_ShadowPSO.reset();}
void CSMTechnique::SetRenderResources(rhi::IRHIBuffer* ob,rhi::DescriptorSetHandle ds){m_ExternalObjectBuffer=ob;m_ExternalDescSet=ds;}

u32 CSMTechnique::CollectLights(he::World& w,he::SceneGraph&,const CameraData& cam,
                                 std::vector<GPUShadowData>& out,std::vector<he::Entity>& ent){
    u32 start=(u32)out.size();
    w.ForEach<he::DirectionalLight>([&](he::Entity e,he::DirectionalLight& lc){
        if(!lc.enabled||!lc.castShadow||out.size()-start>=MAX_SHADOWS)return;
        float3 ld=glm::normalize(lc.direction);GPUShadowData sd{};float la=.5f;
        for(u32 c=0;c<CASCADE_COUNT;++c){
            float p=(c+1)/(float)CASCADE_COUNT;
            float ls=cam.nearPlane*std::pow(cam.farPlane/cam.nearPlane,p);
            float us=cam.nearPlane+(cam.farPlane-cam.nearPlane)*p;
            sd.splitDistances[c]=la*ls+(1.f-la)*us;
            sd.lightViewProj[c]=ComputeCascadeViewProj(ld,cam,c==0?cam.nearPlane:sd.splitDistances[c-1],sd.splitDistances[c]);
        }
        sd.splitDistances[3]=cam.farPlane;sd.cameraForward=float4(glm::normalize(cam.forward),0);
        sd.shadowParams=float4(lc.shadowBias,lc.shadowNormalBias,lc.shadowStrength,0);
        out.push_back(sd);ent.push_back(e);
    });
    return (u32)(out.size()-start);
}

void CSMTechnique::Render(rhi::IRHICommandList* cmd,he::World& w,he::SceneGraph& sg,
                           const std::vector<GPUShadowData>& sd,u32 start){
    if(sd.empty()||!m_ExternalObjectBuffer||m_ExternalDescSet==rhi::kInvalidSet)return;
    for(u32 c=0;c<CASCADE_COUNT;++c)RenderCascade(cmd,c,w,sg,sd[start]);
    for(u32 c=0;c<CASCADE_COUNT;++c)
        cmd->PipelineBarrier(rhi::PipelineStage::LateFragmentTests,rhi::PipelineStage::FragmentShader,
            rhi::ResourceState::DepthStencilRead,rhi::ResourceState::ShaderResource,m_ShadowMaps[c].get());
}

void CSMTechnique::RenderCascade(rhi::IRHICommandList* cmd,u32 ci,he::World& w,he::SceneGraph& sg,const GPUShadowData& sd){
    m_LightVPs[ci] = sd.lightViewProj[ci];  // 缓存供 RSM 查询
    void*dv=m_ShadowMaps[ci]->GetNativeHandle();if(!dv)return;
    rhi::ClearValue cv{};cv.depth=1.f;
    cmd->SetPipeline(m_ShadowPSO.get());cmd->BeginOffscreenPass(nullptr,dv,m_ShadowMapSize,m_ShadowMapSize,&cv);
    cmd->SetPipeline(m_ShadowPSO.get());
    cmd->SetViewport({0,(float)m_ShadowMapSize,(float)m_ShadowMapSize,-(float)m_ShadowMapSize,0,1});
    cmd->SetScissor({0,0,m_ShadowMapSize,m_ShadowMapSize});
    cmd->BindDescriptorSet(rhi::kDescSetPerFrame,m_ExternalDescSet);
    auto*objData=static_cast<GPUObjectData*>(m_ExternalObjectBuffer->Map());
    u32 oi=0;
    auto rm=[&](he::Entity e,he::MeshComponent& m){
        if(m.GetIndexCount()==0||oi>=MAX_OBJECTS)return;
        objData[oi].worldMatrix=sg.GetWorldMatrix(e);
        ShadowPushConstant pc{};pc.lightViewProj=sd.lightViewProj[ci];pc.objectIndex=oi++;
        cmd->SetPushConstants(0,sizeof(ShadowPushConstant),&pc);
        cmd->SetVertexBuffer(m.GetVertexBuffer().get(),0);cmd->SetIndexBuffer(m.GetIndexBuffer().get());
        cmd->DrawIndexed(m.GetIndexCount());
    };
    w.ForEach<he::MeshComponent>(rm);
    w.ForEach<he::CubeComponent>([&](he::Entity e,he::CubeComponent&c){rm(e,static_cast<he::MeshComponent&>(c));});
    w.ForEach<he::SphereComponent>([&](he::Entity e,he::SphereComponent&s){rm(e,static_cast<he::MeshComponent&>(s));});
    m_ExternalObjectBuffer->Unmap();cmd->EndOffscreenPass();
}

rhi::IRHITexture* CSMTechnique::GetShadowMap(u32 i)const{return i<CASCADE_COUNT?m_ShadowMaps[i].get():nullptr;}

} // namespace he::render
