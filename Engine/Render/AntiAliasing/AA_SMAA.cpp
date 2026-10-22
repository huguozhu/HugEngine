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

// SMAA 各 pass 描述符集绑定号
static constexpr u32 kSMAAEdgeBindInput     = 0;   // 边缘检测输入
static constexpr u32 kSMAABlendBindInput    = 0;   // 混合权重输入
static constexpr u32 kSMAANeighborBindInput = 0;   // 邻域检测输入
static constexpr u32 kSMAANeighborBindBlend = 1;   // 混合权重纹理

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
        m_PointSampler = device->CreateSamplerHandle(pointDesc);

        rhi::SamplerDesc linearDesc;
        linearDesc.minFilter = linearDesc.magFilter = rhi::FilterMode::Linear;
        linearDesc.addressU = linearDesc.addressV = rhi::AddressMode::ClampToEdge;
        m_LinearSampler = device->CreateSamplerHandle(linearDesc);
    }

    // ── 中间纹理（分辨率相关）──
    {
        // 边缘纹理 R8_UNORM
        rhi::TextureDesc edgeDesc;
        edgeDesc.format   = rhi::Format::R8_UNORM;
        edgeDesc.width    = width;
        edgeDesc.height   = height;
        edgeDesc.usage    = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::ShaderResource;
        m_EdgeTex = device->CreateTextureHandle(edgeDesc);

        // 混合权重纹理 RGBA8_UNORM
        rhi::TextureDesc blendDesc;
        blendDesc.format = rhi::Format::RGBA8_UNORM;
        blendDesc.width  = width;
        blendDesc.height = height;
        blendDesc.usage  = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::ShaderResource;
        m_BlendTex = device->CreateTextureHandle(blendDesc);
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

    m_Device->DestroyTextureHandle(m_EdgeTex); m_EdgeTex = {};
    m_Device->DestroyTextureHandle(m_BlendTex); m_BlendTex = {};
    m_Device->DestroySamplerHandle(m_PointSampler); m_PointSampler = {};
    m_Device->DestroySamplerHandle(m_LinearSampler); m_LinearSampler = {};

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
    m_EdgeTex = m_Device->CreateTextureHandle(edgeDesc);

    // 重建混合权重纹理
    rhi::TextureDesc blendDesc;
    blendDesc.format = rhi::Format::RGBA8_UNORM;
    blendDesc.width  = width;
    blendDesc.height = height;
    blendDesc.usage  = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::ShaderResource;
    m_BlendTex = m_Device->CreateTextureHandle(blendDesc);
}

// ============================================================
// SetInput — 设置上游 LDR 颜色输入
// ============================================================
void AA_SMAA::SetInput(rhi::IRHITexture* color, rhi::IRHISampler* sampler) {
    m_Input        = color;
    m_InputSampler = sampler;

    // 更新 EdgeDetection Pass 的输入描述符（binding 0 = 输入颜色）
    if (m_Input && m_InputSampler && m_EdgeSet != rhi::kInvalidSet) {
        m_Device->UpdateDescriptorSet(m_EdgeSet, kSMAAEdgeBindInput,
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
        cmd->SetPipeline(ResP(m_EdgePSO));
        cmd->BindDescriptorSet(rhi::kDescSetPerFrame, m_EdgeSet);
        cmd->SetViewport({0, (float)m_Height, (float)m_Width, -(float)m_Height, 0, 1});
        cmd->SetScissor({0, 0, m_Width, m_Height});

        struct { float2 invSize; float2 _pad; } pc;
        pc.invSize = float2(1.0f / m_Width, 1.0f / m_Height);
        pc._pad    = float2(0, 0);
        cmd->SetPushConstants(0, sizeof(pc), &pc);

        rhi::ClearValue edgeClr{};
        edgeClr.color[0] = 0.0f;
        edgeClr.color[1] = 0.0f;
        edgeClr.color[2] = 0.0f;
        edgeClr.color[3] = 0.0f;
        cmd->BeginOffscreenPass(ResT(m_EdgeTex)->GetNativeHandle(), nullptr,
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
        ResT(m_EdgeTex));

    // ── Pass 2: Blending Weight Calculation → m_BlendTex ──
    {
        // 更新描述符：binding 0 = 边缘纹理（Point 采样）
        m_Device->UpdateDescriptorSet(m_BlendSet, kSMAABlendBindInput,
            rhi::DescriptorType::CombinedImageSampler,
            ResT(m_EdgeTex), ResS(m_PointSampler));

        cmd->SetPipeline(ResP(m_BlendPSO));
        cmd->BindDescriptorSet(rhi::kDescSetPerFrame, m_BlendSet);
        cmd->SetViewport({0, (float)m_Height, (float)m_Width, -(float)m_Height, 0, 1});
        cmd->SetScissor({0, 0, m_Width, m_Height});

        struct { float2 invSize; float2 _pad; } pc;
        pc.invSize = float2(1.0f / m_Width, 1.0f / m_Height);
        pc._pad    = float2(0, 0);
        cmd->SetPushConstants(0, sizeof(pc), &pc);

        rhi::ClearValue blendClr{};
        blendClr.color[0] = 0.0f;
        blendClr.color[1] = 0.0f;
        blendClr.color[2] = 0.0f;
        blendClr.color[3] = 0.0f;
        cmd->BeginOffscreenPass(ResT(m_BlendTex)->GetNativeHandle(), nullptr,
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
        ResT(m_BlendTex));
}

// ============================================================
// RenderFinalPass — Pass 3: Neighborhood Blending（RenderPass 内调用）
// 调用者已进入 BeginRenderPass(BackBuffer)
// ============================================================
void AA_SMAA::RenderFinalPass(rhi::IRHICommandList* cmd) {
    if (!m_Ready || !m_Enabled || !m_Input) return;

    // 更新描述符
    // binding 0 = 原始输入颜色（Linear 采样）
    m_Device->UpdateDescriptorSet(m_NeighborSet, kSMAANeighborBindInput,
        rhi::DescriptorType::CombinedImageSampler,
        m_Input, m_InputSampler);
    // binding 1 = 混合权重纹理（Point 采样）
    m_Device->UpdateDescriptorSet(m_NeighborSet, kSMAANeighborBindBlend,
        rhi::DescriptorType::CombinedImageSampler,
        ResT(m_BlendTex), ResS(m_PointSampler));

    cmd->SetPipeline(ResP(m_NeighborPSO));
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
    layout.bindings = {{0, rhi::DescriptorType::CombinedImageSampler, 1, rhi::kStageMaskFragment}};  // stage=16 = Fragment
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
    pcr.stageMask = rhi::kStageMaskFragment;  // Fragment
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
    m_EdgePSO = m_Device->CreatePipelineStateHandle(d);
    HE_ASSERT(m_EdgePSO.IsValid(), "AA_SMAA: EdgeDetection PSO 句柄创建失败");
}

// ============================================================
// CreateBlendPSO — Pass 2: Blending Weight PSO
// ============================================================
void AA_SMAA::CreateBlendPSO() {
    // 描述符集布局：binding 0 = 边缘纹理（CombinedImageSampler, Fragment stage）
    rhi::DescriptorSetLayoutDesc layout;
    layout.bindings = {{0, rhi::DescriptorType::CombinedImageSampler, 1, rhi::kStageMaskFragment}};
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
    pcr.stageMask = rhi::kStageMaskFragment;
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
    m_BlendPSO = m_Device->CreatePipelineStateHandle(d);
    HE_ASSERT(m_BlendPSO.IsValid(), "AA_SMAA: BlendWeight PSO 句柄创建失败");
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
        {0, rhi::DescriptorType::CombinedImageSampler, 1, rhi::kStageMaskFragment},
        {1, rhi::DescriptorType::CombinedImageSampler, 1, rhi::kStageMaskFragment},
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
    pcr.stageMask = rhi::kStageMaskFragment;
    pcr.offset    = 0;
    pcr.size      = 16;

    rhi::PipelineStateDesc d;
    d.vertexShader        = &vs;
    d.pixelShader         = &fs;
    d.topology            = rhi::PrimitiveTopology::TriangleList;
    d.depthTest           = false;
    d.depthWrite          = false;
    // 终稿 pass 输出到交换链 RenderPass（带 D32 深度附件），管线 RP 必须同样声明深度，
    // 否则报 VUID-vkCmdDraw-renderPass-02684；前两个 pass 写自己的离屏目标，保持 Unknown。
    d.depthFormat         = rhi::Format::D32_FLOAT;
    d.colorAttachmentCount = 1;
    d.colorFormats[0]      = rhi::Format::BGRA8_UNORM;  // 终端输出到 BackBuffer
    d.pushConstantRanges   = {pcr};
    d.descriptorSetLayouts = {m_NeighborLayout};
    d.debugName            = "SMAA_Neighborhood";
    m_NeighborPSO = m_Device->CreatePipelineStateHandle(d);
    HE_ASSERT(m_NeighborPSO.IsValid(), "AA_SMAA: Neighborhood PSO 句柄创建失败");
}

// ============================================================
// DestroyAllPSOs — 销毁 3 个 PSO
// ============================================================
void AA_SMAA::DestroyAllPSOs() {
    m_Device->DestroyPipelineStateHandle(m_EdgePSO); m_EdgePSO = {};
    m_Device->DestroyPipelineStateHandle(m_BlendPSO); m_BlendPSO = {};
    m_Device->DestroyPipelineStateHandle(m_NeighborPSO); m_NeighborPSO = {};
}

} // namespace he::render
