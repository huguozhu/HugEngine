// PostProcess/CameraEffectsPass.cpp — LDR 镜头后处理实现
#include "CameraEffectsPass.h"
#include <iterator>
#include "Core/Log.h"
#include "Core/Assert.h"
#include "SSAO.vert.spv.h"
#include "SSAO.vert.dxil.h"
#include "CameraEffects.frag.spv.h"
#include "CameraEffects.frag.dxil.h"

namespace he::render {

// CameraEffects 描述符集绑定号（与 CameraEffects.frag 一致）
static constexpr u32 kCameraEffectsBindInput = 0;   // 输入颜色

bool CameraEffectsPass::Initialize(rhi::IRHIDevice* device, u32 width, u32 height) {
    m_Device = device;
    m_Width = width;
    m_Height = height;

    rhi::DescriptorSetLayoutDesc layout;
    layout.bindings = {{kCameraEffectsBindInput, rhi::DescriptorType::CombinedImageSampler, 1, rhi::kStageMaskFragment}};
    m_Layout = device->CreateDescriptorSetLayout(layout);
    m_Set    = device->AllocateDescriptorSet(m_Layout);

    rhi::ShaderBytecode vs, fs;
    vs.stage = rhi::ShaderStage::Vertex;
    vs.spirv = k_SSAO_vert_spv;
    vs.dxil.assign(std::begin(k_SSAO_vert_dxil), std::end(k_SSAO_vert_dxil));
    vs.dxil.assign(std::begin(k_SSAO_vert_dxil), std::end(k_SSAO_vert_dxil));
    vs.dxil.assign(std::begin(k_SSAO_vert_dxil), std::end(k_SSAO_vert_dxil));
    vs.entryPoint = "main";
    fs.stage = rhi::ShaderStage::Pixel;
    fs.spirv = k_CameraEffects_frag_spv;
    fs.dxil.assign(std::begin(k_CameraEffects_frag_dxil), std::end(k_CameraEffects_frag_dxil));
    fs.dxil.assign(std::begin(k_CameraEffects_frag_dxil), std::end(k_CameraEffects_frag_dxil));
    fs.dxil.assign(std::begin(k_CameraEffects_frag_dxil), std::end(k_CameraEffects_frag_dxil));
    fs.entryPoint = "main";

    rhi::PushConstantRange pc;
    pc.stageMask = rhi::kStageMaskVertex | rhi::kStageMaskFragment;
    pc.size = 32;
    // 8 floats;
    rhi::PipelineStateDesc d;
    d.vertexShader = &vs;
    d.pixelShader = &fs;
    d.topology = rhi::PrimitiveTopology::TriangleList;
    d.depthTest = false;
    d.depthWrite = false;
    d.depthFormat = rhi::Format::Unknown;
    d.colorAttachmentCount = 1;
    d.colorFormats[0] = rhi::Format::BGRA8_UNORM;
    d.pushConstantRanges = {pc}; d.descriptorSetLayouts = {m_Layout}; d.debugName = "CameraEffects";
    m_PSO = device->CreatePipelineStateHandle(d);
    HE_ASSERT(m_PSO.IsValid(), "CameraEffectsPass: PSO failed");

    rhi::TextureDesc td;
    td.format = rhi::Format::BGRA8_UNORM;
    td.width = width;
    td.height = height;
    td.usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::ShaderResource;
    m_Output = device->CreateTexture(td);
    rhi::SamplerDesc sd;
    sd.minFilter = sd.magFilter = rhi::FilterMode::Linear;
    sd.addressU = sd.addressV = rhi::AddressMode::ClampToEdge;
    m_OutSampler = device->CreateSampler(sd);

    m_Ready = true;
    HE_CORE_INFO("CameraEffectsPass 初始化完成");
    return true;
}

void CameraEffectsPass::Shutdown() {
    // 【空保护】m_Device 可能为空（Shutdown 被调用两次，或 Initialize 失败后进入清理路径），
    // 直接解引用会崩溃（实测：02.Cube 退出时连续崩在多个 Shutdown 里）。
    if (!m_Device) return;

    m_Device->DestroyPipelineStateHandle(m_PSO); m_PSO = {};
    m_Output.reset();
    m_OutSampler.reset();
    if (m_Device && m_Layout != rhi::kInvalidLayout) m_Device->DestroyDescriptorSetLayout(m_Layout);
    m_Device = nullptr;
    m_Ready = false;
}

void CameraEffectsPass::OnResize(u32 w, u32 h) {
    if (!m_Ready) return;  // 懒初始化未触发，无需重建
    if (w == m_Width && h == m_Height) return;
    m_Width = w;
    m_Height = h;
    rhi::TextureDesc td;
    td.format = rhi::Format::BGRA8_UNORM;
    td.width = w;
    td.height = h;
    td.usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::ShaderResource;
    m_Output = m_Device->CreateTexture(td);
}

void CameraEffectsPass::SetInput(rhi::IRHITexture* color, rhi::IRHISampler* sampler) {
    m_Input = color;
    m_InputSampler = sampler;
    if (m_Input && m_InputSampler)
        m_Device->UpdateDescriptorSet(m_Set, kCameraEffectsBindInput, rhi::DescriptorType::CombinedImageSampler, m_Input, m_InputSampler);
}

void CameraEffectsPass::Render(rhi::IRHICommandList* cmd) {
    if (!m_Ready || !m_Enabled || !m_Input) return;

    m_Time += 0.016f;  // 帧间隔（颗粒动画），与 AutoExposure kDefaultDeltaTime 一致

    struct { float filmGrain, vignette, ca, distortion, time, _pad[3]; } pc;
    pc.filmGrain = m_FilmGrain;
    pc.vignette = m_Vignette;
    pc.ca = m_CA;
    pc.distortion = m_Distortion;
    pc.time = m_Time;

    cmd->SetPipeline(ResP(m_PSO));
    cmd->BindDescriptorSet(rhi::kDescSetPerFrame, m_Set);
    cmd->SetFullscreenViewport(static_cast<u32>(m_Width), static_cast<u32>(m_Height));
    cmd->SetPushConstants(0, sizeof(pc), &pc);
    cmd->Draw(3);
}

} // namespace he::render
