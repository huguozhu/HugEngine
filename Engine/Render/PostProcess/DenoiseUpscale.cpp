// PostProcess/DenoiseUpscale.cpp — 降噪结果的重建升采样（全屏引导滤波）
#include "PostProcess/DenoiseUpscale.h"
#include "Core/Log.h"
#include "SSAO.vert.spv.h"
#include "Denoise_Upscale.frag.spv.h"

namespace he::render {

// 绑定号（与 Denoise_Upscale.frag 的 vk::binding 一致）
static constexpr u32 kUpscaleBindColor  = 0;
static constexpr u32 kUpscaleBindDepth  = 1;
static constexpr u32 kUpscaleBindNormal = 2;

// push constant 布局（必须与着色器里的 cbuffer Params 逐字段一致）
struct UpscalePushConstants {
    float2 srcTexelSize;
    float2 dstTexelSize;
    float  depthSigma;
    float  normalSigma;
    float2 pad;
};

bool DenoiseUpscale::Initialize(rhi::IRHIDevice* device, u32 outW, u32 outH) {
    m_Device = device;
    rhi::DescriptorSetLayoutDesc l;
    l.bindings = {
        { kUpscaleBindColor,  rhi::DescriptorType::CombinedImageSampler, 1, 16 },
        { kUpscaleBindDepth,  rhi::DescriptorType::CombinedImageSampler, 1, 16 },
        { kUpscaleBindNormal, rhi::DescriptorType::CombinedImageSampler, 1, 16 },
    };
    m_Layout = device->CreateDescriptorSetLayout(l);
    m_Set    = device->AllocateDescriptorSet(m_Layout);

    rhi::ShaderBytecode vs, fs;
    vs.stage      = rhi::ShaderStage::Vertex;
    vs.spirv      = k_SSAO_vert_spv;
    vs.entryPoint = "vertexMain";
    fs.stage      = rhi::ShaderStage::Pixel;
    fs.spirv      = k_Denoise_Upscale_frag_spv;
    fs.entryPoint = "fragmentMain";
    rhi::PipelineStateDesc d;
    d.vertexShader       = &vs;
    d.pixelShader        = &fs;
    d.topology           = rhi::PrimitiveTopology::TriangleList;
    d.depthTest          = false;
    d.depthWrite         = false;
    d.depthFormat        = rhi::Format::Unknown;
    d.colorAttachmentCount = 1;
    d.colorFormats[0]    = rhi::Format::RGBA16_FLOAT;
    d.descriptorSetLayouts = { m_Layout };
    d.debugName          = "DenoiseUpscale";
    m_PSO = device->CreatePipelineState(d);

    rhi::SamplerDesc sd;
    sd.minFilter = sd.magFilter = rhi::FilterMode::Linear;
    sd.addressU  = sd.addressV  = rhi::AddressMode::ClampToEdge;
    m_Sampler = device->CreateSampler(sd);

    OnResize(outW, outH);
    m_Ready = true;
    HE_CORE_INFO("DenoiseUpscale initialized");
    return true;
}

void DenoiseUpscale::Shutdown() {
    m_PSO.reset();
    m_Output.reset();
    m_Sampler.reset();
    if (m_Device && m_Layout != rhi::kInvalidLayout) m_Device->DestroyDescriptorSetLayout(m_Layout);
    m_Layout = rhi::kInvalidLayout;
    m_Device = nullptr;
    m_Ready  = false;
}

void DenoiseUpscale::OnResize(u32 outW, u32 outH) {
    m_Width  = outW;
    m_Height = outH;
    rhi::TextureDesc td;
    td.format    = rhi::Format::RGBA16_FLOAT;
    td.width     = outW;
    td.height    = outH;
    td.mipLevels = 1;
    td.usage     = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::ShaderResource;
    m_Output = m_Device->CreateTexture(td);
}

void DenoiseUpscale::SetInputs(rhi::IRHITexture* color, rhi::IRHITexture* depth,
                               rhi::IRHITexture* normal) {
    m_Input  = color;
    m_Depth  = depth;
    m_Normal = normal;
    if (color)  m_Device->UpdateDescriptorSet(m_Set, kUpscaleBindColor,  rhi::DescriptorType::CombinedImageSampler, color,  m_Sampler.get());
    if (depth)  m_Device->UpdateDescriptorSet(m_Set, kUpscaleBindDepth,  rhi::DescriptorType::CombinedImageSampler, depth,  m_Sampler.get());
    if (normal) m_Device->UpdateDescriptorSet(m_Set, kUpscaleBindNormal, rhi::DescriptorType::CombinedImageSampler, normal, m_Sampler.get());
}

void DenoiseUpscale::Render(rhi::IRHICommandList* cmd) {
    if (!m_Ready || !m_Input || !m_Depth || !m_Normal) return;
    cmd->SetPipeline(m_PSO.get());
    cmd->BindDescriptorSet(rhi::kDescSetPerFrame, m_Set);
    cmd->SetViewport({ 0, (float)m_Height, (float)m_Width, -(float)m_Height, 0, 1 });
    cmd->SetScissor({ 0, 0, m_Width, m_Height });
    // 源尺寸从纹理自身读：半分辨率/全分辨率是运行时开关，缓存一份副本会与纹理漂移
    const u32 srcW = m_Input->GetWidth()  ? m_Input->GetWidth()  : 1u;
    const u32 srcH = m_Input->GetHeight() ? m_Input->GetHeight() : 1u;
    UpscalePushConstants pc{};
    pc.srcTexelSize = float2(1.0f / (float)srcW, 1.0f / (float)srcH);
    pc.dstTexelSize = float2(1.0f / (float)m_Width, 1.0f / (float)m_Height);
    pc.depthSigma   = m_DepthSigma;
    pc.normalSigma  = m_NormalSigma;
    cmd->SetPushConstants(0, sizeof(pc), &pc);
    cmd->Draw(3);
}

} // namespace he::render
