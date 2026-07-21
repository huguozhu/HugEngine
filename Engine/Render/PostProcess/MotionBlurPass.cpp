// PostProcess/MotionBlurPass.cpp — 运动模糊（velocity-based 方向采样）
#include "MotionBlurPass.h"
#include "Core/Log.h"
#include "Core/Assert.h"
#include "Math/Math.h"

#include "SSAO.vert.spv.h"
#include "MotionBlur.frag.spv.h"

namespace he::render {

bool MotionBlurPass::Initialize(rhi::IRHIDevice* device, u32 width, u32 height) {
    m_Device = device;
    m_Width  = width;
    m_Height = height;

    rhi::DescriptorSetLayoutDesc layout;
    layout.bindings = {
        {0, rhi::DescriptorType::CombinedImageSampler, 1, 16},  // HDR
        {1, rhi::DescriptorType::CombinedImageSampler, 1, 16},  // Velocity
    };
    m_Layout = device->CreateDescriptorSetLayout(layout);
    m_Set    = device->AllocateDescriptorSet(m_Layout);

    rhi::ShaderBytecode vs, fs;
    vs.stage = rhi::ShaderStage::Vertex;
    vs.spirv = k_SSAO_vert_spv; vs.entryPoint = "vertexMain";
    fs.stage = rhi::ShaderStage::Pixel;
    fs.spirv = k_MotionBlur_frag_spv; fs.entryPoint = "fragmentMain";

    rhi::PushConstantRange pc; pc.stageMask = 16; pc.size = 16;
    rhi::PipelineStateDesc d;
    d.vertexShader = &vs; d.pixelShader = &fs;
    d.topology = rhi::PrimitiveTopology::TriangleList;
    d.depthTest = false; d.depthWrite = false; d.depthFormat = rhi::Format::Unknown;
    d.colorAttachmentCount = 1; d.colorFormats[0] = rhi::Format::RGBA16_FLOAT;
    d.pushConstantRanges = {pc}; d.descriptorSetLayouts = {m_Layout};
    d.debugName = "MotionBlur";
    m_PSO = device->CreatePipelineState(d);
    HE_ASSERT(m_PSO, "MotionBlurPass: PSO failed");

    rhi::TextureDesc td;
    td.format = rhi::Format::RGBA16_FLOAT;
    td.width = width; td.height = height;
    td.usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::ShaderResource;
    m_Output = device->CreateTexture(td);

    rhi::SamplerDesc sd;
    sd.minFilter = sd.magFilter = rhi::FilterMode::Linear;
    sd.addressU = sd.addressV = rhi::AddressMode::ClampToEdge;
    m_OutSampler = device->CreateSampler(sd);

    m_Ready = true;
    HE_CORE_INFO("MotionBlurPass 初始化完成 ({}x{})", width, height);
    return true;
}

void MotionBlurPass::EnsureInitialized() {
    if (m_Ready || !m_Device) return;
    Initialize(m_Device, m_Width, m_Height);
}

void MotionBlurPass::Shutdown() {
    if (!m_Ready) return;
    m_PSO.reset(); m_Output.reset(); m_OutSampler.reset();
    if (m_Device && m_Layout != rhi::kInvalidLayout) m_Device->DestroyDescriptorSetLayout(m_Layout);
    m_Device = nullptr; m_Ready = false;
}

void MotionBlurPass::OnResize(u32 w, u32 h) {
    if (!m_Ready) return;
    if (w == m_Width && h == m_Height) return;
    m_Width = w; m_Height = h;
    rhi::TextureDesc td;
    td.format = rhi::Format::RGBA16_FLOAT;
    td.width = w; td.height = h;
    td.usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::ShaderResource;
    m_Output = m_Device->CreateTexture(td);
}

void MotionBlurPass::SetInputs(rhi::IRHITexture* hdr, rhi::IRHISampler* hdrSampler,
                               rhi::IRHITexture* velocity, rhi::IRHISampler* velSampler) {
    m_HDRInput   = hdr;
    m_HDRSampler = hdrSampler;
    m_VelInput   = velocity;
    m_VelSampler = velSampler;
}

void MotionBlurPass::Render(rhi::IRHICommandList* cmd) {
    if (!m_Ready || !m_Enabled || !m_HDRInput || !m_VelInput) return;

    m_Device->UpdateDescriptorSet(m_Set, 0, rhi::DescriptorType::CombinedImageSampler,
        m_HDRInput, m_HDRSampler);
    m_Device->UpdateDescriptorSet(m_Set, 1, rhi::DescriptorType::CombinedImageSampler,
        m_VelInput, m_VelSampler);

    struct { float intensity; u32 samples; float2 _pad; } pc;
    pc.intensity  = m_Intensity;
    pc.samples    = 12;

    cmd->SetPipeline(m_PSO.get());
    cmd->BindDescriptorSet(rhi::kDescSetPerFrame, m_Set);
    cmd->SetViewport({0, (float)m_Height, (float)m_Width, -(float)m_Height, 0, 1});
    cmd->SetScissor({0, 0, m_Width, m_Height});
    cmd->SetPushConstants(0, sizeof(pc), &pc);

    rhi::ClearValue clr{};
    cmd->BeginOffscreenPass(m_Output->GetNativeHandle(), nullptr, m_Width, m_Height, &clr, false);
    cmd->Draw(3);
    cmd->EndOffscreenPass();
}

} // namespace he::render
