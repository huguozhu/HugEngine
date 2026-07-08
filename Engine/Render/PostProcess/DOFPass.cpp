// PostProcess/DOFPass.cpp — 景深后处理（CoC + Blur + Composite）
#include "DOFPass.h"
#include "Core/Log.h"
#include "Core/Assert.h"
#include "Math/Math.h"

#include "SSAO.vert.spv.h"
#include "DOF_CoC.frag.spv.h"
#include "DOF_Composite.frag.spv.h"

namespace he::render {

bool DOFPass::Initialize(rhi::IRHIDevice* device, u32 width, u32 height) {
    m_Device = device;
    m_Width  = width;
    m_Height = height;

    // ── CoC PSO ──
    {
        rhi::DescriptorSetLayoutDesc layout;
        layout.bindings = {{0, rhi::DescriptorType::CombinedImageSampler, 1, 16}};
        m_CoCLayout = device->CreateDescriptorSetLayout(layout);
        m_CoCSet    = device->AllocateDescriptorSet(m_CoCLayout);

        rhi::ShaderBytecode vs, fs;
        vs.stage = rhi::ShaderStage::Vertex;
        vs.spirv = k_SSAO_vert_spv; vs.entryPoint = "vertexMain";
        fs.stage = rhi::ShaderStage::Pixel;
        fs.spirv = k_DOF_CoC_frag_spv; fs.entryPoint = "fragmentMain";

        rhi::PushConstantRange pc; pc.stageMask = 16; pc.size = 16;
        rhi::PipelineStateDesc d;
        d.vertexShader = &vs; d.pixelShader = &fs;
        d.topology = rhi::PrimitiveTopology::TriangleList;
        d.depthTest = false; d.depthWrite = false; d.depthFormat = rhi::Format::Unknown;
        d.colorAttachmentCount = 1; d.colorFormats[0] = rhi::Format::R16_FLOAT;
        d.pushConstantRanges = {pc}; d.descriptorSetLayouts = {m_CoCLayout};
        d.debugName = "DOF_CoC";
        m_CoCPSO = device->CreatePipelineState(d);
        HE_ASSERT(m_CoCPSO, "DOFPass: CoC PSO failed");

        rhi::TextureDesc td;
        td.format = rhi::Format::R16_FLOAT;
        td.width = width; td.height = height;
        td.usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::ShaderResource;
        m_CoCTex = device->CreateTexture(td);
    }

    // ── 高斯模糊 ──
    m_Blur.Initialize(device, width, height);

    // ── Composite PSO ──
    {
        rhi::DescriptorSetLayoutDesc layout;
        layout.bindings = {
            {0, rhi::DescriptorType::CombinedImageSampler, 1, 16},
            {1, rhi::DescriptorType::CombinedImageSampler, 1, 16},
            {2, rhi::DescriptorType::CombinedImageSampler, 1, 16},
        };
        m_CompositeLayout = device->CreateDescriptorSetLayout(layout);
        m_CompositeSet    = device->AllocateDescriptorSet(m_CompositeLayout);

        rhi::ShaderBytecode vs, fs;
        vs.stage = rhi::ShaderStage::Vertex;
        vs.spirv = k_SSAO_vert_spv; vs.entryPoint = "vertexMain";
        fs.stage = rhi::ShaderStage::Pixel;
        fs.spirv = k_DOF_Composite_frag_spv; fs.entryPoint = "fragmentMain";

        rhi::PushConstantRange pc; pc.stageMask = 16; pc.size = 16;
        rhi::PipelineStateDesc d;
        d.vertexShader = &vs; d.pixelShader = &fs;
        d.topology = rhi::PrimitiveTopology::TriangleList;
        d.depthTest = false; d.depthWrite = false; d.depthFormat = rhi::Format::Unknown;
        d.colorAttachmentCount = 1; d.colorFormats[0] = rhi::Format::RGBA16_FLOAT;
        d.pushConstantRanges = {pc}; d.descriptorSetLayouts = {m_CompositeLayout};
        d.debugName = "DOF_Composite";
        m_CompositePSO = device->CreatePipelineState(d);
        HE_ASSERT(m_CompositePSO, "DOFPass: Composite PSO failed");

        rhi::TextureDesc td;
        td.format = rhi::Format::RGBA16_FLOAT;
        td.width = width; td.height = height;
        td.usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::ShaderResource;
        m_Output = device->CreateTexture(td);

        rhi::SamplerDesc sd;
        sd.minFilter = sd.magFilter = rhi::FilterMode::Linear;
        sd.addressU = sd.addressV = rhi::AddressMode::ClampToEdge;
        m_OutSampler = device->CreateSampler(sd);
    }

    m_Ready = true;
    HE_CORE_INFO("DOFPass 初始化完成 ({}x{})", width, height);
    return true;
}

void DOFPass::EnsureInitialized() {
    if (m_Ready || !m_Device) return;
    Initialize(m_Device, m_Width, m_Height);
}

void DOFPass::Shutdown() {
    if (!m_Ready) return;
    m_CoCPSO.reset(); m_CoCTex.reset();
    if (m_Device && m_CoCLayout != rhi::kInvalidLayout) m_Device->DestroyDescriptorSetLayout(m_CoCLayout);
    m_Blur.Shutdown();
    m_CompositePSO.reset(); m_Output.reset(); m_OutSampler.reset();
    if (m_Device && m_CompositeLayout != rhi::kInvalidLayout) m_Device->DestroyDescriptorSetLayout(m_CompositeLayout);
    m_Device = nullptr; m_Ready = false;
}

void DOFPass::OnResize(u32 w, u32 h) {
    if (!m_Ready) return;
    if (w == m_Width && h == m_Height) return;
    m_Width = w; m_Height = h;
    rhi::TextureDesc td; td.format = rhi::Format::R16_FLOAT;
    td.width = w; td.height = h; td.usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::ShaderResource;
    m_CoCTex = m_Device->CreateTexture(td);
    m_Blur.OnResize(w, h);
    td.format = rhi::Format::RGBA16_FLOAT;
    m_Output = m_Device->CreateTexture(td);
}

void DOFPass::SetInputs(rhi::IRHITexture* hdr, rhi::IRHISampler* hdrSampler,
                        rhi::IRHITexture* depth) {
    m_HDRInput   = hdr;
    m_HDRSampler = hdrSampler;
    m_DepthInput = depth;
}

void DOFPass::Render(rhi::IRHICommandList* cmd) {
    if (!m_Ready || !m_Enabled || !m_HDRInput || !m_DepthInput) return;
    u32 w = m_Width, h = m_Height;
    rhi::ClearValue clr{};

    // ── Pass 1: CoC ──
    {
        // 深度采样器：点采样（Nearest）避免边缘混合前景/背景深度导致 CoC 错误
        rhi::SamplerDesc depthSamp;
        depthSamp.minFilter = depthSamp.magFilter = rhi::FilterMode::Nearest;
        depthSamp.addressU = depthSamp.addressV = rhi::AddressMode::ClampToEdge;
        auto ds = m_Device->CreateSampler(depthSamp);  // 简便：每帧创建（可优化为复用）

        m_Device->UpdateDescriptorSet(m_CoCSet, 0, rhi::DescriptorType::CombinedImageSampler,
            m_DepthInput, ds.get());

        struct { float focusDepth, focusRange, maxCoC, _pad; } pc;
        pc.focusDepth = m_FocusDepth; pc.focusRange = m_FocusRange; pc.maxCoC = 0.03f;

        cmd->SetPipeline(m_CoCPSO.get());
        cmd->BindDescriptorSet(0, m_CoCSet);
        cmd->SetViewport({0, (float)h, (float)w, -(float)h, 0, 1});
        cmd->SetScissor({0, 0, w, h});
        cmd->SetPushConstants(0, sizeof(pc), &pc);
        cmd->BeginOffscreenPass(m_CoCTex->GetNativeHandle(), nullptr, w, h, &clr, false);
        cmd->Draw(3);
        cmd->EndOffscreenPass();
    }

    // ── Pass 2: GaussianBlur ──
    m_Blur.SetInput(m_HDRInput, m_HDRSampler);
    m_Blur.PreBind(cmd);
    cmd->BeginOffscreenPass(m_Blur.GetOutput()->GetNativeHandle(), nullptr, w, h, &clr, false);
    m_Blur.Render(cmd);
    cmd->EndOffscreenPass();

    // ── Pass 3: Composite ──
    {
        m_Device->UpdateDescriptorSet(m_CompositeSet, 0, rhi::DescriptorType::CombinedImageSampler,
            m_HDRInput, m_HDRSampler);
        m_Device->UpdateDescriptorSet(m_CompositeSet, 1, rhi::DescriptorType::CombinedImageSampler,
            m_Blur.GetOutput(), m_Blur.GetOutputSampler());
        m_Device->UpdateDescriptorSet(m_CompositeSet, 2, rhi::DescriptorType::CombinedImageSampler,
            m_CoCTex.get(), m_Blur.GetOutputSampler());

        struct { float intensity; float _pad[3]; } pc;
        pc.intensity = m_Intensity;

        cmd->SetPipeline(m_CompositePSO.get());
        cmd->BindDescriptorSet(0, m_CompositeSet);
        cmd->SetViewport({0, (float)h, (float)w, -(float)h, 0, 1});
        cmd->SetScissor({0, 0, w, h});
        cmd->SetPushConstants(0, sizeof(pc), &pc);
        cmd->BeginOffscreenPass(m_Output->GetNativeHandle(), nullptr, w, h, &clr, false);
        cmd->Draw(3);
        cmd->EndOffscreenPass();
    }
}

} // namespace he::render
