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
#include <cstdio>

namespace he::render {

// RSM 渲染描述符集绑定号（与 RSM_Generate shader 一致）
static constexpr u32 kRSMBindLights  = 1;   // GPULight[] SSBO
static constexpr u32 kRSMBindObjects = 2;   // GPUObjectData[] SSBO

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

    // 第三个附件：VPL 出射辐射度（任务 30）。独立附件换来"一个附件一个量"——
    // 通量此前挤在法线图的 .a 通道里，消费端必须记住"rgb 是法线、a 才是通量"，
    // DDGI 就曾把编码法线当辐射度读（§9.2-AA ②）；而标量通道也装不下 albedo 与光源颜色。
    rhi::TextureDesc radDesc;
    radDesc.format    = rhi::Format::RGBA16_FLOAT;
    radDesc.width     = m_RSMResolution;
    radDesc.height    = m_RSMResolution;
    radDesc.mipLevels = 1;
    radDesc.usage     = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::ShaderResource;
    m_RSMRadiance = device->CreateTexture(radDesc);

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

    // 本 pass 自己的物体缓冲（每帧重写 worldMatrix）。Storage 用法 + CPU 可写。
    rhi::BufferDesc objDesc;
    objDesc.size      = sizeof(GPUObjectData) * MAX_OBJECTS;
    objDesc.usage     = rhi::BufferUsage::Storage;
    objDesc.cpuAccess = true;
    m_ObjectBuf = device->CreateBuffer(objDesc);
    HE_ASSERT(m_ObjectBuf, "GI_RSM: failed to create RSM object buffer");

    // RSM PSO（三 MRT：位置 / 编码法线 / VPL 辐射度；深度附件为 RSM 自己的 D32）
    rhi::DescriptorSetLayoutDesc layout;
    layout.bindings = {
        { kRSMBindLights,  rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskVertex | rhi::kStageMaskFragment },  // u_Lights (Vertex | Fragment)
        { kRSMBindObjects, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskVertex | rhi::kStageMaskFragment },  // u_Objects (Vertex | Fragment)
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
    pcRange.stageMask = rhi::kStageMaskVertex;  // Vertex only? No — FS needs lightIndex too. Use Vertex|Fragment.
    pcRange.stageMask = rhi::kStageMaskVertex | rhi::kStageMaskFragment;
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
    psoDesc.colorAttachmentCount = 3;  // MRT 三输出（位置 / 法线 / VPL 辐射度）
    psoDesc.colorFormats[0]      = rhi::Format::RGBA16_FLOAT;
    psoDesc.colorFormats[1]      = rhi::Format::RGBA16_FLOAT;
    psoDesc.colorFormats[2]      = rhi::Format::RGBA16_FLOAT;
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
    m_RSMRadiance.reset();
    m_RSMSampler.reset();
    m_ObjectBuf.reset();
    m_RSMPSO.reset();
    m_Ready = false;
}

void GI_RSM::Update(const SubsystemContext&) {
    // 由 ForwardPipeline::PrepareGI 在 Render 前注入 lightViewProj
}

void GI_RSM::SetLightViewProj(const float4x4& vp, u32 resolution,
                               rhi::IRHISampler* shadowSampler,
                               rhi::DescriptorSetHandle descSet) {
    m_LightVP          = vp;
    m_RSMResolution    = resolution;
    m_ExternalDescSet  = descSet;
    // 更新 RSM 采样器绑定（与 Shadow Sampler 一致，但使用 RSM 自有采样器）
    (void)shadowSampler;
}

void GI_RSM::Render(rhi::IRHICommandList* cmd) {
    if (!m_Ready || !m_ObjectBuf || !m_RSMDepth) return;
    // 实际渲染委托给 RenderRSMPass（由 ForwardPipeline 在 Render 中调用）
}

void GI_RSM::RenderRSMPass(rhi::IRHICommandList* cmd, he::World& world, he::SceneGraph& sg) {
    if (!m_Ready || !m_ObjectBuf || !m_RSMDepth) return;

    // binding 1 = GPULight[]：**必须是光源缓冲**。此前这里绑的是对象缓冲
    // （与下一行的 binding 2 同一个），于是着色器读到的"光源"其实是 GPUObjectData[0]，
    // 通量恒为垃圾值——RSM 间接光因此长期恒为 0。见 §9.2-AA 与 SetLightBuffer 的注释。
    if (m_ExternalLightBuf) {
        m_Device->UpdateDescriptorSet(m_RSMSet, kRSMBindLights,
            rhi::DescriptorType::StorageBuffer, m_ExternalLightBuf);
    }
    // binding 2 = **本 pass 自己的** GPUObjectData[]。见头文件：不能借用管线的相机可见列表
    // 对象缓冲（索引空间不同，会读到没填过的槽）。
    m_Device->UpdateDescriptorSet(m_RSMSet, kRSMBindObjects,
        rhi::DescriptorType::StorageBuffer, m_ObjectBuf.get());

    cmd->SetPipeline(m_RSMPSO.get());
    cmd->BindDescriptorSet(rhi::kDescSetPerFrame, m_RSMSet);

    // 【长度契约】colorCount(3) 个颜色项 + 末尾一个深度项 = 4 项，深度项在 clears[colorCount]。
    // 此前这里是 `rhi::ClearValue clears[2]{}`（当时 2 个颜色附件）⇒ 实现去读 `clears[2]` 的
    // 越界内存当深度清除值，深度被清成垃圾 ⇒ **几何全被深度测试丢掉**，三张 RSM 图里只剩颜色
    // 清除值 (0,0,0,1)，而 pass 耗时照付（实测 RSM 0.086 ms、图全 0）。这就是 §9.2-AA 里
    // "RSM 间接光整项不产出"的直接原因之一：不是量级太小，而是**根本没有产出**。
    rhi::ClearValue clears[4]{};
    clears[3].depth = 1.0f;   // 深度项：远平面（与 PSO 的 LessEqual + zero-to-one 约定配套）

    void* colorViews[3] = {
        m_RSMPos->GetNativeHandle(),
        m_RSMFlux->GetNativeHandle(),
        m_RSMRadiance->GetNativeHandle()
    };

    cmd->BeginOffscreenPassMRT(colorViews, 3, m_RSMDepth->GetNativeHandle(),
                               m_RSMResolution, m_RSMResolution, clears, false);
    cmd->SetViewport({ 0, static_cast<float>(m_RSMResolution),
        static_cast<float>(m_RSMResolution), -static_cast<float>(m_RSMResolution),
        0.0f, 1.0f });
    cmd->SetScissor({ 0, 0, m_RSMResolution, m_RSMResolution });

    // 上传对象数据到 GPU（从光源 POV 不需要世界矩阵，直接用 objectIndex 索引）
    auto* objData = static_cast<GPUObjectData*>(m_ObjectBuf->Map());
    u32 objectIndex = 0;

    auto renderMesh = [&](he::Entity e, he::MeshComponent& m) {
        if (m.GetIndexCount() == 0 || objectIndex >= MAX_OBJECTS) return;
        objData[objectIndex].worldMatrix = sg.GetWorldMatrix(e);

        // DrawCall 调试 marker：标记当前 RSM 物体（RenderDoc 定位用）
        char label[64];
        snprintf(label, sizeof(label), "RSM Obj#%u", objectIndex);
        cmd->SetDrawDebugLabel(label);

        // 设置 RSM push constants
        // albedo 随 push constant 走：对象缓冲的索引空间是**相机可见性列表**（SceneRenderer
        // 只写可见物体），而本 pass 遍历全部网格、索引是自己的计数器 —— 从对象缓冲读
        // `baseColorFactor` 会读到没填过的槽（= 0），实测只有 3% 的 texel 有非零辐射度。
        struct alignas(16) RSMPush {
            float4x4 lightVP; u32 objIdx; u32 lightIdx; u32 _pad[2]; float4 albedo;
        } pc{};
        pc.lightVP = m_LightVP;
        pc.objIdx  = objectIndex;
        pc.lightIdx = 0;  // 使用第一个方向光
        pc.albedo  = m.baseColorFactor;
        cmd->SetPushConstants(0, sizeof(RSMPush), &pc);

        cmd->SetVertexBuffer(m.GetVertexBuffer().get(), 0);
        cmd->SetIndexBuffer(m.GetIndexBuffer().get());
        cmd->DrawIndexed(m.GetIndexCount());
        objectIndex++;
    };

    world.ForEach<he::MeshComponent>(renderMesh);
    m_ObjectBuf->Unmap();

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
    cmd->PipelineBarrier(
        rhi::PipelineStage::ColorAttachmentOutput,
        rhi::PipelineStage::FragmentShader,
        rhi::ResourceState::RenderTarget,
        rhi::ResourceState::ShaderResource,
        m_RSMRadiance.get());
}

void GI_RSM::Bind(rhi::IRHICommandList* cmd) const {
    (void)cmd;  // RSM 纹理通过 PBR 描述符集绑定（扩展 binding 15-16）
}

void GI_RSM::OnResize(u32, u32) {}

} // namespace he::render
