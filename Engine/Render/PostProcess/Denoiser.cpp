// PostProcess/Denoiser.cpp — 双边模糊降噪
#include "PostProcess/Denoiser.h"
#include "Core/Log.h"
#include "SSAO.vert.spv.h"
#include "Denoise.frag.spv.h"

namespace he::render {

bool Denoiser::Initialize(rhi::IRHIDevice* device, u32 width, u32 height) {
    m_Device=device; m_Width=width; m_Height=height;
    rhi::DescriptorSetLayoutDesc l;
    l.bindings={{0,rhi::DescriptorType::CombinedImageSampler,1,16},{1,rhi::DescriptorType::CombinedImageSampler,1,16},{2,rhi::DescriptorType::CombinedImageSampler,1,16}};
    m_Layout=device->CreateDescriptorSetLayout(l);
    m_Set=device->AllocateDescriptorSet(m_Layout);

    rhi::ShaderBytecode vs,fs; vs.stage=rhi::ShaderStage::Vertex; vs.spirv=k_SSAO_vert_spv; vs.entryPoint="vertexMain";
    fs.stage=rhi::ShaderStage::Pixel; fs.spirv=k_Denoise_frag_spv; fs.entryPoint="fragmentMain";
    rhi::PipelineStateDesc d; d.vertexShader=&vs; d.pixelShader=&fs; d.topology=rhi::PrimitiveTopology::TriangleList;
    d.depthTest=false; d.depthWrite=false; d.depthFormat=rhi::Format::Unknown; d.colorAttachmentCount=1; d.colorFormats[0]=rhi::Format::RGBA16_FLOAT;
    d.descriptorSetLayouts={m_Layout}; d.debugName="Denoise";
    m_PSO=device->CreatePipelineState(d);

    rhi::SamplerDesc sd; sd.minFilter=sd.magFilter=rhi::FilterMode::Nearest;
    sd.addressU=sd.addressV=rhi::AddressMode::ClampToEdge;
    m_Sampler=device->CreateSampler(sd);
    OnResize(width,height);
    m_Ready=true; HE_CORE_INFO("Denoiser initialized"); return true;
}
void Denoiser::Shutdown(){m_PSO.reset();if(m_Device&&m_Layout)m_Device->DestroyDescriptorSetLayout(m_Layout);m_Denoised.reset();m_Sampler.reset();m_Device=nullptr;m_Ready=false;}
void Denoiser::OnResize(u32 w,u32 h){m_Width=w;m_Height=h;
    rhi::TextureDesc td; td.format=rhi::Format::RGBA16_FLOAT; td.width=w; td.height=h; td.mipLevels=1;
    td.usage=rhi::TextureUsage::RenderTarget|rhi::TextureUsage::ShaderResource;
    m_Denoised=m_Device->CreateTexture(td);
}

void Denoiser::SetInputs(rhi::IRHITexture* color, rhi::IRHITexture* depth, rhi::IRHITexture* normal){
    m_Input=color; m_Depth=depth; m_Normal=normal;
    if(color) m_Device->UpdateDescriptorSet(m_Set,0,rhi::DescriptorType::CombinedImageSampler,color,m_Sampler.get());
    if(depth) m_Device->UpdateDescriptorSet(m_Set,1,rhi::DescriptorType::CombinedImageSampler,depth,m_Sampler.get());
    if(normal)m_Device->UpdateDescriptorSet(m_Set,2,rhi::DescriptorType::CombinedImageSampler,normal,m_Sampler.get());
}
void Denoiser::Render(rhi::IRHICommandList* cmd){
    cmd->SetPipeline(m_PSO.get()); cmd->BindDescriptorSet(rhi::kDescSetPerFrame,m_Set);
    cmd->SetViewport({0,(float)m_Height,(float)m_Width,-(float)m_Height,0,1}); cmd->SetScissor({0,0,m_Width,m_Height});
    struct{float2 ts; float dS; float nS;}pc;
    pc.ts=float2(1.0f/m_Width,1.0f/m_Height); pc.dS=kDefaultDepthSigma; pc.nS=kDefaultNormalSigma;
    cmd->SetPushConstants(0,sizeof(pc),&pc);
    cmd->Draw(3);
}

} // namespace he::render
