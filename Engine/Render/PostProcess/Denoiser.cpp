// PostProcess/Denoiser.cpp — 双边模糊降噪
#include "PostProcess/Denoiser.h"
#include "Core/Log.h"
#include "SSAO.vert.spv.h"
#include "Denoise.frag.spv.h"

namespace he::render {

// Denoiser 描述符集绑定号（与 Denoise.frag 的 vk::binding 一致）
static constexpr u32 kDenoiseBindColor = 0;
static constexpr u32 kDenoiseBindDepth = 1;
static constexpr u32 kDenoiseBindNormal = 2;

bool Denoiser::Initialize(rhi::IRHIDevice* device, u32 width, u32 height) {
    m_Device=device;
    m_Width=width;
    m_Height=height;
    rhi::DescriptorSetLayoutDesc l;
    l.bindings={{0,rhi::DescriptorType::CombinedImageSampler,1,16},{1,rhi::DescriptorType::CombinedImageSampler,1,16},{2,rhi::DescriptorType::CombinedImageSampler,1,16}};
    m_Layout=device->CreateDescriptorSetLayout(l);
    m_Set=device->AllocateDescriptorSet(m_Layout);

    rhi::ShaderBytecode vs,fs;
    vs.stage=rhi::ShaderStage::Vertex;
    vs.spirv=k_SSAO_vert_spv;
    vs.entryPoint="vertexMain";
    fs.stage=rhi::ShaderStage::Pixel;
    fs.spirv=k_Denoise_frag_spv;
    fs.entryPoint="fragmentMain";
    rhi::PipelineStateDesc d;
    d.vertexShader=&vs;
    d.pixelShader=&fs;
    d.topology=rhi::PrimitiveTopology::TriangleList;
    d.depthTest=false;
    d.depthWrite=false;
    d.depthFormat=rhi::Format::Unknown;
    d.colorAttachmentCount=1;
    d.colorFormats[0]=rhi::Format::RGBA16_FLOAT;
    d.descriptorSetLayouts={m_Layout}; d.debugName="Denoise";
    m_PSO=device->CreatePipelineStateHandle(d);

    rhi::SamplerDesc sd;
    sd.minFilter=sd.magFilter=rhi::FilterMode::Nearest;
    sd.addressU=sd.addressV=rhi::AddressMode::ClampToEdge;
    m_Sampler=device->CreateSamplerHandle(sd);
    OnResize(width,height);
    m_Ready=true;
    HE_CORE_INFO("Denoiser initialized");
    return true;
}
void Denoiser::Shutdown(){
    // 【空保护】m_Device 可能为空（Shutdown 被调用两次，或 Initialize 失败后进入清理路径），
    // 直接解引用会崩溃（实测：02.Cube 退出时连续崩在多个 Shutdown 里）。
    if (!m_Device) return;
m_Device->DestroyPipelineStateHandle(m_PSO); m_PSO = {};if(m_Device&&m_Layout)m_Device->DestroyDescriptorSetLayout(m_Layout);m_Denoised.reset();m_Device->DestroySamplerHandle(m_Sampler); m_Sampler = {};m_Device=nullptr;m_Ready=false;}
void Denoiser::OnResize(u32 w,u32 h){m_Width=w;m_Height=h;
    rhi::TextureDesc td;
    td.format=rhi::Format::RGBA16_FLOAT;
    td.width=w;
    td.height=h;
    td.mipLevels=1;
    td.usage=rhi::TextureUsage::RenderTarget|rhi::TextureUsage::ShaderResource;
    m_Denoised=m_Device->CreateTexture(td);
}

void Denoiser::SetInputs(rhi::IRHITexture* color, rhi::IRHITexture* depth, rhi::IRHITexture* normal){
    m_Input=color;
    m_Depth=depth;
    m_Normal=normal;
    if(color) m_Device->UpdateDescriptorSet(m_Set,kDenoiseBindColor,rhi::DescriptorType::CombinedImageSampler,color,ResS(m_Sampler));
    if(depth) m_Device->UpdateDescriptorSet(m_Set,kDenoiseBindDepth,rhi::DescriptorType::CombinedImageSampler,depth,ResS(m_Sampler));
    if(normal)m_Device->UpdateDescriptorSet(m_Set,kDenoiseBindNormal,rhi::DescriptorType::CombinedImageSampler,normal,ResS(m_Sampler));
}
void Denoiser::Render(rhi::IRHICommandList* cmd){
    cmd->SetPipeline(ResP(m_PSO));
    cmd->BindDescriptorSet(rhi::kDescSetPerFrame,m_Set);
    cmd->SetViewport({0,(float)m_Height,(float)m_Width,-(float)m_Height,0,1}); cmd->SetScissor({0,0,m_Width,m_Height});
    struct{float2 ts; float dS; float nS;}pc;
    pc.ts=float2(1.0f/m_Width,1.0f/m_Height);
    pc.dS=m_DepthSigma;      // 可配：按信号类型区分（见 Denoiser.h 文件头）
    pc.nS=m_NormalSigma;
    cmd->SetPushConstants(0,sizeof(pc),&pc);
    cmd->Draw(3);
}

} // namespace he::render
