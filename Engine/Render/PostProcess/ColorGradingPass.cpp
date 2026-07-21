// PostProcess/ColorGradingPass.cpp — LDR 色彩分级实现
#include "ColorGradingPass.h"
#include "Core/Log.h"
#include "Core/Assert.h"
#include "SSAO.vert.spv.h"
#include "ColorGrading.frag.spv.h"

namespace he::render {

bool ColorGradingPass::Initialize(rhi::IRHIDevice* device, u32 width, u32 height) {
    m_Device = device; m_Width = width; m_Height = height;

    rhi::DescriptorSetLayoutDesc layout;
    layout.bindings = {{0, rhi::DescriptorType::CombinedImageSampler, 1, rhi::kStageMaskFragment}};
    m_Layout = device->CreateDescriptorSetLayout(layout);
    m_Set    = device->AllocateDescriptorSet(m_Layout);

    rhi::ShaderBytecode vs, fs;
    vs.stage = rhi::ShaderStage::Vertex; vs.spirv = k_SSAO_vert_spv; vs.entryPoint = "vertexMain";
    fs.stage = rhi::ShaderStage::Pixel;  fs.spirv = k_ColorGrading_frag_spv; fs.entryPoint = "fragmentMain";

    rhi::PushConstantRange pc; pc.stageMask = rhi::kStageMaskVertex | rhi::kStageMaskFragment; pc.size = 64;  // 13 floats + pad
    rhi::PipelineStateDesc d;
    d.vertexShader = &vs; d.pixelShader = &fs;
    d.topology = rhi::PrimitiveTopology::TriangleList;
    d.depthTest = false; d.depthWrite = false; d.depthFormat = rhi::Format::Unknown;
    d.colorAttachmentCount = 1; d.colorFormats[0] = rhi::Format::BGRA8_UNORM;
    d.pushConstantRanges = {pc}; d.descriptorSetLayouts = {m_Layout}; d.debugName = "ColorGrading";
    m_PSO = device->CreatePipelineState(d);
    HE_ASSERT(m_PSO, "ColorGradingPass: PSO failed");

    rhi::TextureDesc td; td.format = rhi::Format::BGRA8_UNORM;
    td.width = width; td.height = height;
    td.usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::ShaderResource;
    m_Output = device->CreateTexture(td);
    rhi::SamplerDesc sd; sd.minFilter = sd.magFilter = rhi::FilterMode::Linear;
    sd.addressU = sd.addressV = rhi::AddressMode::ClampToEdge;
    m_OutSampler = device->CreateSampler(sd);

    m_Ready = true;
    HE_CORE_INFO("ColorGradingPass 初始化完成");
    return true;
}

void ColorGradingPass::Shutdown() {
    m_PSO.reset(); m_Output.reset(); m_OutSampler.reset();
    if (m_Device && m_Layout != rhi::kInvalidLayout) m_Device->DestroyDescriptorSetLayout(m_Layout);
    m_Device = nullptr; m_Ready = false;
}

void ColorGradingPass::OnResize(u32 w, u32 h) {
    m_Width = w; m_Height = h;
    rhi::TextureDesc td; td.format = rhi::Format::BGRA8_UNORM;
    td.width = w; td.height = h; td.usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::ShaderResource;
    m_Output = m_Device->CreateTexture(td);
}

void ColorGradingPass::SetInput(rhi::IRHITexture* color, rhi::IRHISampler* sampler) {
    m_Input = color; m_InputSampler = sampler;
    if (m_Input && m_InputSampler)
        m_Device->UpdateDescriptorSet(m_Set, 0, rhi::DescriptorType::CombinedImageSampler, m_Input, m_InputSampler);
}

void ColorGradingPass::Render(rhi::IRHICommandList* cmd) {
    if (!m_Ready || !m_Enabled || !m_Input) return;

    struct {
        float sat, contrast, vib;
        float shR, shG, shB;
        float mtR, mtG, mtB;
        float hlR, hlG, hlB;
        float _pad;
    } pc;
    pc.sat = m_Saturation; pc.contrast = m_Contrast; pc.vib = m_Vibrance;
    pc.shR = 1.0f; pc.shG = 1.0f; pc.shB = 1.1f;   // 暗部偏蓝
    pc.mtR = 1.05f; pc.mtG = 1.0f; pc.mtB = 0.95f;  // 中间偏暖
    pc.hlR = 1.1f; pc.hlG = 1.05f; pc.hlB = 0.9f;   // 亮部偏暖
    pc._pad = 0;

    cmd->SetPipeline(m_PSO.get()); cmd->BindDescriptorSet(rhi::kDescSetPerFrame, m_Set);
    cmd->SetViewport({0, (float)m_Height, (float)m_Width, -(float)m_Height, 0, 1});
    cmd->SetScissor({0, 0, m_Width, m_Height});
    cmd->SetPushConstants(0, sizeof(pc), &pc);
    cmd->Draw(3);
}

} // namespace he::render
