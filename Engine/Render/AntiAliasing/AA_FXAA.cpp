// AntiAliasing/AA_FXAA.cpp — FXAA 实现
#include "AA_FXAA.h"
#include "Core/Log.h"
#include "Core/Assert.h"

// 着色器
#include "FXAA.vert.spv.h"
#include "FXAA.frag.spv.h"

namespace he::render {

bool AA_FXAA::Initialize(rhi::IRHIDevice* device, u32 width, u32 height) {
    m_Device = device;
    m_Width  = width;
    m_Height = height;

    CreatePSO();

    // 占位纹理
    {
        u8 w4[4] = {255,255,255,255};
        rhi::TextureDesc td; td.format=rhi::Format::BGRA8_UNORM;
        td.width=1; td.height=1; td.mipLevels=1;
        td.usage=rhi::TextureUsage::ShaderResource; td.initialData=w4;
        m_Placeholder = device->CreateTexture(td);
        rhi::SamplerDesc sd; sd.minFilter=sd.magFilter=rhi::FilterMode::Linear;
        sd.addressU=sd.addressV=rhi::AddressMode::ClampToEdge;
        m_PlaceholderSamp = device->CreateSampler(sd);
    }

    m_Ready = true;
    HE_CORE_INFO("AA_FXAA initialized");
    return true;
}

void AA_FXAA::Shutdown() {
    m_PSO.reset();
    if (m_Device && m_DescLayout != rhi::kInvalidLayout)
        m_Device->DestroyDescriptorSetLayout(m_DescLayout);
    m_Placeholder.reset();
    m_PlaceholderSamp.reset();
    m_Device = nullptr;
    m_Ready  = false;
}

void AA_FXAA::OnResize(u32 w, u32 h) { m_Width = w; m_Height = h; }

void AA_FXAA::SetInput(rhi::IRHITexture* color, rhi::IRHISampler* sampler) {
    m_Input = color;
    m_InputSampler = sampler;
    if (m_Input && m_InputSampler)
        m_Device->UpdateDescriptorSet(m_DescSet, 0,
            rhi::DescriptorType::CombinedImageSampler, m_Input, m_InputSampler);
}

void AA_FXAA::Render(rhi::IRHICommandList* cmd) {
    if (!m_Ready || !m_Enabled || !m_Input) return;

    cmd->SetPipeline(m_PSO.get());
    cmd->BindDescriptorSet(rhi::kDescSetPerFrame, m_DescSet);
    cmd->SetViewport({0,(float)m_Height,(float)m_Width,-(float)m_Height,0,1});
    cmd->SetScissor({0,0,m_Width,m_Height});

    struct { float2 invSize; float2 _pad; } pc;
    pc.invSize = float2(1.0f/m_Width, 1.0f/m_Height);
    cmd->SetPushConstants(0, sizeof(pc), &pc);

    cmd->Draw(3);  // 全屏三角形
}

void AA_FXAA::CreatePSO() {
    // 描述符集
    rhi::DescriptorSetLayoutDesc layout;
    layout.bindings = {{0, rhi::DescriptorType::CombinedImageSampler, 1, rhi::kStageMaskFragment}};
    m_DescLayout = m_Device->CreateDescriptorSetLayout(layout);
    m_DescSet = m_Device->AllocateDescriptorSet(m_DescLayout);

    // 着色器
    rhi::ShaderBytecode vs, fs;
    vs.stage=rhi::ShaderStage::Vertex;   vs.spirv=k_FXAA_vert_spv; vs.entryPoint="vertexMain";
    fs.stage=rhi::ShaderStage::Pixel;    fs.spirv=k_FXAA_frag_spv; fs.entryPoint="fragmentMain";

    rhi::PushConstantRange pcr; pcr.stageMask = rhi::kStageMaskFragment; pcr.offset=0; pcr.size=16;

    rhi::PipelineStateDesc d;
    d.vertexShader=&vs; d.pixelShader=&fs;
    d.topology=rhi::PrimitiveTopology::TriangleList;
    d.depthTest=false; d.depthWrite=false;
    d.depthFormat=rhi::Format::Unknown;
    d.colorAttachmentCount=1; d.colorFormats[0]=rhi::Format::BGRA8_UNORM;
    d.pushConstantRanges={pcr}; d.descriptorSetLayouts={m_DescLayout}; d.debugName="FXAA";
    m_PSO = m_Device->CreatePipelineState(d);
    HE_ASSERT(m_PSO, "AA_FXAA: PSO creation failed");
}

} // namespace he::render
