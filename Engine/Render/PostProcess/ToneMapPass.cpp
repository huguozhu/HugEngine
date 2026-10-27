#include "PostProcess/ToneMapPass.h"
#include <iterator>
#include "ToneMap.vert.spv.h"
#include "ToneMap.vert.dxil.h"
#include "ToneMap.frag.spv.h"
#include "ToneMap.frag.dxil.h"
#include "Core/Log.h"
#include "Core/Assert.h"

namespace he::render {

// ToneMap 描述符集绑定号（与 ToneMap.frag 一致）
static constexpr u32 kToneMapBindInput = 0;   // HDR 输入

bool ToneMapPass::Initialize(rhi::IRHIDevice* device,u32 width,u32 height){
    m_Device=device;
    m_Width=width;
    m_Height=height;
    HE_ASSERT(m_Device,"ToneMapPass: null device");

    m_VS.stage=rhi::ShaderStage::Vertex;
    m_VS.spirv=k_ToneMap_vert_spv;
    m_VS.dxil.assign(std::begin(k_ToneMap_vert_dxil), std::end(k_ToneMap_vert_dxil));
    m_VS.dxil.assign(std::begin(k_ToneMap_vert_dxil), std::end(k_ToneMap_vert_dxil));
    m_VS.dxil.assign(std::begin(k_ToneMap_vert_dxil), std::end(k_ToneMap_vert_dxil));
    m_VS.entryPoint="main";
    m_FS.stage=rhi::ShaderStage::Pixel;
    m_FS.spirv=k_ToneMap_frag_spv;
    m_FS.dxil.assign(std::begin(k_ToneMap_frag_dxil), std::end(k_ToneMap_frag_dxil));
    m_FS.dxil.assign(std::begin(k_ToneMap_frag_dxil), std::end(k_ToneMap_frag_dxil));
    m_FS.dxil.assign(std::begin(k_ToneMap_frag_dxil), std::end(k_ToneMap_frag_dxil));
    m_FS.entryPoint="main";

    rhi::DescriptorSetLayoutDesc layout;layout.bindings={
        {0,rhi::DescriptorType::CombinedImageSampler,1,16},
    };
    m_DescLayout=device->CreateDescriptorSetLayout(layout);
    // 输入集按飞行帧各一份：槽位轮换时"本帧只写本槽位的集"，不会改写帧在飞时正被读的那一份
    for(u32 i=0;i<rhi::kMaxFramesInFlight;++i)
        m_DescSet[i]=device->AllocateDescriptorSet(m_DescLayout);

    RecreatePSO();

    m_Ready=true;
    HE_CORE_INFO("ToneMapPass init");
    return true;
}

void ToneMapPass::RecreatePSO(){
    rhi::PipelineStateDesc d;
    d.vertexShader=&m_VS;
    d.pixelShader=&m_FS;
    d.topology=rhi::PrimitiveTopology::TriangleList;
    d.depthTest=false;
    d.depthWrite=false;
    d.depthFormat=rhi::Format::D32_FLOAT;
    d.colorAttachmentCount=1;
    d.colorFormats[0]=m_OutputFormat;
    rhi::PushConstantRange pcr;
    pcr.stageMask=rhi::kStageMaskVertex|rhi::kStageMaskFragment;
    pcr.size=16;
    // Vertex|Fragment;
    d.pushConstantRanges={pcr};
    d.descriptorSetLayouts={m_DescLayout};d.debugName="ToneMap";
    m_PSO = m_Device->CreatePipelineStateHandle(d);   // A-3
    HE_ASSERT(m_PSO.IsValid(), "ToneMapPass: PSO 句柄创建失败");
}

void ToneMapPass::SetOutputFormat(rhi::Format f){
    if(f==m_OutputFormat)return;
    m_OutputFormat=f;
    if(m_Ready)RecreatePSO();
}

void ToneMapPass::Shutdown(){
    if(m_Device&&m_DescLayout!=rhi::kInvalidLayout)m_Device->DestroyDescriptorSetLayout(m_DescLayout);
    m_Device->DestroyPipelineStateHandle(m_PSO);
    m_PSO = {};
    m_Device=nullptr;
    m_Ready=false;
}

void ToneMapPass::SetInput(rhi::IRHITexture* hdr,rhi::IRHISampler* sampler){
    // 槽位模式（HDR 输入逐帧轮换）：本帧只写本槽位的集；旧模式：输入不变就不重写
    const u32 slot = m_SlotAware ? m_Slot : 0u;
    if(!m_SlotAware && hdr==m_HDRTarget[0] && sampler==m_HDRSampler) return;
    m_HDRSampler=sampler;
    m_HDRTarget[slot]=hdr;
    if(hdr&&sampler)
        m_Device->UpdateDescriptorSet(m_DescSet[slot],kToneMapBindInput,rhi::DescriptorType::CombinedImageSampler,hdr,sampler);
}

void ToneMapPass::Render(rhi::IRHICommandList* cmd){
    if(!m_Ready||!m_Enabled)return;
    cmd->SetPipeline(ResP(m_PSO));
    cmd->SetFullscreenViewport(static_cast<u32>(m_Width), static_cast<u32>(m_Height));
    cmd->BindDescriptorSet(rhi::kDescSetPerFrame,m_DescSet[m_SlotAware?m_Slot:0u]);
    struct { float exposure; float hdrEnabled; float whitePointNits; float _pad; } pc;
    pc.exposure = m_Exposure;
    pc.hdrEnabled = m_HDR;
    pc.whitePointNits = m_WhitePointNits;
    cmd->SetPushConstants(0, sizeof(pc), &pc);
    cmd->Draw(3);
}

void ToneMapPass::OnResize(u32 width,u32 height){
    if(width==m_Width&&height==m_Height)return;
    m_Width=width;
    m_Height=height;
}

} // namespace he::render
