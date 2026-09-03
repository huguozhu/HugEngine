#include "Shadow/RectLightShadowTechnique.h"
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
#include <cstdio>

namespace he::render {

bool RectLightShadowTechnique::Initialize(rhi::IRHIDevice* device){
    m_Device=device;
    m_ShadowVS.stage=rhi::ShaderStage::Vertex;m_ShadowVS.spirv=k_Shadow_vert_spv;m_ShadowVS.entryPoint="main";
    m_ShadowFS.stage=rhi::ShaderStage::Pixel;m_ShadowFS.spirv=k_Shadow_frag_spv;m_ShadowFS.entryPoint="main";

    rhi::TextureDesc d;
    d.format=rhi::Format::D32_FLOAT;d.width=m_MapSize;d.height=m_MapSize;
    d.depth=1;d.mipLevels=1;d.arrayLayers=1;
    d.usage=rhi::TextureUsage::DepthStencil|rhi::TextureUsage::ShaderResource;
    m_RectShadowMap=device->CreateTexture(d);

    rhi::SamplerDesc sd;
    sd.minFilter=rhi::FilterMode::Linear;sd.magFilter=rhi::FilterMode::Linear;
    sd.addressU=sd.addressV=rhi::AddressMode::ClampToEdge;
    m_RectShadowSampler=device->CreateSampler(sd);

    HE_CORE_INFO("RectLightShadowTechnique init ({}×{})", m_MapSize, m_MapSize);
    return true;
}

void RectLightShadowTechnique::CreatePSO(rhi::DescriptorSetLayoutHandle layout){
    rhi::VertexInputLayout vl;vl.stride=sizeof(he::StaticVertex);
    vl.attributes={{0,0,rhi::VertexFormat::Float3,offsetof(he::StaticVertex,position)}};
    rhi::PushConstantRange pcr; pcr.stageMask=rhi::kStageMaskVertex|rhi::kStageMaskFragment;pcr.offset=0;pcr.size=sizeof(ShadowPushConstant);
    rhi::PipelineStateDesc d;d.vertexShader=&m_ShadowVS;d.pixelShader=&m_ShadowFS;
    d.vertexLayout=vl;d.topology=rhi::PrimitiveTopology::TriangleList;
    d.depthTest=d.depthWrite=true;d.depthCompare=rhi::CompareFunc::LessEqual;
    d.depthFormat=rhi::Format::D32_FLOAT;d.colorAttachmentCount=0;
    d.pushConstantRanges={pcr};d.descriptorSetLayouts={layout};d.debugName="RectDepth";
    m_ShadowPSO=m_Device->CreatePipelineState(d);
    HE_ASSERT(m_ShadowPSO,"RectLightShadowTechnique: PSO failed");
}

void RectLightShadowTechnique::Shutdown(){
    m_RectShadowMap.reset();m_RectShadowSampler.reset();m_ShadowPSO.reset();
    m_Device=nullptr;
}

void RectLightShadowTechnique::SetRenderResources(rhi::IRHIBuffer* ob,rhi::DescriptorSetHandle ds){
    m_ExternalObjectBuffer=ob;m_ExternalDescSet=ds;
}

u32 RectLightShadowTechnique::CollectLights(he::World& w,he::SceneGraph& sg,const CameraData&,
                                             std::vector<GPUShadowData>& out,std::vector<he::Entity>& ent){
    u32 start=(u32)out.size();
    w.ForEach<he::RectLight>([&](he::Entity e,he::RectLight& lc){
        if(!lc.enabled||!lc.castShadow||out.size()-start>=MAX_SHADOWS)return;
        float3 lp=sg.GetWorldPosition(e);
        float3 n=glm::normalize(lc.normal);
        // 透视投影 FOV：覆盖矩形对角，近似用宽高/范围
        float halfDiag=0.5f*glm::length(float2(lc.width,lc.height));
        float fov=2.0f*glm::atan(halfDiag/glm::max(lc.range,halfDiag+0.1f));
        float4x4 proj=glm::perspectiveRH_ZO(fov,1.0f,.1f,std::max(lc.range,.2f));
        float4x4 view=glm::lookAtRH(lp,lp+n,float3(0,1,0));
        GPUShadowData sd{};
        sd.lightViewProj[0]=proj*view;                 // Rect 透视 VP
        sd.pointLightData=float4(lp,lc.range);
        sd.shadowParams=float4(lc.shadowBias,lc.shadowNormalBias,lc.shadowStrength,3.0f); // 3=Rect
        // 复用 splitDistances 存 法线 + 宽/高（供 Render 重建 + 主 pass 软阴影）
        sd.splitDistances=float4(n,0.0f);
        sd.splitDistances.w=lc.softness;               // 软阴影系数（PCF 半径缩放）
        out.push_back(sd);ent.push_back(e);
    });
    return (u32)(out.size()-start);
}

void RectLightShadowTechnique::Render(rhi::IRHICommandList* cmd,he::World& w,he::SceneGraph&,
                                       const std::vector<GPUShadowData>& sd,u32 start){
    if(!m_RectShadowMap||!m_ExternalObjectBuffer||m_ExternalDescSet==rhi::kInvalidSet)return;
    for(u32 li=start;li<(u32)sd.size()&&li-start<MAX_SHADOWS;++li){
        const auto& sm=sd[li];
        float3 lp(sm.pointLightData.x,sm.pointLightData.y,sm.pointLightData.z);
        float rng=sm.pointLightData.w;
        float3 n=glm::normalize(float3(sm.splitDistances.x,sm.splitDistances.y,sm.splitDistances.z));

        // 透视投影（CollectLights 已存好 lightViewProj[0]，直接复用）
        float4x4 vp=sm.lightViewProj[0];

        rhi::ClearValue cv{};cv.depth=1.f;
        void* depthView=m_RectShadowMap->GetNativeHandle();
        cmd->SetPipeline(m_ShadowPSO.get());
        cmd->BeginOffscreenPass(nullptr,depthView,m_MapSize,m_MapSize,&cv);
        cmd->SetPipeline(m_ShadowPSO.get());
        cmd->SetViewport({0,(float)m_MapSize,(float)m_MapSize,-(float)m_MapSize,0,1});
        cmd->SetScissor({0,0,m_MapSize,m_MapSize});
        cmd->BindDescriptorSet(rhi::kDescSetPerFrame,m_ExternalDescSet);
        u32 oi=0;
        auto rm=[&](he::Entity,he::MeshComponent& m){
            if(m.GetIndexCount()==0||oi>=MAX_OBJECTS)return;
            if(!m.castShadow)return;   // castShadow=false 不写入阴影
            char label[64];
            snprintf(label,sizeof(label),"Shadow R%u Obj#%u",li,oi);
            cmd->SetDrawDebugLabel(label);
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
            rhi::ResourceState::DepthStencilWrite,rhi::ResourceState::DepthStencilRead,m_RectShadowMap.get());
    }
}

} // namespace he::render
