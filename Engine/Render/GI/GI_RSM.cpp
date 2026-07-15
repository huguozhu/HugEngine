#include "GI/GI_RSM.h"
#include "RHI/RHI.h"
#include "Core/Log.h"
#include "Core/Assert.h"
#include "Vulkan/VulkanResources.h"
#include "Scene/World.h"
#include "Scene/SceneGraph.h"
#include "Scene/MeshComponent.h"
#include "Pipeline/Material.h"   // GPUObjectData
#include "RSM_Generate.vert.spv.h"
#include "RSM_Generate.frag.spv.h"

namespace he::render {

bool GI_RSM::Initialize(rhi::IRHIDevice* device, u32, u32) {
    m_Device = device;
    HE_CORE_INFO("GI_RSM::Initialize");

    // RSM 纹理（分辨率匹配 Shadow Map）
    m_RSMResolution = 512;

    rhi::TextureDesc posDesc;
    posDesc.format    = rhi::Format::RGBA16_FLOAT;
    posDesc.width     = m_RSMResolution;
    posDesc.height    = m_RSMResolution;
    posDesc.mipLevels = 1;
    posDesc.usage     = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::ShaderResource;
    m_RSMPos = device->CreateTexture(posDesc);

    rhi::TextureDesc fluxDesc;
    fluxDesc.format    = rhi::Format::RGBA16_FLOAT;
    fluxDesc.width     = m_RSMResolution;
    fluxDesc.height    = m_RSMResolution;
    fluxDesc.mipLevels = 1;
    fluxDesc.usage     = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::ShaderResource;
    m_RSMFlux = device->CreateTexture(fluxDesc);

    // 独立深度缓冲（不再复用 CSM ShadowMap，避免布局冲突导致白屏）
    rhi::TextureDesc depthDesc;
    depthDesc.format    = rhi::Format::D32_FLOAT;
    depthDesc.width     = m_RSMResolution;
    depthDesc.height    = m_RSMResolution;
    depthDesc.mipLevels = 1;
    depthDesc.usage     = rhi::TextureUsage::DepthStencil;  // 仅深度附件，无需采样
    m_RSMDepth = device->CreateTexture(depthDesc);

    rhi::SamplerDesc sampDesc;
    sampDesc.minFilter = rhi::FilterMode::Linear;
    sampDesc.magFilter = rhi::FilterMode::Linear;
    sampDesc.addressU  = rhi::AddressMode::ClampToEdge;
    sampDesc.addressV  = rhi::AddressMode::ClampToEdge;
    m_RSMSampler = device->CreateSampler(sampDesc);

    // RSM PSO（双 MRT：pos + normal+flux，深度附件复用 Shadow Map 的 D32）
    rhi::DescriptorSetLayoutDesc layout;
    layout.bindings = {
        { 1, rhi::DescriptorType::StorageBuffer, 1, 17 },  // u_Lights (Vertex | Fragment)
        { 2, rhi::DescriptorType::StorageBuffer, 1, 17 },  // u_Objects (Vertex | Fragment)
    };
    m_RSMLayout = device->CreateDescriptorSetLayout(layout);
    m_RSMSet    = device->AllocateDescriptorSet(m_RSMLayout);

    rhi::ShaderBytecode vs, fs;
    vs.stage      = rhi::ShaderStage::Vertex;
    vs.spirv      = k_RSM_Generate_vert_spv;
    vs.entryPoint = "main";
    fs.stage      = rhi::ShaderStage::Pixel;
    fs.spirv      = k_RSM_Generate_frag_spv;
    fs.entryPoint = "main";

    rhi::VertexInputLayout vertexLayout;
    vertexLayout.stride = sizeof(he::StaticVertex);
    vertexLayout.attributes = {
        { 0, 0, rhi::VertexFormat::Float3, offsetof(he::StaticVertex, position) },
        { 1, 0, rhi::VertexFormat::Float3, offsetof(he::StaticVertex, normal) },
    };

    rhi::PushConstantRange pcRange;
    pcRange.stageMask = 1;  // Vertex only? No — FS needs lightIndex too. Use Vertex|Fragment.
    pcRange.stageMask = 1 | 16;
    pcRange.offset    = 0;
    pcRange.size      = 96;

    rhi::PipelineStateDesc psoDesc;
    psoDesc.vertexShader         = &vs;
    psoDesc.pixelShader          = &fs;
    psoDesc.vertexLayout         = vertexLayout;
    psoDesc.topology             = rhi::PrimitiveTopology::TriangleList;
    psoDesc.depthTest            = true;
    psoDesc.depthWrite           = true;
    psoDesc.depthCompare         = rhi::CompareFunc::LessEqual;
    psoDesc.depthFormat          = rhi::Format::D32_FLOAT;
    psoDesc.colorAttachmentCount = 2;  // MRT 双输出
    psoDesc.colorFormats[0]      = rhi::Format::RGBA16_FLOAT;
    psoDesc.colorFormats[1]      = rhi::Format::RGBA16_FLOAT;
    psoDesc.pushConstantRanges   = { pcRange };
    psoDesc.descriptorSetLayouts = { m_RSMLayout };
    psoDesc.debugName            = "RSM_Generate";

    m_RSMPSO = device->CreatePipelineState(psoDesc);
    HE_ASSERT(m_RSMPSO, "GI_RSM: failed to create RSM PSO");

    m_Ready = true;
    return true;
}

void GI_RSM::Shutdown() {
    m_RSMPos.reset();
    m_RSMFlux.reset();
    m_RSMSampler.reset();
    m_RSMPSO.reset();
    m_Ready = false;
}

void GI_RSM::Update(const SubsystemContext&) {
    // 由 ForwardPipeline::PrepareGI 在 Render 前注入 lightViewProj
}

void GI_RSM::SetLightViewProj(const float4x4& vp, u32 resolution,
                               rhi::IRHIBuffer* objBuf, rhi::IRHISampler* shadowSampler,
                               rhi::DescriptorSetHandle descSet) {
    m_LightVP          = vp;
    m_RSMResolution    = resolution;
    m_ExternalObjBuf   = objBuf;
    m_ExternalDescSet  = descSet;
    // 更新 RSM 采样器绑定（与 Shadow Sampler 一致，但使用 RSM 自有采样器）
    (void)shadowSampler;
}

void GI_RSM::Render(rhi::IRHICommandList* cmd) {
    if (!m_Ready || !m_ExternalObjBuf || !m_RSMDepth) return;
    // 实际渲染委托给 RenderRSMPass（由 ForwardPipeline 在 Render 中调用）
}

void GI_RSM::RenderRSMPass(rhi::IRHICommandList* cmd, he::World& world, he::SceneGraph& sg) {
    if (!m_Ready || !m_ExternalObjBuf || !m_RSMDepth) return;

    m_Device->UpdateDescriptorSet(m_RSMSet, 1,
        rhi::DescriptorType::StorageBuffer, m_ExternalObjBuf);
    m_Device->UpdateDescriptorSet(m_RSMSet, 2,
        rhi::DescriptorType::StorageBuffer, m_ExternalObjBuf);

    cmd->SetPipeline(m_RSMPSO.get());
    cmd->BindDescriptorSet(0, m_RSMSet);

    rhi::ClearValue clears[2]{};

    void* colorViews[2] = {
        m_RSMPos->GetNativeHandle(),
        m_RSMFlux->GetNativeHandle()
    };

    cmd->BeginOffscreenPassMRT(colorViews, 2, m_RSMDepth->GetNativeHandle(),
                               m_RSMResolution, m_RSMResolution, clears, false);
    cmd->SetViewport({ 0, static_cast<float>(m_RSMResolution),
        static_cast<float>(m_RSMResolution), -static_cast<float>(m_RSMResolution),
        0.0f, 1.0f });
    cmd->SetScissor({ 0, 0, m_RSMResolution, m_RSMResolution });

    // 上传对象数据到 GPU（从光源 POV 不需要世界矩阵，直接用 objectIndex 索引）
    auto* objData = static_cast<GPUObjectData*>(m_ExternalObjBuf->Map());
    u32 objectIndex = 0;

    auto renderMesh = [&](he::Entity e, he::MeshComponent& m) {
        if (m.GetIndexCount() == 0 || objectIndex >= MAX_OBJECTS) return;
        objData[objectIndex].worldMatrix = sg.GetWorldMatrix(e);

        // 设置 RSM push constants
        struct alignas(16) RSMPush { float4x4 lightVP; u32 objIdx; u32 lightIdx; u32 _pad[2]; } pc{};
        pc.lightVP = m_LightVP;
        pc.objIdx  = objectIndex;
        pc.lightIdx = 0;  // 使用第一个方向光
        cmd->SetPushConstants(0, sizeof(RSMPush), &pc);

        cmd->SetVertexBuffer(m.GetVertexBuffer().get(), 0);
        cmd->SetIndexBuffer(m.GetIndexBuffer().get());
        cmd->DrawIndexed(m.GetIndexCount());
        objectIndex++;
    };

    world.ForEach<he::MeshComponent>(renderMesh);
    m_ExternalObjBuf->Unmap();

    cmd->EndOffscreenPass();

    // 布局转换：COLOR_ATTACHMENT → SHADER_READ
    cmd->PipelineBarrier(
        rhi::PipelineStage::ColorAttachmentOutput,
        rhi::PipelineStage::FragmentShader,
        rhi::ResourceState::RenderTarget,
        rhi::ResourceState::ShaderResource,
        m_RSMPos.get());
    cmd->PipelineBarrier(
        rhi::PipelineStage::ColorAttachmentOutput,
        rhi::PipelineStage::FragmentShader,
        rhi::ResourceState::RenderTarget,
        rhi::ResourceState::ShaderResource,
        m_RSMFlux.get());
}

void GI_RSM::Bind(rhi::IRHICommandList* cmd) const {
    (void)cmd;  // RSM 纹理通过 PBR 描述符集绑定（扩展 binding 15-16）
}

void GI_RSM::OnResize(u32, u32) {}

} // namespace he::render
