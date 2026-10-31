// AntiAliasing/AA_FXAA.cpp — FXAA 实现
#include "AA_FXAA.h"
#include <iterator>
#include "Core/Log.h"
#include "Core/Assert.h"

// 着色器
#include "FXAA.vert.spv.h"
#include "FXAA.vert.dxil.h"
#include "FXAA.frag.spv.h"
#include "FXAA.frag.dxil.h"

namespace he::render {

// FXAA 描述符集绑定号（输入 LDR 纹理）
static constexpr u32 kFXAABindInput = 0;

bool AA_FXAA::Initialize(rhi::IRHIDevice* device, u32 width, u32 height) {
    m_Device = device;
    m_Width  = width;
    m_Height = height;

    CreatePSO();

    // 占位纹理
    {
        u8 w4[4] = {255,255,255,255};
        rhi::TextureDesc td;
        td.format=rhi::Format::BGRA8_UNORM;
        td.width=1;
        td.height=1;
        td.mipLevels=1;
        td.usage=rhi::TextureUsage::ShaderResource;
        td.initialData=w4;
        m_Placeholder = device->CreateTextureHandle(td);
        rhi::SamplerDesc sd;
        sd.minFilter=sd.magFilter=rhi::FilterMode::Linear;
        sd.addressU=sd.addressV=rhi::AddressMode::ClampToEdge;
        m_PlaceholderSamp = device->CreateSamplerHandle(sd);
    }

    m_Ready = true;
    HE_CORE_INFO("AA_FXAA initialized");
    return true;
}

void AA_FXAA::Shutdown() {
    // 【空保护】m_Device 可能为空（Shutdown 被调用两次，或 Initialize 失败后进入清理路径），
    // 直接解引用会崩溃（实测：02.Cube 退出时连续崩在多个 Shutdown 里）。
    if (!m_Device) return;

    m_Device->DestroyPipelineStateHandle(m_PSO); m_PSO = {};
    if (m_Device && m_DescLayout != rhi::kInvalidLayout)
        m_Device->DestroyDescriptorSetLayout(m_DescLayout);
    m_Device->DestroyTextureHandle(m_Placeholder); m_Placeholder = {};
    m_Device->DestroySamplerHandle(m_PlaceholderSamp); m_PlaceholderSamp = {};
    m_Device = nullptr;
    m_Ready  = false;
}

void AA_FXAA::OnResize(u32 w, u32 h) { m_Width = w; m_Height = h; }

void AA_FXAA::SetInput(rhi::IRHITexture* color, rhi::IRHISampler* sampler) {
    m_Input = color;
    m_InputSampler = sampler;
    if (m_Input && m_InputSampler)
        m_Device->UpdateDescriptorSet(m_DescSet, kFXAABindInput,
            rhi::DescriptorType::CombinedImageSampler, m_Input, m_InputSampler);
}

void AA_FXAA::Render(rhi::IRHICommandList* cmd) {
    if (!m_Ready || !m_Enabled || !m_Input) return;

    cmd->SetPipeline(ResP(m_PSO));
    cmd->BindDescriptorSet(rhi::kDescSetPerFrame, m_DescSet);
    cmd->SetFullscreenViewport(static_cast<u32>(m_Width), static_cast<u32>(m_Height));

    struct { float2 invSize; float2 _pad; } pc;
    pc.invSize = float2(1.0f/m_Width, 1.0f/m_Height);
    cmd->SetPushConstants(0, sizeof(pc), &pc);

    cmd->Draw(3);  // 全屏三角形
}

void AA_FXAA::CreatePSO() {
    // 描述符集
    rhi::DescriptorSetLayoutDesc layout;
    layout.bindings = {{kFXAABindInput, rhi::DescriptorType::CombinedImageSampler, 1, rhi::kStageMaskFragment}};
    m_DescLayout = m_Device->CreateDescriptorSetLayout(layout);
    m_DescSet = m_Device->AllocateDescriptorSet(m_DescLayout);

    // 着色器
    rhi::ShaderBytecode vs, fs;
    vs.stage=rhi::ShaderStage::Vertex;
    vs.spirv=k_FXAA_vert_spv;
    vs.dxil.assign(std::begin(k_FXAA_vert_dxil), std::end(k_FXAA_vert_dxil));
    vs.dxil.assign(std::begin(k_FXAA_vert_dxil), std::end(k_FXAA_vert_dxil));
    vs.dxil.assign(std::begin(k_FXAA_vert_dxil), std::end(k_FXAA_vert_dxil));
    vs.entryPoint="vertexMain";
    fs.stage=rhi::ShaderStage::Pixel;
    fs.spirv=k_FXAA_frag_spv;
    fs.dxil.assign(std::begin(k_FXAA_frag_dxil), std::end(k_FXAA_frag_dxil));
    fs.dxil.assign(std::begin(k_FXAA_frag_dxil), std::end(k_FXAA_frag_dxil));
    fs.dxil.assign(std::begin(k_FXAA_frag_dxil), std::end(k_FXAA_frag_dxil));
    fs.entryPoint="fragmentMain";

    rhi::PushConstantRange pcr;
    pcr.stageMask = rhi::kStageMaskFragment;
    pcr.offset=0;
    pcr.size=16;

    rhi::PipelineStateDesc d;
    d.vertexShader=&vs;
    d.pixelShader=&fs;
    d.topology=rhi::PrimitiveTopology::TriangleList;
    d.depthTest=false;
    d.depthWrite=false;
    // 终稿 pass 画进交换链 RenderPass（BeginRenderPass(1, swapFmt)），而那个 RP 带 D32
    // 深度附件：管线 RP 必须同样声明深度，否则每帧报
    // VUID-vkCmdDraw-renderPass-02684（当前 RP 深度附件=1，管线 RP=VK_ATTACHMENT_UNUSED）。
    // 与 ToneMapPass 的处理一致（depthTest/Write 仍为 false，附件只是要求兼容）。
    d.depthFormat=rhi::Format::D32_FLOAT;
    d.colorAttachmentCount=1;
    d.colorFormats[0]=rhi::Format::BGRA8_UNORM;
    d.pushConstantRanges={pcr}; d.descriptorSetLayouts={m_DescLayout}; d.debugName="FXAA";
    m_PSO = m_Device->CreatePipelineStateHandle(d);
    HE_ASSERT(m_PSO.IsValid(), "AA_FXAA: PSO 句柄创建失败");
}

} // namespace he::render
