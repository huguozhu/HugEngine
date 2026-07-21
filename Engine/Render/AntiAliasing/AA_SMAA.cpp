// AntiAliasing/AA_SMAA.cpp — SMAA 子像素形态学抗锯齿实现
//
// 3 Pass 算法：
//   1. EdgeDetection   — 亮度梯度 → R8 边缘纹理（离屏）
//   2. BlendWeight     — 搜索端点距离 → RGBA8 混合权重（离屏）
//   3. Neighborhood    — 4 邻域加权混合 → BackBuffer（RenderPass 内）
//
#include "AA_SMAA.h"
#include "Core/Log.h"
#include "Core/Assert.h"

// 着色器字节码（slangc → SPIR-V → .spv.h）
#include "SSAO.vert.spv.h"                    // 顶点着色器（全屏三角形，共享）
#include "SMAA_EdgeDetect.frag.spv.h"         // Pass 1
#include "SMAA_BlendWeight.frag.spv.h"        // Pass 2
#include "SMAA_Neighborhood.frag.spv.h"       // Pass 3

namespace he::render {

// ============================================================
// Initialize — 创建所有 GPU 资源
// ============================================================
bool AA_SMAA::Initialize(rhi::IRHIDevice* device, u32 width, u32 height) {
    m_Device = device;
    m_Width  = width;
    m_Height = height;

    // ── 采样器 ──
    {
        rhi::SamplerDesc pointDesc;
        pointDesc.minFilter = pointDesc.magFilter = rhi::FilterMode::Nearest;
        pointDesc.addressU = pointDesc.addressV = rhi::AddressMode::ClampToEdge;
        m_PointSampler = device->CreateSampler(pointDesc);

        rhi::SamplerDesc linearDesc;
        linearDesc.minFilter = linearDesc.magFilter = rhi::FilterMode::Linear;
        linearDesc.addressU = linearDesc.addressV = rhi::AddressMode::ClampToEdge;
        m_LinearSampler = device->CreateSampler(linearDesc);
    }

    // ── 中间纹理（分辨率相关）──
    {
        // 边缘纹理 R8_UNORM
        rhi::TextureDesc edgeDesc;
        edgeDesc.format   = rhi::Format::R8_UNORM;
        edgeDesc.width    = width;
        edgeDesc.height   = height;
        edgeDesc.usage    = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::ShaderResource;
        m_EdgeTex = device->CreateTexture(edgeDesc);

        // 混合权重纹理 RGBA8_UNORM
        rhi::TextureDesc blendDesc;
        blendDesc.format = rhi::Format::RGBA8_UNORM;
        blendDesc.width  = width;
        blendDesc.height = height;
        blendDesc.usage  = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::ShaderResource;
        m_BlendTex = device->CreateTexture(blendDesc);
    }

    // ── 3 个 PSO ──
    CreateEdgePSO();
    CreateBlendPSO();
    CreateNeighborPSO();

    m_Ready = true;
    HE_CORE_INFO("AA_SMAA initialized ({}x{})", width, height);
    return true;
}

// ============================================================
// Shutdown — 销毁所有 GPU 资源
// ============================================================
void AA_SMAA::Shutdown() {
    DestroyAllPSOs();

    if (m_Device) {
        if (m_EdgeLayout  != rhi::kInvalidLayout) m_Device->DestroyDescriptorSetLayout(m_EdgeLayout);
        if (m_BlendLayout != rhi::kInvalidLayout) m_Device->DestroyDescriptorSetLayout(m_BlendLayout);
        if (m_NeighborLayout != rhi::kInvalidLayout) m_Device->DestroyDescriptorSetLayout(m_NeighborLayout);
    }

    m_EdgeTex.reset();
    m_BlendTex.reset();
    m_PointSampler.reset();
    m_LinearSampler.reset();

    m_EdgeLayout    = rhi::kInvalidLayout;
    m_EdgeSet       = rhi::kInvalidSet;
    m_BlendLayout   = rhi::kInvalidLayout;
    m_BlendSet      = rhi::kInvalidSet;
    m_NeighborLayout = rhi::kInvalidLayout;
    m_NeighborSet   = rhi::kInvalidSet;

    m_Device = nullptr;
    m_Ready  = false;
}

// ============================================================
// OnResize — 重建分辨率相关的中间纹理
// ============================================================
void AA_SMAA::OnResize(u32 width, u32 height) {
    if (width == m_Width && height == m_Height) return;
    if (!m_Device) return;

    m_Width  = width;
    m_Height = height;

    // 重建边缘纹理
    rhi::TextureDesc edgeDesc;
    edgeDesc.format = rhi::Format::R8_UNORM;
    edgeDesc.width  = width;
    edgeDesc.height = height;
    edgeDesc.usage  = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::ShaderResource;
    m_EdgeTex = m_Device->CreateTexture(edgeDesc);

    // 重建混合权重纹理
    rhi::TextureDesc blendDesc;
    blendDesc.format = rhi::Format::RGBA8_UNORM;
    blendDesc.width  = width;
    blendDesc.height = height;
    blendDesc.usage  = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::ShaderResource;
    m_BlendTex = m_Device->CreateTexture(blendDesc);
}

// ============================================================
// SetInput — 设置上游 LDR 颜色输入
// ============================================================
void AA_SMAA::SetInput(rhi::IRHITexture* color, rhi::IRHISampler* sampler) {
    m_Input        = color;
    m_InputSampler = sampler;

    // 更新 EdgeDetection Pass 的输入描述符（binding 0 = 输入颜色）
    if (m_Input && m_InputSampler && m_EdgeSet != rhi::kInvalidSet) {
        m_Device->UpdateDescriptorSet(m_EdgeSet, 0,
            rhi::DescriptorType::CombinedImageSampler, m_Input, m_InputSampler);
    }
}

// ============================================================
// Render — Pass 1+2（离屏渲染）
// ============================================================
void AA_SMAA::Render(rhi::IRHICommandList* cmd) {
    if (!m_Ready || !m_Enabled || !m_Input) return;

    // ── Pass 1: Edge Detection → m_EdgeTex ──
    {
        cmd->SetPipeline(m_EdgePSO.get());
        cmd->BindDescriptorSet(rhi::kDescSetPerFrame, m_EdgeSet);
        cmd->SetViewport({0, (float)m_Height, (float)m_Width, -(float)m_Height, 0, 1});
        cmd->SetScissor({0, 0, m_Width, m_Height});

        struct { float2 invSize; float2 _pad; } pc;
        pc.invSize = float2(1.0f / m_Width, 1.0f / m_Height);
        pc._pad    = float2(0, 0);
        cmd->SetPushConstants(0, sizeof(pc), &pc);

        rhi::ClearValue edgeClr{};
        edgeClr.color[0] = 0.0f; edgeClr.color[1] = 0.0f;
        edgeClr.color[2] = 0.0f; edgeClr.color[3] = 0.0f;
        cmd->BeginOffscreenPass(m_EdgeTex->GetNativeHandle(), nullptr,
            m_Width, m_Height, &edgeClr, false);
        cmd->Draw(3);  // 全屏三角形
        cmd->EndOffscreenPass();
    }

    // 边缘纹理布局转换：RenderTarget → ShaderResource（供 Pass 2 采样）
    cmd->PipelineBarrier(
        rhi::PipelineStage::ColorAttachmentOutput,
        rhi::PipelineStage::FragmentShader,
        rhi::ResourceState::RenderTarget,
        rhi::ResourceState::ShaderResource,
        m_EdgeTex.get());

    // ── Pass 2: Blending Weight Calculation → m_BlendTex ──
    {
        // 更新描述符：binding 0 = 边缘纹理（Point 采样）
        m_Device->UpdateDescriptorSet(m_BlendSet, 0,
            rhi::DescriptorType::CombinedImageSampler,
            m_EdgeTex.get(), m_PointSampler.get());

        cmd->SetPipeline(m_BlendPSO.get());
        cmd->BindDescriptorSet(rhi::kDescSetPerFrame, m_BlendSet);
        cmd->SetViewport({0, (float)m_Height, (float)m_Width, -(float)m_Height, 0, 1});
        cmd->SetScissor({0, 0, m_Width, m_Height});

        struct { float2 invSize; float2 _pad; } pc;
        pc.invSize = float2(1.0f / m_Width, 1.0f / m_Height);
        pc._pad    = float2(0, 0);
        cmd->SetPushConstants(0, sizeof(pc), &pc);

        rhi::ClearValue blendClr{};
        blendClr.color[0] = 0.0f; blendClr.color[1] = 0.0f;
        blendClr.color[2] = 0.0f; blendClr.color[3] = 0.0f;
        cmd->BeginOffscreenPass(m_BlendTex->GetNativeHandle(), nullptr,
            m_Width, m_Height, &blendClr, false);
        cmd->Draw(3);
        cmd->EndOffscreenPass();
    }

    // 混合权重纹理布局转换：RenderTarget → ShaderResource（供 Pass 3 采样）
    cmd->PipelineBarrier(
        rhi::PipelineStage::ColorAttachmentOutput,
        rhi::PipelineStage::FragmentShader,
        rhi::ResourceState::RenderTarget,
        rhi::ResourceState::ShaderResource,
        m_BlendTex.get());
}

// ============================================================
// RenderFinalPass — Pass 3: Neighborhood Blending（RenderPass 内调用）
// 调用者已进入 BeginRenderPass(BackBuffer)
// ============================================================
void AA_SMAA::RenderFinalPass(rhi::IRHICommandList* cmd) {
    if (!m_Ready || !m_Enabled || !m_Input) return;

    // 更新描述符
    // binding 0 = 原始输入颜色（Linear 采样）
    m_Device->UpdateDescriptorSet(m_NeighborSet, 0,
        rhi::DescriptorType::CombinedImageSampler,
        m_Input, m_InputSampler);
    // binding 1 = 混合权重纹理（Point 采样）
    m_Device->UpdateDescriptorSet(m_NeighborSet, 1,
        rhi::DescriptorType::CombinedImageSampler,
        m_BlendTex.get(), m_PointSampler.get());

    cmd->SetPipeline(m_NeighborPSO.get());
    cmd->BindDescriptorSet(rhi::kDescSetPerFrame, m_NeighborSet);
    cmd->SetViewport({0, (float)m_Height, (float)m_Width, -(float)m_Height, 0, 1});
    cmd->SetScissor({0, 0, m_Width, m_Height});

    struct { float2 invSize; float2 _pad; } pc;
    pc.invSize = float2(1.0f / m_Width, 1.0f / m_Height);
    pc._pad    = float2(0, 0);
    cmd->SetPushConstants(0, sizeof(pc), &pc);

    cmd->Draw(3);
}

// ============================================================
// CreateEdgePSO — Pass 1: Edge Detection PSO
// ============================================================
void AA_SMAA::CreateEdgePSO() {
    // 描述符集布局：binding 0 = 输入颜色（CombinedImageSampler, Fragment stage）
    rhi::DescriptorSetLayoutDesc layout;
    layout.bindings = {{0, rhi::DescriptorType::CombinedImageSampler, 1, 16}};  // stage=16 = Fragment
    m_EdgeLayout = m_Device->CreateDescriptorSetLayout(layout);
    m_EdgeSet    = m_Device->AllocateDescriptorSet(m_EdgeLayout);

    // 着色器
    rhi::ShaderBytecode vs, fs;
    vs.stage      = rhi::ShaderStage::Vertex;
    vs.spirv      = k_SSAO_vert_spv;
    vs.entryPoint = "vertexMain";
    fs.stage      = rhi::ShaderStage::Pixel;
    fs.spirv      = k_SMAA_EdgeDetect_frag_spv;
    fs.entryPoint = "fragmentMain";

    rhi::PushConstantRange pcr;
    pcr.stageMask = 16;  // Fragment
    pcr.offset    = 0;
    pcr.size      = 16;  // float2 invScreenSize + float2 padding

    rhi::PipelineStateDesc d;
    d.vertexShader        = &vs;
    d.pixelShader         = &fs;
    d.topology            = rhi::PrimitiveTopology::TriangleList;
    d.depthTest           = false;
    d.depthWrite          = false;
    d.depthFormat         = rhi::Format::Unknown;
    d.colorAttachmentCount = 1;
    d.colorFormats[0]      = rhi::Format::R8_UNORM;
    d.pushConstantRanges   = {pcr};
    d.descriptorSetLayouts = {m_EdgeLayout};
    d.debugName            = "SMAA_EdgeDetect";
    m_EdgePSO = m_Device->CreatePipelineState(d);
    HE_ASSERT(m_EdgePSO, "AA_SMAA: EdgeDetection PSO creation failed");
}

// ============================================================
// CreateBlendPSO — Pass 2: Blending Weight PSO
// ============================================================
void AA_SMAA::CreateBlendPSO() {
    // 描述符集布局：binding 0 = 边缘纹理（CombinedImageSampler, Fragment stage）
    rhi::DescriptorSetLayoutDesc layout;
    layout.bindings = {{0, rhi::DescriptorType::CombinedImageSampler, 1, 16}};
    m_BlendLayout = m_Device->CreateDescriptorSetLayout(layout);
    m_BlendSet    = m_Device->AllocateDescriptorSet(m_BlendLayout);

    // 着色器
    rhi::ShaderBytecode vs, fs;
    vs.stage      = rhi::ShaderStage::Vertex;
    vs.spirv      = k_SSAO_vert_spv;
    vs.entryPoint = "vertexMain";
    fs.stage      = rhi::ShaderStage::Pixel;
    fs.spirv      = k_SMAA_BlendWeight_frag_spv;
    fs.entryPoint = "fragmentMain";

    rhi::PushConstantRange pcr;
    pcr.stageMask = 16;
    pcr.offset    = 0;
    pcr.size      = 16;

    rhi::PipelineStateDesc d;
    d.vertexShader        = &vs;
    d.pixelShader         = &fs;
    d.topology            = rhi::PrimitiveTopology::TriangleList;
    d.depthTest           = false;
    d.depthWrite          = false;
    d.depthFormat         = rhi::Format::Unknown;
    d.colorAttachmentCount = 1;
    d.colorFormats[0]      = rhi::Format::RGBA8_UNORM;
    d.pushConstantRanges   = {pcr};
    d.descriptorSetLayouts = {m_BlendLayout};
    d.debugName            = "SMAA_BlendWeight";
    m_BlendPSO = m_Device->CreatePipelineState(d);
    HE_ASSERT(m_BlendPSO, "AA_SMAA: BlendWeight PSO creation failed");
}

// ============================================================
// CreateNeighborPSO — Pass 3: Neighborhood Blending PSO
// ============================================================
void AA_SMAA::CreateNeighborPSO() {
    // 描述符集布局：
    //   binding 0 = 输入颜色（CombinedImageSampler, Fragment stage）
    //   binding 1 = 混合权重（CombinedImageSampler, Fragment stage）
    rhi::DescriptorSetLayoutDesc layout;
    layout.bindings = {
        {0, rhi::DescriptorType::CombinedImageSampler, 1, 16},
        {1, rhi::DescriptorType::CombinedImageSampler, 1, 16},
    };
    m_NeighborLayout = m_Device->CreateDescriptorSetLayout(layout);
    m_NeighborSet    = m_Device->AllocateDescriptorSet(m_NeighborLayout);

    // 着色器
    rhi::ShaderBytecode vs, fs;
    vs.stage      = rhi::ShaderStage::Vertex;
    vs.spirv      = k_SSAO_vert_spv;
    vs.entryPoint = "vertexMain";
    fs.stage      = rhi::ShaderStage::Pixel;
    fs.spirv      = k_SMAA_Neighborhood_frag_spv;
    fs.entryPoint = "fragmentMain";

    rhi::PushConstantRange pcr;
    pcr.stageMask = 16;
    pcr.offset    = 0;
    pcr.size      = 16;

    rhi::PipelineStateDesc d;
    d.vertexShader        = &vs;
    d.pixelShader         = &fs;
    d.topology            = rhi::PrimitiveTopology::TriangleList;
    d.depthTest           = false;
    d.depthWrite          = false;
    d.depthFormat         = rhi::Format::Unknown;
    d.colorAttachmentCount = 1;
    d.colorFormats[0]      = rhi::Format::BGRA8_UNORM;  // 终端输出到 BackBuffer
    d.pushConstantRanges   = {pcr};
    d.descriptorSetLayouts = {m_NeighborLayout};
    d.debugName            = "SMAA_Neighborhood";
    m_NeighborPSO = m_Device->CreatePipelineState(d);
    HE_ASSERT(m_NeighborPSO, "AA_SMAA: Neighborhood PSO creation failed");
}

// ============================================================
// DestroyAllPSOs — 销毁 3 个 PSO
// ============================================================
void AA_SMAA::DestroyAllPSOs() {
    m_EdgePSO.reset();
    m_BlendPSO.reset();
    m_NeighborPSO.reset();
}

} // namespace he::render
