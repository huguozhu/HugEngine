// ============================================================
// GBufferRenderer.cpp — GBuffer 纹理所有权 + 渲染实现
// 负责 5 MRT + Depth 纹理创建、PSO 创建、描述符集管理、
// RenderGraph 导入接口。内部委托给 IGBufferRenderer（CPU/GPU 策略）。
// ============================================================
#include "Pipeline/GBufferRenderer.h"
#include "Pipeline/GBufferRenderer_CPU.h"
#include "Pipeline/GBufferRenderer_GPU.h"
#include "Asset/BindlessTextureManager.h"
#include "Scene/MeshComponent.h"
#include "Core/Log.h"
#include "Core/Assert.h"

// SPIR-V 嵌入头文件
#include "GBuffer.vert.spv.h"
#include "GBuffer.frag.spv.h"

namespace he::render {

bool GBufferRenderer::Initialize(rhi::IRHIDevice* device, u32 width, u32 height) {
    m_Width  = width;
    m_Height = height;

    // ── 1. 创建 GBuffer 纹理 ──
    CreateTextures(device);

    // ── 2. 创建 GBuffer PSO ──
    CreatePSO(device);

    // ── 3. 创建描述符集 ──
    CreateDescriptorSet(device);

    // ── 4. 填充 GBufferContext ──
    m_Ctx.device    = device;
    m_Ctx.width     = width;
    m_Ctx.height    = height;
    m_Ctx.gbA       = m_A.get();
    m_Ctx.gbB       = m_B.get();
    m_Ctx.gbC       = m_C.get();
    m_Ctx.gbVel     = m_D.get();
    m_Ctx.gbDepth   = m_Depth.get();
    m_Ctx.gbWorldPos = m_E.get();
    m_Ctx.gbDisneyA = m_F.get();
    m_Ctx.gbDisneyB = m_G.get();
    m_Ctx.pso       = m_PSO.get();
    m_Ctx.descSet   = m_Set;

    // ── 5. 创建策略模式渲染器 ──
    SetMode(m_Mode);

    HE_CORE_INFO("GBufferRenderer: 初始化完成 ({}x{}, mode={})",
                 width, height, m_Mode == Mode::CPU ? "CPU" : "GPU");
    return true;
}

void GBufferRenderer::Shutdown() {
    // 先关闭渲染器（使用 GBufferContext 的裸指针）
    if (m_Renderer) {
        m_Renderer->Shutdown();
        m_Renderer.reset();
    }

    // 销毁描述符集
    if (m_Set != rhi::kInvalidSet && m_Ctx.device) {
        // DescriptorSet 由设备管理，随设备销毁自动释放
    }
    if (m_Layout != rhi::kInvalidLayout && m_Ctx.device) {
        m_Ctx.device->DestroyDescriptorSetLayout(m_Layout);
        m_Layout = rhi::kInvalidLayout;
    }

    // 销毁 PSO + 纹理
    m_PSO.reset();
    m_Depth.reset();
    m_G.reset();
    m_F.reset();
    m_E.reset();
    m_D.reset();
    m_C.reset();
    m_B.reset();
    m_A.reset();

    m_Ctx.device = nullptr;
    m_Width = m_Height = 0;
}

void GBufferRenderer::OnResize(u32 width, u32 height) {
    if (width == m_Width && height == m_Height) return;
    m_Width  = width;
    m_Height = height;

    // 重建所有纹理（旧纹理通过 unique_ptr 自动释放）
    CreateTextures(m_Ctx.device);

    // 更新 GBufferContext 中的裸指针
    m_Ctx.width     = width;
    m_Ctx.height    = height;
    m_Ctx.gbA       = m_A.get();
    m_Ctx.gbB       = m_B.get();
    m_Ctx.gbC       = m_C.get();
    m_Ctx.gbVel     = m_D.get();
    m_Ctx.gbDepth   = m_Depth.get();
    m_Ctx.gbWorldPos = m_E.get();
    m_Ctx.gbDisneyA = m_F.get();
    m_Ctx.gbDisneyB = m_G.get();
}

GBufferRenderer::Handles GBufferRenderer::ImportToRenderGraph(RenderGraph& rg) {
    Handles h;
    h.albedo   = rg.ImportTexture("GB_A",        m_A.get());
    h.normal   = rg.ImportTexture("GB_B",        m_B.get());
    h.emissive  = rg.ImportTexture("GB_C",        m_C.get());
    h.velocity = rg.ImportTexture("GB_Vel",      m_D.get());
    h.worldPos = rg.ImportTexture("GB_WorldPos", m_E.get());
    h.disneyA  = rg.ImportTexture("GB_DisneyA",  m_F.get());
    h.disneyB  = rg.ImportTexture("GB_DisneyB",  m_G.get());
    h.depth    = rg.ImportTexture("GB_Depth",    m_Depth.get());
    return h;
}

void GBufferRenderer::Render(rhi::IRHICommandList* cmd, he::World& world,
                              he::SceneGraph& sg, const CameraData& camera) {
    if (m_Renderer) {
        m_Renderer->Render(cmd, m_Ctx, world, sg, camera);
    }
}

void GBufferRenderer::SetMode(Mode mode) {
    m_Mode = mode;
    if (m_Renderer) m_Renderer->Shutdown();

    if (mode == Mode::GPU)
        m_Renderer = std::make_unique<GBufferRenderer_GPU>();
    else
        m_Renderer = std::make_unique<GBufferRenderer_CPU>();

    // 如果 context 已初始化（device 不为空），则初始化渲染器
    if (m_Ctx.device) {
        m_Renderer->Initialize(m_Ctx);
    }
}

// ============================================================
// 内部实现
// ============================================================

void GBufferRenderer::CreateTextures(rhi::IRHIDevice* device) {
    // GBuffer A/B/C/E: RGBA16_FLOAT（RenderTarget + ShaderResource）
    auto createRGBA16F = [&]() {
        rhi::TextureDesc d;
        d.format = rhi::Format::RGBA16_FLOAT;
        d.width  = m_Width; d.height = m_Height;
        d.usage  = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::ShaderResource;
        return device->CreateTexture(d);
    };
    m_A = createRGBA16F();  // Albedo.rgb + Metallic.a
    m_B = createRGBA16F();  // Normal.xyz + Roughness.a
    m_C = createRGBA16F();  // Emissive.rgb + AO.a
    m_E = createRGBA16F();  // WorldPos.xyz
    m_F = createRGBA16F();  // DisneyA（anisotropic/subsurface/specular/sheen）
    m_G = createRGBA16F();  // DisneyB（clearcoat/clearcoatGloss/specularTint.rg）

    // GBuffer D: velocity（RG16_FLOAT，屏幕空间运动矢量）
    {
        rhi::TextureDesc d;
        d.format = rhi::Format::RG16_FLOAT;
        d.width  = m_Width; d.height = m_Height;
        d.usage  = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::ShaderResource;
        m_D = device->CreateTexture(d);
    }

    // GBuffer Depth: D32_FLOAT（DepthStencil + ShaderResource）
    {
        rhi::TextureDesc d;
        d.format = rhi::Format::D32_FLOAT;
        d.width  = m_Width; d.height = m_Height;
        d.usage  = rhi::TextureUsage::DepthStencil | rhi::TextureUsage::ShaderResource;
        m_Depth = device->CreateTexture(d);
    }
}

void GBufferRenderer::CreatePSO(rhi::IRHIDevice* device) {
    // 描述符集布局：set=0 = per-frame GPUObjectData[] + bindless 纹理/采样器数组
    rhi::DescriptorSetLayoutDesc gbLayout;
    gbLayout.bindings = {
        {2, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskVertex | rhi::kStageMaskFragment},
        {5, rhi::DescriptorType::SampledImage, 4096, rhi::kStageMaskFragment, true},   // bindless 纹理
        {6, rhi::DescriptorType::Sampler, 4096, rhi::kStageMaskFragment, true},         // bindless 采样器
    };
    m_Layout = device->CreateDescriptorSetLayout(gbLayout);

    // 着色器
    rhi::ShaderBytecode gbVS, gbFS;
    gbVS.stage = rhi::ShaderStage::Vertex; gbVS.spirv = k_GBuffer_vert_spv; gbVS.entryPoint = "main";
    gbFS.stage = rhi::ShaderStage::Pixel;  gbFS.spirv = k_GBuffer_frag_spv;  gbFS.entryPoint = "main";

    // 顶点布局
    rhi::VertexInputLayout vl;
    vl.stride = sizeof(he::StaticVertex);
    vl.attributes = {
        {0, 0, rhi::VertexFormat::Float3, offsetof(he::StaticVertex, position)},
        {1, 0, rhi::VertexFormat::Float3, offsetof(he::StaticVertex, normal)},
        {2, 0, rhi::VertexFormat::Float2, offsetof(he::StaticVertex, uv)},
    };

    // Push Constant
    rhi::PushConstantRange pc;
    pc.stageMask = rhi::kStageMaskVertex | rhi::kStageMaskFragment;
    pc.size = rhi::kMaxPushConstantSize;

    // PSO 描述
    rhi::PipelineStateDesc gbDesc;
    gbDesc.vertexShader = &gbVS; gbDesc.pixelShader = &gbFS;
    gbDesc.vertexLayout = vl;
    gbDesc.depthTest  = true; gbDesc.depthWrite = true;
    gbDesc.depthFormat = rhi::Format::D32_FLOAT;
    gbDesc.colorAttachmentCount = kGBufferAttachmentCount;
    gbDesc.colorFormats[0] = rhi::Format::RGBA16_FLOAT;  // Albedo+Metallic
    gbDesc.colorFormats[1] = rhi::Format::RGBA16_FLOAT;  // Normal+Roughness
    gbDesc.colorFormats[2] = rhi::Format::RGBA16_FLOAT;  // Emissive+AO
    gbDesc.colorFormats[3] = rhi::Format::RG16_FLOAT;    // Velocity
    gbDesc.colorFormats[4] = rhi::Format::RGBA16_FLOAT;  // WorldPos
    gbDesc.colorFormats[5] = rhi::Format::RGBA16_FLOAT;  // DisneyA（anisotropic/subsurface/specular/sheen）
    gbDesc.colorFormats[6] = rhi::Format::RGBA16_FLOAT;  // DisneyB（clearcoat/clearcoatGloss/specularTint.rg）
    gbDesc.pushConstantRanges = {pc};
    gbDesc.descriptorSetLayouts = {m_Layout};
    gbDesc.debugName = "GBuffer";

    m_PSO = device->CreatePipelineState(gbDesc);
    HE_ASSERT(m_PSO, "GBufferRenderer: PSO 创建失败");
}

void GBufferRenderer::CreateDescriptorSet(rhi::IRHIDevice* device) {
    m_Set = device->AllocateDescriptorSet(m_Layout);

    // 创建 bindless 占位纹理和采样器（bindless 数组回退用）
    u8 white[4] = {255, 255, 255, 255};
    rhi::TextureDesc defDesc;
    defDesc.format = rhi::Format::RGBA8_UNORM;
    defDesc.width = 1; defDesc.height = 1; defDesc.mipLevels = 1; defDesc.arrayLayers = 1;
    defDesc.usage = rhi::TextureUsage::ShaderResource;
    defDesc.initialData = white;
    auto placeholderTex = device->CreateTexture(defDesc);

    rhi::SamplerDesc sd;
    sd.minFilter = sd.magFilter = rhi::FilterMode::Linear;
    sd.addressU = sd.addressV = rhi::AddressMode::Repeat;
    auto placeholderSamp = device->CreateSampler(sd);

    // 设置 BindlessTextureManager 默认纹理
    he::asset::BindlessTextureManager::Instance().SetDefaultTexture(
        placeholderTex.get(), placeholderSamp.get());

    // 预填充 bindless 绑定
    rhi::IRHITexture* texPtrs[]  = { placeholderTex.get() };
    rhi::IRHISampler*  sampPtrs[] = { placeholderSamp.get() };
    device->UpdateDescriptorSet(m_Set, rhi::kBindingBindlessTextures,
        rhi::DescriptorType::SampledImage, texPtrs, nullptr, 1);
    device->UpdateDescriptorSet(m_Set, rhi::kBindingBindlessSamplers,
        rhi::DescriptorType::Sampler, nullptr, sampPtrs, 1);

    // 注册到 BindlessTextureManager（纹理加载后自动推送）
    he::asset::BindlessTextureManager::Instance().RegisterDescriptorSet(
        device, m_Set, 5, 6);
}

} // namespace he::render
