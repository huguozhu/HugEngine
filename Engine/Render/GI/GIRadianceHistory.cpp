// GI/GIRadianceHistory.cpp — 前帧 HDR 辐射度（GI 源共享）
//
// 实现本身是把 GI_DDGI 里原有的 CaptureHDR 抽出来；除了所有权搬到这里，渲染流程
// （FullscreenCopy + 线性采样自动降采样）与判定条件保持不变，以免影响 DDGI 探针行为。
#include "GI/GIRadianceHistory.h"
#include "Core/Log.h"
#include "Fullscreen.vert.spv.h"
#include "FullscreenCopy.frag.spv.h"

namespace he::render {

// 下采样描述符集绑定号（与本组件自己的 set=0 一致）
static constexpr u32 kRadianceBindInput = 0;

void GIRadianceHistory::CreateTexture(u32 width, u32 height, bool bumpGeneration) {
    rhi::TextureDesc td;
    td.width  = (width  / kDownsampleFactor > 0) ? width  / kDownsampleFactor : 1;
    td.height = (height / kDownsampleFactor > 0) ? height / kDownsampleFactor : 1;
    td.format = rhi::Format::RGBA16_FLOAT;
    td.usage  = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::ShaderResource;
    m_Texture = m_Device->CreateTexture(td);
    if (bumpGeneration) ++m_Generation;
}

bool GIRadianceHistory::Initialize(rhi::IRHIDevice* device, u32 width, u32 height) {
    if (!device) return false;
    m_Device = device;

    // 线性采样：既是下采样源的采样方式（自动降采样），也是消费方采样本纹理的方式
    rhi::SamplerDesc sd;
    sd.minFilter = sd.magFilter = rhi::FilterMode::Linear;
    sd.addressU  = sd.addressV  = rhi::AddressMode::ClampToEdge;
    m_Sampler = device->CreateSampler(sd);

    // 下采样输入：全分辨率 HDR
    rhi::DescriptorSetLayoutDesc dl;
    dl.bindings = {{kRadianceBindInput, rhi::DescriptorType::CombinedImageSampler, 1, 16}};
    m_Layout = device->CreateDescriptorSetLayout(dl);
    m_Set    = device->AllocateDescriptorSet(m_Layout);

    // 下采样 PSO（FullscreenCopy：线性采样源 HDR 自动降采样）
    rhi::ShaderBytecode vs, fs;
    vs.stage      = rhi::ShaderStage::Vertex;
    vs.spirv      = k_Fullscreen_vert_spv;
    vs.entryPoint = "vertexMain";
    fs.stage      = rhi::ShaderStage::Pixel;
    fs.spirv      = k_FullscreenCopy_frag_spv;
    fs.entryPoint = "fragmentMain";

    rhi::PipelineStateDesc pd;
    pd.vertexShader         = &vs;
    pd.pixelShader          = &fs;
    pd.topology             = rhi::PrimitiveTopology::TriangleList;
    pd.depthTest            = false;
    pd.depthWrite           = false;
    pd.depthFormat          = rhi::Format::Unknown;
    pd.colorAttachmentCount = 1;
    pd.colorFormats[0]      = rhi::Format::RGBA16_FLOAT;
    pd.descriptorSetLayouts = {m_Layout};
    pd.debugName            = "GIRadianceHistory_Downsample";
    m_PSO = device->CreatePipelineState(pd);

    // 立即建纹理（代次置 1），使消费方在 Initialize 阶段就能绑定到有效纹理；
    // 之后 Capture 只在源尺寸变化时重建。
    CreateTexture(width, height, true);
    m_SourceWidth  = width;
    m_SourceHeight = height;

    if (!m_Texture || !m_PSO) {
        HE_CORE_ERROR("GIRadianceHistory: 初始化失败（纹理或 PSO 为空）");
        return false;
    }
    HE_CORE_INFO("GIRadianceHistory initialized (source {}x{} -> {}x{})",
                 width, height, m_Texture->GetWidth(), m_Texture->GetHeight());
    return true;
}

void GIRadianceHistory::Shutdown() {
    m_Set = rhi::kInvalidSet;
    m_Layout = rhi::kInvalidLayout;
    m_PSO.reset();
    m_Texture.reset();
    m_Sampler.reset();
    m_Device = nullptr;
    m_Generation = 0;
    m_SourceWidth = m_SourceHeight = 0;
}

void GIRadianceHistory::Capture(rhi::IRHICommandList* cmd, rhi::IRHITexture* hdr) {
    if (!cmd || !hdr || !m_Device || !m_Texture || !m_PSO) return;

    // 源尺寸变化（窗口 resize）→ 重建纹理并递增代次，消费方据此重绑描述符
    if (hdr->GetWidth() != m_SourceWidth || hdr->GetHeight() != m_SourceHeight) {
        CreateTexture(hdr->GetWidth(), hdr->GetHeight(), true);
        m_SourceWidth  = hdr->GetWidth();
        m_SourceHeight = hdr->GetHeight();
        if (!m_Texture) return;
    }

    const u32 tw = m_Texture->GetWidth();
    const u32 th = m_Texture->GetHeight();

    m_Device->UpdateDescriptorSet(m_Set, kRadianceBindInput,
        rhi::DescriptorType::CombinedImageSampler, hdr, m_Sampler.get());

    cmd->SetPipeline(m_PSO.get());
    cmd->BindDescriptorSet(rhi::kDescSetPerFrame, m_Set);
    rhi::ClearValue clr{};
    cmd->BeginOffscreenPass(m_Texture->GetNativeHandle(), nullptr, tw, th, &clr, false);
    cmd->SetViewport({0, (float)th, (float)tw, -(float)th, 0, 1});
    cmd->SetScissor({0, 0, tw, th});
    cmd->Draw(3);
    cmd->EndOffscreenPass();
}

} // namespace he::render
