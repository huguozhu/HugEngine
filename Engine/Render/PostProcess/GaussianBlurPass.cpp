// PostProcess/GaussianBlurPass.cpp — 7×7 可分离高斯模糊实现
#include "GaussianBlurPass.h"
#include "Core/Log.h"
#include "Core/Assert.h"
#include "Math/Math.h"  // float2

// 着色器字节码
#include "SSAO.vert.spv.h"              // 复用通用全屏三角形顶点着色器
#include "GaussianBlur.frag.spv.h"

namespace he::render {

bool GaussianBlurPass::Initialize(rhi::IRHIDevice* device, u32 width, u32 height) {
    m_Device = device;
    m_Width  = width;
    m_Height = height;

    // 描述符集布局：单个 CombinedImageSampler
    rhi::DescriptorSetLayoutDesc layoutDesc;
    layoutDesc.bindings = {
        {0, rhi::DescriptorType::CombinedImageSampler, 1, 16}  // stageMask=16 (Fragment)
    };
    m_Layout = device->CreateDescriptorSetLayout(layoutDesc);
    m_Set    = device->AllocateDescriptorSet(m_Layout);

    // 着色器
    rhi::ShaderBytecode vs, fs;
    vs.stage = rhi::ShaderStage::Vertex;
    vs.spirv = k_SSAO_vert_spv;
    vs.entryPoint = "vertexMain";
    fs.stage = rhi::ShaderStage::Pixel;
    fs.spirv = k_GaussianBlur_frag_spv;
    fs.entryPoint = "fragmentMain";

    // Push constant：texelSize (float2)
    rhi::PushConstantRange pcRange;
    pcRange.stageMask = 16;  // Fragment
    pcRange.size      = 16;  // float2 + float2 padding

    // PSO：全屏三角形，无深度
    rhi::PipelineStateDesc psoDesc;
    psoDesc.vertexShader        = &vs;
    psoDesc.pixelShader         = &fs;
    psoDesc.topology            = rhi::PrimitiveTopology::TriangleList;
    psoDesc.depthTest           = false;
    psoDesc.depthWrite          = false;
    psoDesc.depthFormat         = rhi::Format::Unknown;
    psoDesc.colorAttachmentCount = 1;
    psoDesc.colorFormats[0]     = rhi::Format::RGBA16_FLOAT;
    psoDesc.pushConstantRanges  = {pcRange};
    psoDesc.descriptorSetLayouts = {m_Layout};
    psoDesc.debugName           = "GaussianBlur";

    m_PSO = device->CreatePipelineState(psoDesc);
    HE_ASSERT(m_PSO, "GaussianBlurPass: PSO 创建失败");

    // 输出纹理（半分辨率，HDR 格式）
    {
        rhi::TextureDesc td;
        td.format = rhi::Format::RGBA16_FLOAT;
        td.width  = m_Width;
        td.height = m_Height;
        td.usage  = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::ShaderResource;
        m_Output = device->CreateTexture(td);
    }

    // 采样器（线性插值 + 边缘钳制）
    {
        rhi::SamplerDesc sd;
        sd.minFilter = sd.magFilter = rhi::FilterMode::Linear;
        sd.addressU = sd.addressV = rhi::AddressMode::ClampToEdge;
        m_OutSampler = device->CreateSampler(sd);
    }

    m_Ready = true;
    HE_CORE_INFO("GaussianBlurPass 初始化完成 ({}x{})", m_Width, m_Height);
    return true;
}

void GaussianBlurPass::Shutdown() {
    m_PSO.reset();
    m_Output.reset();
    m_OutSampler.reset();
    if (m_Device && m_Layout != rhi::kInvalidLayout) {
        m_Device->DestroyDescriptorSetLayout(m_Layout);
    }
    m_Device = nullptr;
    m_Ready  = false;
}

void GaussianBlurPass::OnResize(u32 w, u32 h) {
    if (w == m_Width && h == m_Height) return;
    m_Width  = w;
    m_Height = h;
    // 重建输出纹理
    rhi::TextureDesc td;
    td.format = rhi::Format::RGBA16_FLOAT;
    td.width  = w;
    td.height = h;
    td.usage  = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::ShaderResource;
    m_Output = m_Device->CreateTexture(td);
}

void GaussianBlurPass::SetInput(rhi::IRHITexture* color, rhi::IRHISampler* sampler) {
    m_Input        = color;
    m_InputSampler = sampler;
    if (m_Input && m_InputSampler) {
        m_Device->UpdateDescriptorSet(m_Set, 0,
            rhi::DescriptorType::CombinedImageSampler, m_Input, m_InputSampler);
    }
}

void GaussianBlurPass::Render(rhi::IRHICommandList* cmd) {
    if (!m_Ready || !m_Input) return;

    cmd->SetPipeline(m_PSO.get());
    cmd->BindDescriptorSet(rhi::kDescSetPerFrame, m_Set);
    cmd->SetViewport({0, (float)m_Height, (float)m_Width, -(float)m_Height, 0, 1});
    cmd->SetScissor({0, 0, m_Width, m_Height});

    struct {
        float2 texelSize;
        float2 _pad;
    } pc;
    pc.texelSize = float2(1.0f / float(m_Width), 1.0f / float(m_Height));
    cmd->SetPushConstants(0, sizeof(pc), &pc);

    cmd->Draw(3);  // 全屏三角形
}

} // namespace he::render
