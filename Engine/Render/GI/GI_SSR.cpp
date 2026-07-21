// GI/GI_SSR.cpp — 屏幕空间反射
#include "GI/GI_SSR.h"
#include "Core/Log.h"
#include "SSAO.vert.spv.h"
#include "SSR.frag.spv.h"
#include <glm/gtc/matrix_transform.hpp>

namespace he::render {

bool GI_SSR::Initialize(rhi::IRHIDevice* device, u32 width, u32 height) {
    m_Device=device; m_Width=width; m_Height=height;
    m_Settings.enabled=false; m_Settings.intensity=1.0f; m_Settings.mode=GIMode::SSGI;

    rhi::DescriptorSetLayoutDesc l;
    l.bindings={{0,rhi::DescriptorType::CombinedImageSampler,1,16},{1,rhi::DescriptorType::CombinedImageSampler,1,16},{2,rhi::DescriptorType::CombinedImageSampler,1,16},{3,rhi::DescriptorType::UniformBuffer,1,16}};
    m_DescLayout=device->CreateDescriptorSetLayout(l);
    m_DescSet=device->AllocateDescriptorSet(m_DescLayout);

    rhi::ShaderBytecode vs,fs;
    vs.stage=rhi::ShaderStage::Vertex; vs.spirv=k_SSAO_vert_spv; vs.entryPoint="vertexMain";
    fs.stage=rhi::ShaderStage::Pixel;  fs.spirv=k_SSR_frag_spv;  fs.entryPoint="fragmentMain";
    rhi::PipelineStateDesc d; d.vertexShader=&vs; d.pixelShader=&fs;
    d.topology=rhi::PrimitiveTopology::TriangleList; d.depthTest=false; d.depthWrite=false;
    d.depthFormat=rhi::Format::Unknown; d.colorAttachmentCount=1; d.colorFormats[0]=rhi::Format::RGBA16_FLOAT;
    d.descriptorSetLayouts={m_DescLayout}; d.debugName="SSR";
    m_PSO=device->CreatePipelineState(d);

    rhi::SamplerDesc sd; sd.minFilter=sd.magFilter=rhi::FilterMode::Nearest;
    sd.addressU=sd.addressV=rhi::AddressMode::ClampToEdge;
    m_PointSampler=device->CreateSampler(sd);
    CreateOutputTex(width,height);
    m_Ready=true; HE_CORE_INFO("GI_SSR initialized"); return true;
}

void GI_SSR::Shutdown(){m_PSO.reset();if(m_Device&&m_DescLayout!=rhi::kInvalidLayout)m_Device->DestroyDescriptorSetLayout(m_DescLayout);m_Output.reset();m_Sampler.reset();m_PointSampler.reset();m_Device=nullptr;m_Ready=false;}
void GI_SSR::OnResize(u32 w,u32 h){m_Width=w;m_Height=h;CreateOutputTex(w,h);}
void GI_SSR::CreateOutputTex(u32 w,u32 h){
    rhi::TextureDesc td; td.format=rhi::Format::RGBA16_FLOAT; td.width=w; td.height=h;
    td.mipLevels=1; td.usage=rhi::TextureUsage::RenderTarget|rhi::TextureUsage::ShaderResource;
    m_Output=m_Device->CreateTexture(td);
    rhi::SamplerDesc sd; sd.minFilter=sd.magFilter=rhi::FilterMode::Linear;
    sd.addressU=sd.addressV=rhi::AddressMode::ClampToEdge;
    m_Sampler=m_Device->CreateSampler(sd);
}
void GI_SSR::SetInputs(rhi::IRHITexture* depth, rhi::IRHITexture* normal, rhi::IRHITexture* albedo){
    m_Depth=depth; m_Normal=normal; m_Albedo=albedo;
    if(m_Depth) m_Device->UpdateDescriptorSet(m_DescSet,0,rhi::DescriptorType::CombinedImageSampler,m_Depth,m_PointSampler.get());
    if(m_Normal)m_Device->UpdateDescriptorSet(m_DescSet,1,rhi::DescriptorType::CombinedImageSampler,m_Normal,m_PointSampler.get());
    if(m_Albedo)m_Device->UpdateDescriptorSet(m_DescSet,2,rhi::DescriptorType::CombinedImageSampler,m_Albedo,m_PointSampler.get());
}
void GI_SSR::Render(rhi::IRHICommandList* cmd){
    if(!m_Ready||!m_Settings.enabled||!m_Depth||!m_Normal||!m_Albedo)return;
    cmd->SetPipeline(m_PSO.get()); cmd->BindDescriptorSet(rhi::kDescSetPerFrame,m_DescSet);
    cmd->SetViewport({0,(float)m_Height,(float)m_Width,-(float)m_Height,0,1});
    cmd->SetScissor({0,0,m_Width,m_Height});
    struct{float4x4 proj;float4 p;}pc;
    float a=float(m_Width)/float(m_Height);
    pc.proj=glm::inverse(glm::perspectiveRH_ZO(glm::radians(kDefaultFOV),a,kDefaultNearPlane,kDefaultFarPlane));
    pc.p=float4(maxSteps,stepSize,maxDistance,thickness);
    cmd->SetPushConstants(0,sizeof(pc),&pc);
    cmd->Draw(3);
}

} // namespace he::render
