// PostProcess/SSGI.cpp — 屏幕空间全局光照
#include "PostProcess/SSGI.h"
#include "Core/Log.h"
#include "SSAO.vert.spv.h"
#include "SSGI.frag.spv.h"
#include <glm/gtc/matrix_transform.hpp>
#include <random>
#include <cstring>

namespace he::render {

static void GenSSGISamples(std::vector<float4>& kernel, int count) {
    kernel.resize(32);
    std::default_random_engine gen(42);
    std::uniform_real_distribution<float> rnd(0,1);
    for(int i=0;i<count&&i<32;i++){
        float3 s(rnd(gen)*2-1,rnd(gen)*2-1,rnd(gen));
        s=glm::normalize(s)*rnd(gen);
        float scale=float(i)/float(count);scale=glm::mix(0.1f,1.0f,scale*scale);
        kernel[i]=float4(s*scale,0);
    }
}

bool GI_SSGI::Initialize(rhi::IRHIDevice* device, u32 width, u32 height) {
    m_Device=device;m_Width=width;m_Height=height;
    m_Settings.enabled=true; m_Settings.intensity=1.0f; m_Settings.mode=GIMode::SSGI;

    rhi::DescriptorSetLayoutDesc l;
    l.bindings={{0,rhi::DescriptorType::CombinedImageSampler,1,16},{1,rhi::DescriptorType::CombinedImageSampler,1,16},{2,rhi::DescriptorType::CombinedImageSampler,1,16},{3,rhi::DescriptorType::UniformBuffer,1,16}};
    m_DescLayout=device->CreateDescriptorSetLayout(l);
    m_DescSet=device->AllocateDescriptorSet(m_DescLayout);

    rhi::ShaderBytecode vs,fs;vs.stage=rhi::ShaderStage::Vertex;vs.spirv=k_SSAO_vert_spv;vs.entryPoint="vertexMain";
    fs.stage=rhi::ShaderStage::Pixel;fs.spirv=k_SSGI_frag_spv;fs.entryPoint="fragmentMain";
    rhi::PipelineStateDesc d;d.vertexShader=&vs;d.pixelShader=&fs;d.topology=rhi::PrimitiveTopology::TriangleList;
    d.depthTest=false;d.depthWrite=false;d.depthFormat=rhi::Format::Unknown;
    d.colorAttachmentCount=1;d.colorFormats[0]=rhi::Format::RGBA16_FLOAT;
    d.descriptorSetLayouts={m_DescLayout};d.debugName="SSGI";
    m_PSO=device->CreatePipelineState(d);

    rhi::SamplerDesc sd;sd.minFilter=sd.magFilter=rhi::FilterMode::Nearest;
    sd.addressU=sd.addressV=rhi::AddressMode::ClampToEdge;
    m_PointSampler=device->CreateSampler(sd);

    CreateOutputTex(width,height);
    m_Ready=true;HE_CORE_INFO("SSGI initialized");return true;
}

void GI_SSGI::Shutdown(){m_PSO.reset();if(m_Device&&m_DescLayout!=rhi::kInvalidLayout)m_Device->DestroyDescriptorSetLayout(m_DescLayout);m_Output.reset();m_Sampler.reset();m_PointSampler.reset();m_Device=nullptr;m_Ready=false;}

void GI_SSGI::OnResize(u32 w,u32 h){m_Width=w;m_Height=h;CreateOutputTex(w,h);}

void GI_SSGI::CreateOutputTex(u32 w,u32 h){
    rhi::TextureDesc td;td.format=rhi::Format::RGBA16_FLOAT;td.width=w;td.height=h;
    td.mipLevels=1;td.usage=rhi::TextureUsage::RenderTarget|rhi::TextureUsage::ShaderResource;
    m_Output=m_Device->CreateTexture(td);
    rhi::SamplerDesc sd;sd.minFilter=sd.magFilter=rhi::FilterMode::Linear;
    sd.addressU=sd.addressV=rhi::AddressMode::ClampToEdge;
    m_Sampler=m_Device->CreateSampler(sd);
}

void GI_SSGI::SetInputs(rhi::IRHITexture* depth, rhi::IRHITexture* normal, rhi::IRHITexture* albedo){
    m_Depth=depth;m_Normal=normal;m_Albedo=albedo;
    if(m_Depth)m_Device->UpdateDescriptorSet(m_DescSet,0,rhi::DescriptorType::CombinedImageSampler,m_Depth,m_PointSampler.get());
    if(m_Normal)m_Device->UpdateDescriptorSet(m_DescSet,1,rhi::DescriptorType::CombinedImageSampler,m_Normal,m_PointSampler.get());
    if(m_Albedo)m_Device->UpdateDescriptorSet(m_DescSet,2,rhi::DescriptorType::CombinedImageSampler,m_Albedo,m_PointSampler.get());
}

void GI_SSGI::Render(rhi::IRHICommandList* cmd){
    if(!m_Ready||!m_Settings.enabled||!m_Depth||!m_Normal||!m_Albedo)return;
    cmd->SetPipeline(m_PSO.get());cmd->BindDescriptorSet(0,m_DescSet);
    cmd->SetViewport({0,(float)m_Height,(float)m_Width,-(float)m_Height,0,1});
    cmd->SetScissor({0,0,m_Width,m_Height});

    static std::vector<float4> kernel; if(kernel.empty())GenSSGISamples(kernel,32);
    struct{float4 k[32];float4 p;float4x4 proj;}pc;memcpy(pc.k,kernel.data(),32*sizeof(float4));
    pc.p=float4(radius, m_Settings.intensity, float(sampleCount), 0);
    float a=float(m_Width)/float(m_Height);
    pc.proj=glm::inverse(glm::perspectiveRH_ZO(glm::radians(60.0f),a,0.1f,1000.0f));
    cmd->SetPushConstants(0,sizeof(pc),&pc);
    cmd->Draw(3);
}

} // namespace he::render
