#include "PostProcess/ToneMapPass.h"
#include "ToneMap.vert.spv.h"
#include "ToneMap.frag.spv.h"
#include "Core/Log.h"
#include "Core/Assert.h"

namespace he::render {

bool ToneMapPass::Initialize(rhi::IRHIDevice* device,u32 width,u32 height){
    m_Device=device;m_Width=width;m_Height=height;
    HE_ASSERT(m_Device,"ToneMapPass: null device");

    m_VS.stage=rhi::ShaderStage::Vertex;m_VS.spirv=k_ToneMap_vert_spv;m_VS.entryPoint="main";
    m_FS.stage=rhi::ShaderStage::Pixel;m_FS.spirv=k_ToneMap_frag_spv;m_FS.entryPoint="main";

    rhi::DescriptorSetLayoutDesc layout;layout.bindings={
        {0,rhi::DescriptorType::CombinedImageSampler,1,16},
    };
    m_DescLayout=device->CreateDescriptorSetLayout(layout);
    m_DescSet=device->AllocateDescriptorSet(m_DescLayout);

    rhi::PipelineStateDesc d;d.vertexShader=&m_VS;d.pixelShader=&m_FS;
    d.topology=rhi::PrimitiveTopology::TriangleList;
    d.depthTest=false;d.depthWrite=false;
    d.depthFormat=rhi::Format::D32_FLOAT;
    d.colorAttachmentCount=1;d.colorFormats[0]=rhi::Format::BGRA8_UNORM;
    rhi::PushConstantRange pcr; pcr.stageMask=1|16; pcr.size=16;  // Vertex|Fragment
    d.pushConstantRanges={pcr};
    d.descriptorSetLayouts={m_DescLayout};d.debugName="ToneMap";
    m_PSO=device->CreatePipelineState(d);
    HE_ASSERT(m_PSO,"ToneMapPass: PSO failed");

    m_Ready=true;
    HE_CORE_INFO("ToneMapPass init");
    return true;
}

void ToneMapPass::Shutdown(){
    if(m_Device&&m_DescLayout!=rhi::kInvalidLayout)m_Device->DestroyDescriptorSetLayout(m_DescLayout);
    m_PSO.reset();m_Device=nullptr;m_Ready=false;
}

void ToneMapPass::SetInput(rhi::IRHITexture* hdr,rhi::IRHISampler* sampler){
    if(hdr!=m_HDRTarget||sampler!=m_HDRSampler){
        m_HDRTarget=hdr;m_HDRSampler=sampler;
        if(m_HDRTarget&&m_HDRSampler)
            m_Device->UpdateDescriptorSet(m_DescSet,0,rhi::DescriptorType::CombinedImageSampler,m_HDRTarget,m_HDRSampler);
    }
}

void ToneMapPass::Render(rhi::IRHICommandList* cmd){
    if(!m_Ready||!m_Enabled)return;
    cmd->SetPipeline(m_PSO.get());
    cmd->SetViewport({0,(float)m_Height,(float)m_Width,-(float)m_Height,0,1});
    cmd->SetScissor({0,0,m_Width,m_Height});
    cmd->BindDescriptorSet(rhi::kDescSetPerFrame,m_DescSet);
    struct { float exposure; float _pad[3]; } pc;
    pc.exposure = m_Exposure;
    cmd->SetPushConstants(0, sizeof(pc), &pc);
    cmd->Draw(3);
}

void ToneMapPass::OnResize(u32 width,u32 height){
    if(width==m_Width&&height==m_Height)return;
    m_Width=width;m_Height=height;
}

} // namespace he::render
