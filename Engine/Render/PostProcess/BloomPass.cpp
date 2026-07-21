// PostProcess/BloomPass.cpp — 完整 Bloom 后处理实现
// 管线：HDR → BrightPass(阈值+半分辨率) → GaussianBlur → Composite(上采样+叠加)
#include "BloomPass.h"
#include "Core/Log.h"
#include "Core/Assert.h"
#include "Math/Math.h"

// 着色器字节码
#include "SSAO.vert.spv.h"          // 复用通用全屏三角形顶点着色器
#include "BrightPass.frag.spv.h"
#include "BloomComposite.frag.spv.h"

namespace he::render {

bool BloomPass::Initialize(rhi::IRHIDevice* device, u32 width, u32 height) {
    m_Device = device;
    m_Width  = width;
    m_Height = height;
    u32 hw = width / 2, hh = height / 2;

    // ── BrightPass PSO ──
    {
        rhi::DescriptorSetLayoutDesc layout;
        layout.bindings = {{0, rhi::DescriptorType::CombinedImageSampler, 1, rhi::kStageMaskFragment}};
        m_BrightLayout = device->CreateDescriptorSetLayout(layout);
        m_BrightSet    = device->AllocateDescriptorSet(m_BrightLayout);

        rhi::ShaderBytecode vs, fs;
        vs.stage = rhi::ShaderStage::Vertex;
        vs.spirv = k_SSAO_vert_spv;
        vs.entryPoint = "vertexMain";
        fs.stage = rhi::ShaderStage::Pixel;
        fs.spirv = k_BrightPass_frag_spv;
        fs.entryPoint = "fragmentMain";

        rhi::PushConstantRange pcRange;
        pcRange.stageMask = rhi::kStageMaskFragment;  // Fragment
        pcRange.size      = 16;  // float threshold + 3 float pad

        rhi::PipelineStateDesc d;
        d.vertexShader         = &vs;
        d.pixelShader          = &fs;
        d.topology             = rhi::PrimitiveTopology::TriangleList;
        d.depthTest            = false;
        d.depthWrite           = false;
        d.depthFormat          = rhi::Format::Unknown;
        d.colorAttachmentCount = 1;
        d.colorFormats[0]      = rhi::Format::RGBA16_FLOAT;
        d.pushConstantRanges   = {pcRange};
        d.descriptorSetLayouts = {m_BrightLayout};
        d.debugName            = "BrightPass";
        m_BrightPSO = device->CreatePipelineState(d);
        HE_ASSERT(m_BrightPSO, "BloomPass: BrightPass PSO 创建失败");

        // 半分辨率 bright 纹理
        rhi::TextureDesc td;
        td.format = rhi::Format::RGBA16_FLOAT;
        td.width  = hw; td.height = hh;
        td.usage  = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::ShaderResource;
        m_BrightTex = device->CreateTexture(td);

        rhi::SamplerDesc sd;
        sd.minFilter = sd.magFilter = rhi::FilterMode::Linear;
        sd.addressU = sd.addressV = rhi::AddressMode::ClampToEdge;
        m_BrightSampler = device->CreateSampler(sd);
    }

    // ── 高斯模糊（半分辨率）──
    m_Blur.Initialize(device, hw, hh);

    // ── Composite PSO ──
    {
        rhi::DescriptorSetLayoutDesc layout;
        layout.bindings = {
            {0, rhi::DescriptorType::CombinedImageSampler, 1, rhi::kStageMaskFragment},  // HDR
            {1, rhi::DescriptorType::CombinedImageSampler, 1, rhi::kStageMaskFragment},  // Bloom
        };
        m_CompositeLayout = device->CreateDescriptorSetLayout(layout);
        m_CompositeSet    = device->AllocateDescriptorSet(m_CompositeLayout);

        rhi::ShaderBytecode vs, fs;
        vs.stage = rhi::ShaderStage::Vertex;
        vs.spirv = k_SSAO_vert_spv;
        vs.entryPoint = "vertexMain";
        fs.stage = rhi::ShaderStage::Pixel;
        fs.spirv = k_BloomComposite_frag_spv;
        fs.entryPoint = "fragmentMain";

        rhi::PushConstantRange pcRange;
        pcRange.stageMask = rhi::kStageMaskFragment;
        pcRange.size      = 16;  // float intensity + 3 float pad

        rhi::PipelineStateDesc d;
        d.vertexShader         = &vs;
        d.pixelShader          = &fs;
        d.topology             = rhi::PrimitiveTopology::TriangleList;
        d.depthTest            = false;
        d.depthWrite           = false;
        d.depthFormat          = rhi::Format::Unknown;
        d.colorAttachmentCount = 1;
        d.colorFormats[0]      = rhi::Format::RGBA16_FLOAT;
        d.pushConstantRanges   = {pcRange};
        d.descriptorSetLayouts = {m_CompositeLayout};
        d.debugName            = "BloomComposite";
        m_CompositePSO = device->CreatePipelineState(d);
        HE_ASSERT(m_CompositePSO, "BloomPass: Composite PSO 创建失败");

        // 全分辨率输出纹理
        rhi::TextureDesc td;
        td.format = rhi::Format::RGBA16_FLOAT;
        td.width  = width; td.height = height;
        td.usage  = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::ShaderResource;
        m_Output = device->CreateTexture(td);

        rhi::SamplerDesc sd;
        sd.minFilter = sd.magFilter = rhi::FilterMode::Linear;
        sd.addressU = sd.addressV = rhi::AddressMode::ClampToEdge;
        m_OutSampler = device->CreateSampler(sd);
    }

    m_Ready = true;
    HE_CORE_INFO("BloomPass 初始化完成 ({}x{} + {}x{}), threshold={}, intensity={}",
                 m_Width, m_Height, hw, hh, m_Threshold, m_Intensity);
    return true;
}

void BloomPass::Shutdown() {
    if (!m_Ready) return;  // 懒初始化从未触发，无需清理

    m_BrightPSO.reset();
    m_BrightTex.reset();
    m_BrightSampler.reset();
    if (m_Device && m_BrightLayout != rhi::kInvalidLayout) {
        m_Device->DestroyDescriptorSetLayout(m_BrightLayout);
    }

    m_Blur.Shutdown();

    m_CompositePSO.reset();
    m_Output.reset();
    m_OutSampler.reset();
    if (m_Device && m_CompositeLayout != rhi::kInvalidLayout) {
        m_Device->DestroyDescriptorSetLayout(m_CompositeLayout);
    }

    m_Device = nullptr;
    m_Ready  = false;
}

void BloomPass::OnResize(u32 w, u32 h) {
    if (!m_Ready) return;  // 懒初始化从未触发，无需 resize
    if (w == m_Width && h == m_Height) return;
    m_Width  = w;
    m_Height = h;
    u32 hw = w / 2, hh = h / 2;

    // 重建 bright 纹理
    rhi::TextureDesc td;
    td.format = rhi::Format::RGBA16_FLOAT;
    td.width  = hw; td.height = hh;
    td.usage  = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::ShaderResource;
    m_BrightTex = m_Device->CreateTexture(td);

    // 重建高斯模糊（半分辨率）
    m_Blur.OnResize(hw, hh);

    // 重建输出纹理
    td.width  = w; td.height = h;
    m_Output = m_Device->CreateTexture(td);
}

void BloomPass::SetInput(rhi::IRHITexture* hdr, rhi::IRHISampler* sampler) {
    m_HDRInput   = hdr;
    m_HDRSampler = sampler;
}

void BloomPass::EnsureInitialized() {
    if (m_Ready || !m_Device) return;
    Initialize(m_Device, m_Width, m_Height);
}

void BloomPass::Render(rhi::IRHICommandList* cmd) {
    if (!m_Ready || !m_Enabled) return;  // 未初始化或禁用 → 跳过
    if (!m_HDRInput) return;
    u32 hw = m_Width / 2, hh = m_Height / 2;
    rhi::ClearValue clr{};

    // ── Pass 1: BrightPass（阈值提取 → 半分辨率）──
    m_Device->UpdateDescriptorSet(m_BrightSet, 0,
        rhi::DescriptorType::CombinedImageSampler, m_HDRInput, m_HDRSampler);

    cmd->SetPipeline(m_BrightPSO.get());
    cmd->BindDescriptorSet(rhi::kDescSetPerFrame, m_BrightSet);
    cmd->SetViewport({0, (float)hh, (float)hw, -(float)hh, 0, 1});
    cmd->SetScissor({0, 0, hw, hh});

    struct { float threshold; float _pad[3]; } bpc;
    bpc.threshold = m_Threshold;
    cmd->SetPushConstants(0, sizeof(bpc), &bpc);

    cmd->BeginOffscreenPass(m_BrightTex->GetNativeHandle(), nullptr, hw, hh, &clr, false);
    cmd->Draw(3);
    cmd->EndOffscreenPass();

    // ── Pass 2: 高斯模糊（半分辨率）──
    m_Blur.SetInput(m_BrightTex.get(), m_BrightSampler.get());
    m_Blur.PreBind(cmd);
    cmd->BeginOffscreenPass(m_Blur.GetOutput()->GetNativeHandle(), nullptr, hw, hh, &clr, false);
    m_Blur.Render(cmd);
    cmd->EndOffscreenPass();

    // ── Pass 3: Composite（上采样 Bloom + 叠加到原始 HDR）──
    m_Device->UpdateDescriptorSet(m_CompositeSet, 0,
        rhi::DescriptorType::CombinedImageSampler, m_HDRInput, m_HDRSampler);
    m_Device->UpdateDescriptorSet(m_CompositeSet, 1,
        rhi::DescriptorType::CombinedImageSampler,
        m_Blur.GetOutput(), m_Blur.GetOutputSampler());

    cmd->SetPipeline(m_CompositePSO.get());
    cmd->BindDescriptorSet(rhi::kDescSetPerFrame, m_CompositeSet);
    cmd->SetViewport({0, (float)m_Height, (float)m_Width, -(float)m_Height, 0, 1});
    cmd->SetScissor({0, 0, m_Width, m_Height});

    struct { float intensity; float _pad[3]; } cpc;
    cpc.intensity = m_Intensity;
    cmd->SetPushConstants(0, sizeof(cpc), &cpc);

    cmd->BeginOffscreenPass(m_Output->GetNativeHandle(), nullptr, m_Width, m_Height, &clr, false);
    cmd->Draw(3);
    cmd->EndOffscreenPass();
}

} // namespace he::render
