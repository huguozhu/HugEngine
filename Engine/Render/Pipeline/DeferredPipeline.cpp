#include "Pipeline/DeferredPipeline.h"
#include "GI/GI_IBL.h"
#include "GI/GI_RSM.h"
#include "Shadow/ShadowSystem.h"
#include "PostProcess/ToneMapPass.h"
#include "PostProcess/SkyboxPass.h"
#include "SceneRenderer.h"
#include "AntiAliasing/AA_TAA.h"
#include "AntiAliasing/AA_FXAA.h"
#include "Pipeline/GBufferRenderer_CPU.h"
#include "Pipeline/GBufferRenderer_GPU.h"
#include "Asset/BindlessTextureManager.h"
#include "Scene/CubeComponent.h"
#include "Scene/SphereComponent.h"
#include "Scene/LightComponent.h"
#include "Core/Log.h"
#include "Core/Assert.h"
#include <cstring>
#include <cmath>
#include <unordered_set>
#include "GBuffer.vert.spv.h"
#include "GBuffer.frag.spv.h"
#include "DeferredLighting.vert.spv.h"
#include "DeferredLighting.frag.spv.h"

namespace he::render {

bool DeferredPipeline::Initialize(rhi::IRHIDevice* device) {
    m_Device = device;
    HE_ASSERT(m_Device, "DeferredPipeline: null device");

    // GBuffer 纹理（3×RGBA16_FLOAT + D32）
    auto createGBuffer = [&](const char* name) {
        rhi::TextureDesc d;
        d.format = rhi::Format::RGBA16_FLOAT;
        d.width = m_Width; d.height = m_Height;
        d.usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::ShaderResource;
        return device->CreateTexture(d);
    };
    m_GBufferA = createGBuffer("GBufferA");
    m_GBufferB = createGBuffer("GBufferB");
    m_GBufferC = createGBuffer("GBufferC");
    {
        rhi::TextureDesc d;
        d.format = rhi::Format::D32_FLOAT;
        d.width = m_Width; d.height = m_Height;
        d.usage = rhi::TextureUsage::DepthStencil | rhi::TextureUsage::ShaderResource;  // 深度采样需要 SAMPLED
        m_GBufferDepth = device->CreateTexture(d);
    }

    // GBuffer D: velocity (RG16_FLOAT)
    {
        rhi::TextureDesc d;
        d.format = rhi::Format::RG16_FLOAT;
        d.width  = m_Width; d.height = m_Height;
        d.usage  = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::ShaderResource;
        m_GBufferD = device->CreateTexture(d);
    }

    // HDR target
    {
        rhi::TextureDesc d;
        d.format = rhi::Format::RGBA16_FLOAT;
        d.width = m_Width; d.height = m_Height;
        d.usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::ShaderResource;
        m_HDRTarget = device->CreateTexture(d);
        rhi::TextureDesc dd;
        dd.format = rhi::Format::D32_FLOAT;
        dd.width = m_Width; dd.height = m_Height;
        dd.usage = rhi::TextureUsage::DepthStencil;
        m_HDRDepth = device->CreateTexture(dd);
        rhi::SamplerDesc s; s.minFilter = s.magFilter = rhi::FilterMode::Linear;
        s.addressU = s.addressV = rhi::AddressMode::ClampToEdge;
        m_HDRSampler = device->CreateSampler(s);
    }

    // LDR 中间纹理（FXAA 链路：ToneMap → LDR → FXAA → BackBuffer）
    {
        rhi::TextureDesc d;
        d.format = rhi::Format::BGRA8_UNORM;
        d.width = m_Width; d.height = m_Height;
        d.usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::ShaderResource;
        m_LDRTarget = device->CreateTexture(d);
        rhi::SamplerDesc s; s.minFilter = s.magFilter = rhi::FilterMode::Linear;
        s.addressU = s.addressV = rhi::AddressMode::ClampToEdge;
        m_LDRSampler = device->CreateSampler(s);
        // 虚拟深度附件（ToneMap PSO 带 D32_FLOAT depthFormat，Offscreen 需要 2 附件）
        rhi::TextureDesc dd; dd.format = rhi::Format::D32_FLOAT;
        dd.width = 1; dd.height = 1; dd.usage = rhi::TextureUsage::DepthStencil;
        m_LDRDummyDepth = device->CreateTexture(dd);
    }

    // 三缓冲
    for (u32 i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        m_LightBuffers[i]      = device->CreateBuffer({sizeof(GPULight) * MAX_LIGHTS, rhi::BufferUsage::Storage});
        m_ObjectBuffers[i]     = device->CreateBuffer({sizeof(GPUObjectData) * MAX_OBJECTS, rhi::BufferUsage::Storage});
        m_ShadowBuffers[i]     = device->CreateBuffer({sizeof(GPUShadowData) * MAX_SHADOWS, rhi::BufferUsage::Storage});
        m_ShadowObjBuffers[i]  = device->CreateBuffer({sizeof(GPUObjectData) * MAX_OBJECTS, rhi::BufferUsage::Storage});
    }

    // 子系统（IBL/RSM 可选，初始化失败不影响核心渲染）
    m_ShadowSystem = std::make_unique<ShadowSystem>();
    m_ShadowSystem->Initialize(device, 0, 0);
    try {
        auto gi = std::make_unique<GI_IBL>();
        gi->Initialize(device, 0, 0);
        m_GI = std::move(gi);
        HE_CORE_INFO("DeferredPipeline: GI_IBL ready");
    } catch (...) {
        HE_CORE_WARN("DeferredPipeline: GI_IBL init failed, IBL disabled");
        m_GI.reset();
    }
    try { m_RSM = std::make_unique<GI_RSM>(); m_RSM->Initialize(device, 0, 0); } catch (...) { m_RSM.reset(); }
    m_ToneMap = std::make_unique<ToneMapPass>(); m_ToneMap->Initialize(device, m_Width, m_Height);
    m_Skybox  = std::make_unique<SkyboxPass>(); m_Skybox->Initialize(device, m_Width, m_Height);
    m_SceneRenderer = std::make_unique<SceneRenderer>();
    m_GPUCulling.Initialize(device);
    m_GPUScene.Initialize(device);
    m_SSGI.Initialize(device, m_Width, m_Height);
    m_SSR.Initialize(device, m_Width, m_Height);
    m_DDGI.Initialize(device, m_Width, m_Height);
    m_DenoiseSSGI.Initialize(device, m_Width, m_Height);
    m_DenoiseSSR.Initialize(device, m_Width, m_Height);
    m_SSAO.Initialize(device, m_Width, m_Height);
    m_SSAO.enabled = false;  // 默认关闭
    // Bloom / FXAA / GaussianBlur：懒初始化，首次 SetEnabled(true) 时才分配 GPU 资源

    // AA_TAA（HDR 空间）
    m_AntiAliasing = std::make_unique<AA_TAA>();
    if (!m_AntiAliasing->Initialize(device, m_Width, m_Height)) {
        HE_CORE_WARN("DeferredPipeline: AA_TAA init failed, anti-aliasing disabled");
        m_AntiAliasing.reset();
    }

    // AA_FXAA（LDR 空间，懒初始化：EnableFXAA(true) 首次调用时分配 GPU 资源）


    // GBuffer PSO (4 MRT + D32, set=0 合并 per-frame + bindless 纹理/采样器数组)
    // set=0: per-frame GPUObjectData[] + bindless Texture2D[] + SamplerState[]
    rhi::DescriptorSetLayoutDesc gbLayout;
    gbLayout.bindings = {
        {2, rhi::DescriptorType::StorageBuffer, 1, 17},   // u_Objects (Vertex|Fragment)
        {5, rhi::DescriptorType::SampledImage, 4096, 16, true},  // u_Textures[] bindless
        {6, rhi::DescriptorType::Sampler, 4096, 16, true},               // u_Samplers[] bindless
    };
    m_GBufferLayout = device->CreateDescriptorSetLayout(gbLayout);

    rhi::ShaderBytecode gbVS, gbFS;
    gbVS.stage = rhi::ShaderStage::Vertex; gbVS.spirv = k_GBuffer_vert_spv; gbVS.entryPoint = "main";
    gbFS.stage = rhi::ShaderStage::Pixel;  gbFS.spirv = k_GBuffer_frag_spv;  gbFS.entryPoint = "main";
    rhi::VertexInputLayout vl;
    vl.stride = sizeof(he::StaticVertex);
    vl.attributes = {
        {0,0,rhi::VertexFormat::Float3, offsetof(he::StaticVertex, position)},
        {1,0,rhi::VertexFormat::Float3, offsetof(he::StaticVertex, normal)},
        {2,0,rhi::VertexFormat::Float2, offsetof(he::StaticVertex, uv)},
    };
    rhi::PushConstantRange pc; pc.stageMask = 1|16; pc.size = 256; // 192B 实际用量
    rhi::PipelineStateDesc gbDesc;
    gbDesc.vertexShader = &gbVS; gbDesc.pixelShader = &gbFS;
    gbDesc.vertexLayout = vl;
    gbDesc.depthTest = true; gbDesc.depthWrite = true;
    gbDesc.depthFormat = rhi::Format::D32_FLOAT;
    gbDesc.colorAttachmentCount = 4;
    gbDesc.colorFormats[0] = gbDesc.colorFormats[1] = gbDesc.colorFormats[2] = rhi::Format::RGBA16_FLOAT;
    gbDesc.colorFormats[3] = rhi::Format::RG16_FLOAT;  // velocity
    gbDesc.pushConstantRanges = {pc};
    gbDesc.descriptorSetLayouts = {m_GBufferLayout};  // 仅 set=0，无 per-mesh
    gbDesc.debugName = "GBuffer";
    m_GBufferPSO = device->CreatePipelineState(gbDesc);
    HE_ASSERT(m_GBufferPSO, "DeferredPipeline: GBuffer PSO failed");

    // GBuffer set=0 共享描述符集（含 binding 2: Object SSBO + bindless 5/6）
    m_GBufferSet = device->AllocateDescriptorSet(m_GBufferLayout);
    // 创建 bindless 占位纹理和采样器（bindless 数组回退用）
    {
        u8 w4[4]={255,255,255,255};
        rhi::TextureDesc defDesc; defDesc.format=rhi::Format::RGBA8_UNORM;
        defDesc.width=1; defDesc.height=1; defDesc.mipLevels=1; defDesc.arrayLayers=1;
        defDesc.usage=rhi::TextureUsage::ShaderResource; defDesc.initialData=w4;
        m_BindlessPlaceholder = device->CreateTexture(defDesc);
        rhi::SamplerDesc sd; sd.minFilter=sd.magFilter=rhi::FilterMode::Linear;
        sd.addressU=sd.addressV=rhi::AddressMode::Repeat;
        m_BindlessSampler = device->CreateSampler(sd);
        // 设置 BindlessTextureManager 的默认纹理（null 回退用）
        he::asset::BindlessTextureManager::Instance().SetDefaultTexture(
            m_BindlessPlaceholder.get(), m_BindlessSampler.get());
        // 预填充绑定 5 和 6 至少一个有效的占位符
        rhi::IRHITexture* texPtrs[] = { m_BindlessPlaceholder.get() };
        rhi::IRHISampler* sampPtrs[] = { m_BindlessSampler.get() };
        device->UpdateDescriptorSet(m_GBufferSet, 5, rhi::DescriptorType::SampledImage,
            texPtrs, nullptr, 1);
        device->UpdateDescriptorSet(m_GBufferSet, 6, rhi::DescriptorType::Sampler,
            nullptr, sampPtrs, 1);
        // 注册 GBufferSet 到 BindlessTextureManager（纹理加载后 FlushPending 自动推送）
        he::asset::BindlessTextureManager::Instance().RegisterDescriptorSet(
            device, m_GBufferSet, 5, 6);
    }

    // ── GBuffer 渲染器（PSO + DescriptorSet 就绪后创建）──
    m_GBufferCtx.device       = device;
    m_GBufferCtx.width        = m_Width;
    m_GBufferCtx.height       = m_Height;
    m_GBufferCtx.gbA          = m_GBufferA.get();
    m_GBufferCtx.gbB          = m_GBufferB.get();
    m_GBufferCtx.gbC          = m_GBufferC.get();
    m_GBufferCtx.gbVel        = m_GBufferD.get();
    m_GBufferCtx.gbDepth      = m_GBufferDepth.get();
    m_GBufferCtx.pso          = m_GBufferPSO.get();
    m_GBufferCtx.descSet      = m_GBufferSet;
    m_GBufferCtx.sceneRenderer = m_SceneRenderer.get();
    m_GBufferCtx.gpuCulling    = &m_GPUCulling;
    m_GBufferCtx.gpuScene      = &m_GPUScene;
    m_GBufferCtx.gpuVisibleIndices = &m_GPUVisibleIndices;
    m_GBufferCtx.meshBatcher        = &m_MeshBatcher;
    if (m_GBufferMode == GBufferMode::GPU)
        m_GBufferRenderer = std::make_unique<GBufferRenderer_GPU>();
    else
        m_GBufferRenderer = std::make_unique<GBufferRenderer_CPU>();
    m_GBufferRenderer->Initialize(m_GBufferCtx);

    // GPU Profiler
    m_Profiler.Initialize(device, 20, MAX_FRAMES_IN_FLIGHT);

    // Lighting PSO (全屏三角形，无深度)
    rhi::ShaderBytecode lVS, lFS;
    lVS.stage = rhi::ShaderStage::Vertex; lVS.spirv = k_DeferredLighting_vert_spv; lVS.entryPoint = "main";
    lFS.stage = rhi::ShaderStage::Pixel;  lFS.spirv = k_DeferredLighting_frag_spv; lFS.entryPoint = "main";
    rhi::DescriptorSetLayoutDesc ll;
    ll.bindings = {
        {0, rhi::DescriptorType::CombinedImageSampler, 1, 16},  // GBufferA
        {1, rhi::DescriptorType::CombinedImageSampler, 1, 16},  // GBufferB
        {2, rhi::DescriptorType::CombinedImageSampler, 1, 16},  // GBufferC
        {3, rhi::DescriptorType::CombinedImageSampler, 1, 16},  // Depth
        {4, rhi::DescriptorType::CombinedImageSampler, 1, 16},  // Shadow0 (CSM0)
        {7, rhi::DescriptorType::StorageBuffer, 1, 16},  // LightGrid (Clustered)
        {8, rhi::DescriptorType::StorageBuffer, 1, 16},  // LightIndexList (Clustered)
        {9, rhi::DescriptorType::CombinedImageSampler, 1, 16},  // SpotShadow
        {10, rhi::DescriptorType::CombinedImageSampler, 1, 16}, // Shadow1 (CSM1)
        {11, rhi::DescriptorType::CombinedImageSampler, 1, 16}, // Shadow2 (CSM2)
        {12, rhi::DescriptorType::CombinedImageSampler, 1, 16}, // Irradiance
        {13, rhi::DescriptorType::CombinedImageSampler, 1, 16}, // Prefilter
        {14, rhi::DescriptorType::CombinedImageSampler, 1, 16}, // BRDF LUT
        {15, rhi::DescriptorType::CombinedImageSampler, 1, 16}, // RSM Pos
        {16, rhi::DescriptorType::CombinedImageSampler, 1, 16}, // RSM Flux
        {17, rhi::DescriptorType::StorageBuffer, 1, 16},  // Lights
        {18, rhi::DescriptorType::StorageBuffer, 1, 16},  // ShadowData
        {19, rhi::DescriptorType::CombinedImageSampler, 1, 16},  // SSGI
        {20, rhi::DescriptorType::CombinedImageSampler, 1, 16},  // SSAO
        {21, rhi::DescriptorType::CombinedImageSampler, 1, 16},  // SSR
        {22, rhi::DescriptorType::StorageBuffer,         1, 16},  // DDGI Probes
    };
    m_LightingLayout = device->CreateDescriptorSetLayout(ll);
    m_LightingSet    = device->AllocateDescriptorSet(m_LightingLayout);
    // 预填充所有 binding 占位纹理（避免未绑定→Intel GPU 白屏）
    {
        u8 w4[4]={255,255,255,255};
        rhi::TextureDesc ptd; ptd.format=rhi::Format::RGBA8_UNORM; ptd.width=1; ptd.height=1; ptd.mipLevels=1; ptd.arrayLayers=1; ptd.usage=rhi::TextureUsage::ShaderResource; ptd.initialData=w4;
        auto pt = device->CreateTexture(ptd);
        rhi::SamplerDesc sd; sd.minFilter=sd.magFilter=rhi::FilterMode::Linear;
        sd.addressU=sd.addressV=rhi::AddressMode::ClampToEdge;
        auto ps = device->CreateSampler(sd);
        // 只更新 layout 中声明的 binding（0-4, 9-16）
        for (u32 b : {0u,1u,2u,3u,4u,9u,10u,11u,12u,13u,14u,15u,16u})
            device->UpdateDescriptorSet(m_LightingSet, b, rhi::DescriptorType::CombinedImageSampler, pt.get(), ps.get());
        // Cluster SSBO 占位（binding 7/8）
        rhi::BufferDesc gd; gd.size=16; gd.usage=rhi::BufferUsage::Storage;
        auto gb = device->CreateBuffer(gd);
        device->UpdateDescriptorSet(m_LightingSet, 7, rhi::DescriptorType::StorageBuffer, gb.get());
        device->UpdateDescriptorSet(m_LightingSet, 8, rhi::DescriptorType::StorageBuffer, gb.get());
        // DDGI 探针 SSBO 占位（binding 22）
        device->UpdateDescriptorSet(m_LightingSet, 22, rhi::DescriptorType::StorageBuffer, gb.get());
    }

    rhi::PushConstantRange lpc; lpc.stageMask = 1|16; lpc.size = 128;  // 含 cluster 参数
    rhi::PipelineStateDesc lDesc;
    lDesc.vertexShader = &lVS; lDesc.pixelShader = &lFS;
    lDesc.topology = rhi::PrimitiveTopology::TriangleList;
    lDesc.depthTest = false; lDesc.depthWrite = false;
    lDesc.colorAttachmentCount = 1;
    lDesc.colorFormats[0] = rhi::Format::RGBA16_FLOAT;
    lDesc.pushConstantRanges = {lpc};
    lDesc.descriptorSetLayouts = {m_LightingLayout};
    lDesc.debugName = "DeferredLighting";
    m_LightingPSO = device->CreatePipelineState(lDesc);
    HE_ASSERT(m_LightingPSO, "DeferredPipeline: Lighting PSO failed");

    m_Ready = true;
    HE_CORE_INFO("DeferredPipeline initialized");
    return true;
}

void DeferredPipeline::Shutdown() {
    if (m_ShadowSystem) m_ShadowSystem->Shutdown();
    if (m_ToneMap) m_ToneMap->Shutdown();
    if (m_Skybox)  m_Skybox->Shutdown();
    if (m_GBufferLayout != rhi::kInvalidLayout) { m_Device->DestroyDescriptorSetLayout(m_GBufferLayout); }
    if (m_LightingLayout != rhi::kInvalidLayout) { m_Device->DestroyDescriptorSetLayout(m_LightingLayout); }
    m_GBufferA.reset(); m_GBufferB.reset(); m_GBufferC.reset(); m_GBufferDepth.reset();
    m_GBufferD.reset();
    if (m_AntiAliasing) m_AntiAliasing->Shutdown();
    m_AntiAliasing.reset();
    if (m_FXAA) m_FXAA->Shutdown();
    m_FXAA.reset();
    m_LDRTarget.reset(); m_LDRSampler.reset(); m_LDRDummyDepth.reset();
    m_HDRTarget.reset(); m_HDRDepth.reset(); m_HDRSampler.reset();
    m_GBufferPSO.reset(); m_LightingPSO.reset();
    for (auto& b : m_LightBuffers) b.reset();
    for (auto& b : m_ObjectBuffers) b.reset();
    for (auto& b : m_ShadowBuffers) b.reset();
    for (auto& b : m_ShadowObjBuffers) b.reset();
    m_GPUCulling.Shutdown(m_Device);
    m_GPUScene.Shutdown();
    if (m_GBufferRenderer) m_GBufferRenderer->Shutdown();
    m_GBufferRenderer.reset();
    m_Profiler.Shutdown();
    m_SSGI.Shutdown();
    m_SSR.Shutdown();
    m_DDGI.Shutdown();
    m_DenoiseSSGI.Shutdown();
    m_DenoiseSSR.Shutdown();
    m_SSAO.Shutdown();
    m_Bloom.Shutdown();
    m_Device = nullptr; m_Ready = false;
    HE_CORE_INFO("DeferredPipeline shutdown");
}

// CreateTextureDescriptorSet 已移除 — 使用全局 bindless 纹理数组替代

void DeferredPipeline::SetGBufferMode(GBufferMode mode) {
    if (mode == m_GBufferMode && m_GBufferRenderer) return;
    m_GBufferMode = mode;
    if (m_GBufferRenderer) m_GBufferRenderer->Shutdown();
    if (mode == GBufferMode::GPU)
        m_GBufferRenderer = std::make_unique<GBufferRenderer_GPU>();
    else
        m_GBufferRenderer = std::make_unique<GBufferRenderer_CPU>();
    m_GBufferRenderer->Initialize(m_GBufferCtx);
    HE_CORE_INFO("DeferredPipeline: GBuffer 模式切换到 {}", mode == GBufferMode::GPU ? "GPU" : "CPU");
}

void DeferredPipeline::EnableFXAA(bool enable) {
    m_FXAAEnabled = enable;
    if (!enable || !m_Device) return;
    // 懒初始化：首次 EnableFXAA(true) 时创建 PSO 和纹理
    if (!m_FXAA) {
        m_FXAA = std::make_unique<AA_FXAA>();
        if (!m_FXAA->Initialize(m_Device, m_Width, m_Height)) {
            HE_CORE_WARN("DeferredPipeline: AA_FXAA init failed");
            m_FXAA.reset();
        }
    }
}

void DeferredPipeline::NextFrame() {
    m_CurrentFrameSlot = (m_CurrentFrameSlot + 1) % MAX_FRAMES_IN_FLIGHT;
}

void DeferredPipeline::OnResize(u32 w, u32 h) {
    if (w == m_Width && h == m_Height) return;
    m_Width = w; m_Height = h;
    // 重建 GBuffer + HDR (简化：直接 CreateTexture 重建)
    auto r = [&](auto& t, rhi::Format f, rhi::TextureUsage u) {
        rhi::TextureDesc d; d.format = f; d.width = w; d.height = h; d.usage = u;
        t = m_Device->CreateTexture(d);
    };
    r(m_GBufferA, rhi::Format::RGBA16_FLOAT, rhi::TextureUsage::RenderTarget | rhi::TextureUsage::ShaderResource);
    r(m_GBufferB, rhi::Format::RGBA16_FLOAT, rhi::TextureUsage::RenderTarget | rhi::TextureUsage::ShaderResource);
    r(m_GBufferC, rhi::Format::RGBA16_FLOAT, rhi::TextureUsage::RenderTarget | rhi::TextureUsage::ShaderResource);
    r(m_GBufferD, rhi::Format::RG16_FLOAT, rhi::TextureUsage::RenderTarget | rhi::TextureUsage::ShaderResource);
    r(m_GBufferDepth, rhi::Format::D32_FLOAT, rhi::TextureUsage::DepthStencil | rhi::TextureUsage::ShaderResource);
    r(m_HDRTarget, rhi::Format::RGBA16_FLOAT, rhi::TextureUsage::RenderTarget | rhi::TextureUsage::ShaderResource);
    r(m_HDRDepth, rhi::Format::D32_FLOAT, rhi::TextureUsage::DepthStencil);
    if (m_ToneMap) m_ToneMap->OnResize(w, h);
    if (m_Skybox)  { m_Skybox->Shutdown(); m_Skybox->Initialize(m_Device, w, h); }
    if (m_AntiAliasing) m_AntiAliasing->OnResize(w, h);
    if (m_FXAA) m_FXAA->OnResize(w, h);
    m_GBufferCtx.width  = w;
    m_GBufferCtx.height = h;
    m_GBufferCtx.gbA    = m_GBufferA.get();
    m_GBufferCtx.gbB    = m_GBufferB.get();
    m_GBufferCtx.gbC    = m_GBufferC.get();
    m_GBufferCtx.gbVel  = m_GBufferD.get();
    m_GBufferCtx.gbDepth = m_GBufferDepth.get();
    // 重建 LDR 中间纹理
    {
        rhi::TextureDesc d; d.format = rhi::Format::BGRA8_UNORM;
        d.width = w; d.height = h; d.usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::ShaderResource;
        m_LDRTarget = m_Device->CreateTexture(d);
    }
    // 重建 LDR 虚拟深度
    {
        rhi::TextureDesc dd; dd.format = rhi::Format::D32_FLOAT;
        dd.width = 1; dd.height = 1; dd.usage = rhi::TextureUsage::DepthStencil;
        m_LDRDummyDepth = m_Device->CreateTexture(dd);
    }
    m_SSAO.OnResize(w, h);
    m_SSGI.OnResize(w, h);
    m_SSR.OnResize(w, h);
    m_DDGI.OnResize(w, h);
    m_DenoiseSSGI.OnResize(w, h);
    m_DenoiseSSR.OnResize(w, h);
    m_Bloom.OnResize(w, h);  // 内部守卫：未初始化时直接 return
}

void DeferredPipeline::Render(rhi::IRHICommandList* cmd, he::World& world,
                               he::SceneGraph& sg, const CameraData& camera) {
    // 延迟创建 Compute 命令列表（AsyncCompute 模式）
    bool useAsyncCompute = m_Device->HasAsyncComputeQueue();
    if (useAsyncCompute && !m_ComputeCmdList) {
        m_ComputeCmdList = m_Device->CreateCommandList(rhi::QueueType::Compute);
        HE_CORE_INFO("DeferredPipeline: AsyncCompute enabled — dedicated Compute command list created");
    }

    RenderGraph rg;
    rg.SetProfiler(&m_Profiler);
    rg.SetAsyncComputeEnabled(useAsyncCompute);
    BuildFrameGraph(rg, world, sg, camera);
    rg.Compile();

    if (useAsyncCompute && m_ComputeCmdList) {
        // 双队列模式: Graphics + Compute 并行执行
        m_ComputeCmdList->Begin();
        rg.Execute(cmd, m_ComputeCmdList.get(), m_Device);
        m_ComputeCmdList->End();
        m_ComputeCmdList->Submit();
    } else {
        // 回退: 单 Graphics 队列（与原来行为一致）
        rg.Execute(cmd, m_Device);
    }
}

void DeferredPipeline::BuildFrameGraph(RenderGraph& rg, he::World& world,
                                        he::SceneGraph& sg, const CameraData& camera) {
    if (m_SwapChain) rg.SetSwapChain(m_SwapChain);
    u32 w = m_Width, h = m_Height;
    auto gbA = rg.ImportTexture("GB_A", m_GBufferA.get());
    auto gbB = rg.ImportTexture("GB_B", m_GBufferB.get());
    auto gbC = rg.ImportTexture("GB_C", m_GBufferC.get());
    auto gbDepth = rg.ImportTexture("GB_Depth", m_GBufferDepth.get());
    auto gbVel = rg.ImportTexture("GB_Vel", m_GBufferD.get());
    auto hdrC = rg.ImportTexture("HDR_C", m_HDRTarget.get());
    auto backBuf = rg.ImportBackBuffer();

    (void)world; (void)sg;

    // ── 帧首：更新成员变量（lambda 内通过 this 安全访问，无悬垂引用风险）──
    m_CurrViewProj = camera.GetViewProjMatrix();
    static bool firstFrame = true;
    if (firstFrame) { m_PrevViewProj = m_CurrViewProj; firstFrame = false; }
    if (m_AntiAliasing) m_AntiAliasing->OnBeginFrame();

    // GPUScene 收集 → [GPU 模式: 填充 IndirectDraw 参数] → 上传
    m_GPUScene.Collect(world, sg);
    if (m_GBufferMode == GBufferMode::GPU) {
        if (!m_BatchBuilt) { m_MeshBatcher.Build(world); m_BatchBuilt = true; }
        m_MeshBatcher.FillGPUScene(m_GPUScene);  // 在 Upload 前写入 draw 参数
    }
    m_GPUScene.Upload(m_Device);

    // GPU 剔除读回（上帧结果）+ 过滤可见物体
    bool useGPUVisible = false;
    if (m_GPUCulling.enabled) {
        m_GPUCulling.Readback(m_Device, m_GPUVisibleIndices);
        useGPUVisible = !m_GPUVisibleIndices.empty();
    }

    // ── GPU 剔除 Compute Pass（读上帧 GBuffer 深度 → 调度下帧剔除）──
    // 必须在 GBuffer 之前：此时 gbDepth 保留上帧数据且未作为渲染目标
    rg.AddPass("GPU_Cull",
        {{gbDepth, ResourceAccess::Read}},  // 读上一帧深度做 Hi-Z 遮挡剔除
        {},
        [&, w, h](rhi::IRHICommandList* c) {
            if (!m_GPUCulling.enabled) return;
            m_GPUCulling.SetSceneBuffer(m_Device, m_GPUScene.GetObjectBuffer());
            if (m_GBufferDepth) m_GPUCulling.SetDepthTexture(m_Device, m_GBufferDepth.get(), w, h);
            m_GPUCulling.Dispatch(c, camera.GetViewProjMatrix(), m_GPUScene.GetObjectCount(), w, h);
            // 恢复 graphics pipeline state（compute dispatch 后状态未定义，影响后续 GBuffer pass）
            c->SetPipeline(m_GBufferPSO.get());
        },
        RGPassQueue::Compute);  // AsyncCompute: GPU 剔除在 Compute 队列执行

    // GBuffer 4×MRT + 绘制（委托给 IGBufferRenderer，支持 CPU/GPU 双模式）
    rg.AddPass("GB_Clear", {}, {{gbA, ResourceAccess::Write}, {gbB, ResourceAccess::Write},
        {gbC, ResourceAccess::Write}, {gbVel, ResourceAccess::Write}, {gbDepth, ResourceAccess::Write}},
        [&](rhi::IRHICommandList* c) {
            // 更新运行时 context
            m_GBufferCtx.objectBuffer = m_ObjectBuffers[m_CurrentFrameSlot].get();
            m_GBufferCtx.prevViewProj = m_PrevViewProj;
            m_GBufferRenderer->Render(c, m_GBufferCtx, world, sg, camera);
        });

    // ============================================================
    // DDGI Probe Update（Compute Shader：必须放在所有 offscreen pass 之前，
    // 避免 compute pipeline 切换影响后续 render pass 状态）
    // ============================================================
    rg.AddPass("DDGI_Update",
        {{gbA, ResourceAccess::Read}, {gbB, ResourceAccess::Read}, {gbDepth, ResourceAccess::Read}},
        {},
        [&](rhi::IRHICommandList* c) {
            if (m_DDGI.IsEnabled()) {
                m_DDGI.SetGBufferInputs(m_GBufferDepth.get(), m_GBufferB.get(), m_GBufferA.get());
                SubsystemContext dgiCtx;
                dgiCtx.camera = &camera;
                m_DDGI.Update(dgiCtx);
                m_DDGI.Render(c);
                // Compute dispatch 后恢复 graphics pipeline，
                // 确保后续 pass 的 SetPipeline / BeginOffscreenPass 状态正确
                c->SetPipeline(m_LightingPSO.get());
            }
        },
        RGPassQueue::Compute);  // AsyncCompute: DDGI 探针更新在 Compute 队列执行

    // SSAO Pass
    auto ssaoOut = rg.ImportTexture("SSAO_Output", m_SSAO.GetAOTexture());
    rg.AddPass("SSAO", {}, {{ssaoOut, ResourceAccess::Write}},
        [&, w, h](rhi::IRHICommandList* c) {
            m_SSAO.PreBind(c);
            if (m_SSAO.enabled) {
                m_SSAO.SetInputs(m_GBufferDepth.get(), m_GBufferB.get());
                c->BeginOffscreenPass(m_SSAO.GetAOTexture()->GetNativeHandle(), nullptr, w, h, nullptr, false);
                m_SSAO.Render(c);
            } else {
                rhi::ClearValue wclr; wclr.color[0]=wclr.color[1]=wclr.color[2]=wclr.color[3]=1;
                c->BeginOffscreenPass(m_SSAO.GetAOTexture()->GetNativeHandle(), nullptr, w, h, &wclr, false);
            }
            c->EndOffscreenPass();
        });

    // SSR Pass（屏幕空间反射）
    auto ssrOut = rg.ImportTexture("SSR_Output", m_SSR.GetIndirectSpecularTexture());
    rg.AddPass("SSR", {}, {{ssrOut, ResourceAccess::Write}},
        [&, w, h](rhi::IRHICommandList* c) {
            m_SSR.PreBind(c);
            rhi::ClearValue clr{};
            if (m_SSR.IsEnabled()) {
                m_SSR.SetInputs(m_GBufferDepth.get(), m_GBufferB.get(), m_GBufferA.get());
                c->BeginOffscreenPass(m_SSR.GetIndirectSpecularTexture()->GetNativeHandle(), nullptr, w, h, &clr, false);
                m_SSR.Render(c);
            } else {
                c->BeginOffscreenPass(m_SSR.GetIndirectSpecularTexture()->GetNativeHandle(), nullptr, w, h, &clr, false);
            }
            c->EndOffscreenPass();
        });

    // SSR Denoise
    auto ssrDenoised = rg.ImportTexture("SSR_Denoised", m_DenoiseSSR.GetOutput());
    rg.AddPass("SSR_Denoise", {{ssrOut, ResourceAccess::Read}}, {{ssrDenoised, ResourceAccess::Write}},
        [&, w, h](rhi::IRHICommandList* c) {
            m_DenoiseSSR.PreBind(c);
            m_DenoiseSSR.SetInputs(m_SSR.GetIndirectSpecularTexture(), m_GBufferDepth.get(), m_GBufferB.get());
            rhi::ClearValue clr{};
            c->BeginOffscreenPass(m_DenoiseSSR.GetOutput()->GetNativeHandle(), nullptr, w, h, &clr, false);
            m_DenoiseSSR.Render(c);
            c->EndOffscreenPass();
        });

    // SSGI Pass（屏幕空间间接漫反射）
    auto ssgiOut = rg.ImportTexture("SSGI_Output", m_SSGI.GetIndirectDiffuseTexture());
    rg.AddPass("SSGI", {}, {{ssgiOut, ResourceAccess::Write}},
        [&, w, h](rhi::IRHICommandList* c) {
            m_SSGI.PreBind(c);
            rhi::ClearValue clr{};
            if (m_SSGI.IsEnabled()) {
                m_SSGI.SetInputs(m_GBufferDepth.get(), m_GBufferB.get(), m_GBufferA.get());
                c->BeginOffscreenPass(m_SSGI.GetIndirectDiffuseTexture()->GetNativeHandle(), nullptr, w, h, &clr, false);
                m_SSGI.Render(c);
            } else {
                c->BeginOffscreenPass(m_SSGI.GetIndirectDiffuseTexture()->GetNativeHandle(), nullptr, w, h, &clr, false);
            }
            c->EndOffscreenPass();
        });

    // SSGI Denoise
    auto ssgiDenoised = rg.ImportTexture("SSGI_Denoised", m_DenoiseSSGI.GetOutput());
    rg.AddPass("SSGI_Denoise", {{ssgiOut, ResourceAccess::Read}}, {{ssgiDenoised, ResourceAccess::Write}},
        [&, w, h](rhi::IRHICommandList* c) {
            m_DenoiseSSGI.PreBind(c);
            m_DenoiseSSGI.SetInputs(m_SSGI.GetIndirectDiffuseTexture(), m_GBufferDepth.get(), m_GBufferB.get());
            rhi::ClearValue clr{};
            c->BeginOffscreenPass(m_DenoiseSSGI.GetOutput()->GetNativeHandle(), nullptr, w, h, &clr, false);
            m_DenoiseSSGI.Render(c);
            c->EndOffscreenPass();
        });

    // Lighting Pass (全屏 PBR + 降噪后 SSGI/SSR/DDGI 读取)
    rg.AddPass("Lighting",
        {{gbA, ResourceAccess::Read}, {gbB, ResourceAccess::Read}, {gbC, ResourceAccess::Read},
         {ssgiDenoised, ResourceAccess::Read},
         {ssrDenoised, ResourceAccess::Read}},
        {{hdrC, ResourceAccess::Write}},
        [&, w, h](rhi::IRHICommandList* c) {
            c->PipelineBarrier(rhi::PipelineStage::LateFragmentTests, rhi::PipelineStage::FragmentShader,
                rhi::ResourceState::DepthStencilWrite, rhi::ResourceState::DepthStencilRead, m_GBufferDepth.get());
            auto bindTex = [&](u32 b, rhi::IRHITexture* t) { if(t) m_Device->UpdateDescriptorSet(m_LightingSet, b, rhi::DescriptorType::CombinedImageSampler, t, m_HDRSampler.get()); };
            bindTex(0, m_GBufferA.get()); bindTex(1, m_GBufferB.get()); bindTex(2, m_GBufferC.get());
            bindTex(3, m_GBufferDepth.get());
            if (m_ShadowSystem && m_ShadowSystem->GetShadowMap(0))
                bindTex(4, m_ShadowSystem->GetShadowMap(0));
            // Spot 阴影贴图（映射索引 4 = CSM(3) + Point(1) + Spot(0)）
            if (m_ShadowSystem && m_ShadowSystem->GetShadowMap(4))
                bindTex(9, m_ShadowSystem->GetShadowMap(4));
            m_Device->UpdateDescriptorSet(m_LightingSet, 17, rhi::DescriptorType::StorageBuffer, m_LightBuffers[m_CurrentFrameSlot].get());
            m_Device->UpdateDescriptorSet(m_LightingSet, 18, rhi::DescriptorType::StorageBuffer, m_ShadowBuffers[m_CurrentFrameSlot].get());
            m_Device->UpdateDescriptorSet(m_LightingSet, 19, rhi::DescriptorType::CombinedImageSampler,
                m_DenoiseSSGI.GetOutput(), m_SSGI.GetOutputSampler());
            m_Device->UpdateDescriptorSet(m_LightingSet, 20, rhi::DescriptorType::CombinedImageSampler,
                m_SSAO.GetAOTexture(), m_SSAO.GetAOSampler());
            m_Device->UpdateDescriptorSet(m_LightingSet, 21, rhi::DescriptorType::CombinedImageSampler,
                m_DenoiseSSR.GetOutput(), m_SSR.GetOutputSampler());

            // DDGI 探针数据（binding 22：StorageBuffer，Lighting 中三线性插值采样）
            m_Device->UpdateDescriptorSet(m_LightingSet, 22, rhi::DescriptorType::StorageBuffer,
                m_DDGI.GetProbeBuffer());

            // Clustered Shading: 构建 cluster AABB + 光源剔除（仅在启用时）
            PushConstantData fpc{}; CollectLights(fpc, world, sg, camera);
            float4x4 ivp = glm::inverse(camera.GetViewProjMatrix());
            u32 useClustered = 0u;
            if (m_ClusteredShading.enabled) {
                m_CachedLights.resize(fpc.lightCount);
                auto* gpuLights = static_cast<const GPULight*>(m_LightBuffers[m_CurrentFrameSlot]->Map());
                if (gpuLights) { memcpy(m_CachedLights.data(), gpuLights, fpc.lightCount * sizeof(GPULight)); }
                m_LightBuffers[m_CurrentFrameSlot]->Unmap();
                m_ClusteredShading.BuildClusters(ivp, w, h, camera.nearPlane, camera.farPlane);
                m_ClusteredShading.CullLights(m_CachedLights.data(), fpc.lightCount);
                // 上传 LightGrid
                auto& grid = m_ClusteredShading.GetLightGrid();
                if (!m_LightGridBuffer || m_LightGridBuffer->GetSize() < grid.size() * 8) {
                    rhi::BufferDesc d; d.size = grid.size() * 8; d.usage = rhi::BufferUsage::Storage; d.cpuAccess = true;
                    m_LightGridBuffer = m_Device->CreateBuffer(d);
                }
                void* m = m_LightGridBuffer->Map();
                if (m) { memcpy(m, grid.data(), grid.size() * 8); m_LightGridBuffer->Unmap(); }
                m_Device->UpdateDescriptorSet(m_LightingSet, 7, rhi::DescriptorType::StorageBuffer, m_LightGridBuffer.get());
                // 上传 LightIndexList
                auto& list = m_ClusteredShading.GetLightIndexList();
                if (!m_LightIndexListBuffer || m_LightIndexListBuffer->GetSize() < list.size() * 4) {
                    rhi::BufferDesc d; d.size = std::max<usize>(list.size() * 4, 64); d.usage = rhi::BufferUsage::Storage; d.cpuAccess = true;
                    m_LightIndexListBuffer = m_Device->CreateBuffer(d);
                }
                void* m2 = m_LightIndexListBuffer->Map();
                if (m2) { memcpy(m2, list.data(), list.size() * 4); m_LightIndexListBuffer->Unmap(); }
                m_Device->UpdateDescriptorSet(m_LightingSet, 8, rhi::DescriptorType::StorageBuffer, m_LightIndexListBuffer.get());
                useClustered = 1u;
            }

            c->SetPipeline(m_LightingPSO.get()); c->BindDescriptorSet(0, m_LightingSet);
            rhi::ClearValue clr{};
            c->BeginOffscreenPass(m_HDRTarget->GetNativeHandle(), m_HDRDepth->GetNativeHandle(), w, h, &clr, false);
            c->SetViewport({0,(float)h,(float)w,-(float)h,0,1});
            c->SetScissor({0,0,w,h});
            // Push constant: 含 cluster 网格参数 + 开关
            struct { float4x4 ivp; float4 cp; u32 lc; float ii;
                     u32 cTx; u32 cTy; float cNear; float cFar; float cLogF;
                     u32 cUse; u32 _pad2[2]; } lpc;
            lpc.ivp = ivp; lpc.cp = float4(camera.position, 0); lpc.ii = 0; lpc.lc = fpc.lightCount;
            lpc.cUse = useClustered;
            lpc.cTx = useClustered ? m_ClusteredShading.GetTileCountX() : 0u;
            lpc.cTy = useClustered ? m_ClusteredShading.GetTileCountY() : 0u;
            float n = camera.nearPlane, f = camera.farPlane;
            lpc.cNear = n; lpc.cFar = f; lpc.cLogF = std::log(f / n);
            c->SetPushConstants(0, sizeof(lpc), &lpc);
            c->Draw(3);
            c->EndOffscreenPass();
        });

    // ── 后处理链路：Bloom → DOF → MotionBlur（责任链，按序串联）──
    bool bloomActive = m_Bloom.IsEnabled() && m_Bloom.GetOutput() != nullptr;
    bool dofActive   = m_DOF.IsEnabled()   && m_DOF.GetOutput()   != nullptr;
    bool mbActive    = m_MotionBlur.IsEnabled() && m_MotionBlur.GetOutput() != nullptr;
    bool anyPostActive = bloomActive || dofActive || mbActive;

    // Bloom
    if (bloomActive) {
        auto bloomOut = rg.ImportTexture("Bloom_Out", m_Bloom.GetOutput());
        rg.AddPass("Bloom", {{hdrC, ResourceAccess::Read}}, {{bloomOut, ResourceAccess::Write}},
            [&](rhi::IRHICommandList* c) {
                c->PipelineBarrier(rhi::PipelineStage::ColorAttachmentOutput, rhi::PipelineStage::FragmentShader,
                    rhi::ResourceState::RenderTarget, rhi::ResourceState::ShaderResource, m_HDRTarget.get());
                m_Bloom.SetInput(m_HDRTarget.get(), m_HDRSampler.get());
                m_Bloom.Render(c);
            });
    }

    // DOF（景深）：读取 Bloom 输出或原始 HDR
    if (dofActive) {
        auto dofOut = rg.ImportTexture("DOF_Out", m_DOF.GetOutput());
        rg.AddPass("DOF", {{hdrC, ResourceAccess::Read}}, {{dofOut, ResourceAccess::Write}},
            [&, bloomActive](rhi::IRHICommandList* c) {
                auto* src = bloomActive ? m_Bloom.GetOutput() : m_HDRTarget.get();
                auto* smp = bloomActive ? m_Bloom.GetOutputSampler() : m_HDRSampler.get();
                m_DOF.SetInputs(src, smp, m_GBufferDepth.get());
                c->PipelineBarrier(rhi::PipelineStage::ColorAttachmentOutput, rhi::PipelineStage::FragmentShader,
                    rhi::ResourceState::RenderTarget, rhi::ResourceState::ShaderResource, src);
                m_DOF.Render(c);
            });
    }

    // MotionBlur：读取 DOF 输出 > Bloom 输出 > 原始 HDR
    if (mbActive) {
        auto mbOut = rg.ImportTexture("MB_Out", m_MotionBlur.GetOutput());
        rg.AddPass("MotionBlur", {{hdrC, ResourceAccess::Read}}, {{mbOut, ResourceAccess::Write}},
            [&, bloomActive, dofActive](rhi::IRHICommandList* c) {
                auto* src = dofActive   ? m_DOF.GetOutput()
                           : bloomActive ? m_Bloom.GetOutput()
                           :               m_HDRTarget.get();
                auto* smp = dofActive   ? m_DOF.GetOutputSampler()
                           : bloomActive ? m_Bloom.GetOutputSampler()
                           :               m_HDRSampler.get();
                m_MotionBlur.SetInputs(src, smp, m_GBufferD.get(), m_HDRSampler.get());
                c->PipelineBarrier(rhi::PipelineStage::ColorAttachmentOutput, rhi::PipelineStage::FragmentShader,
                    rhi::ResourceState::RenderTarget, rhi::ResourceState::ShaderResource, src);
                m_MotionBlur.Render(c);
            });
    }

    // TAA Resolve — 读取后处理链最后一个激活 Pass 的输出
    rg.AddPass("TAA_Resolve",
        {{hdrC, ResourceAccess::Read}},
        {},
        [&, bloomActive, dofActive, mbActive, anyPostActive, h = m_Height, w = m_Width](rhi::IRHICommandList* c) {
            if (!m_AntiAliasing || !m_AntiAliasing->IsEnabled()) return;
            auto* src = mbActive ? m_MotionBlur.GetOutput()
                      : dofActive ? m_DOF.GetOutput()
                      : bloomActive ? m_Bloom.GetOutput()
                      : m_HDRTarget.get();
            auto* smp = mbActive ? m_MotionBlur.GetOutputSampler()
                      : dofActive ? m_DOF.GetOutputSampler()
                      : bloomActive ? m_Bloom.GetOutputSampler()
                      : m_HDRSampler.get();
            m_AntiAliasing->SetInput(src, smp);
            if (anyPostActive) {
                c->PipelineBarrier(rhi::PipelineStage::ColorAttachmentOutput, rhi::PipelineStage::FragmentShader,
                    rhi::ResourceState::RenderTarget, rhi::ResourceState::ShaderResource, src);
            } else {
                c->PipelineBarrier(rhi::PipelineStage::ColorAttachmentOutput, rhi::PipelineStage::FragmentShader,
                    rhi::ResourceState::RenderTarget, rhi::ResourceState::ShaderResource, m_HDRTarget.get());
            }
            auto* taa = static_cast<AA_TAA*>(m_AntiAliasing.get());
            taa->SetGBufferInputs(m_GBufferDepth.get(), m_GBufferB.get(), m_GBufferD.get());
            float4x4 invCurrVP = glm::inverse(m_CurrViewProj);
            taa->UpdateUniforms(m_PrevViewProj, invCurrVP, m_Width, m_Height);
            m_AntiAliasing->Render(c);
        });

    // LDR 中间纹理（FXAA 启用时 ToneMap 写入此处，FXAA 再写入 BackBuffer）
    auto ldrTarget = rg.ImportTexture("LDR", m_LDRTarget.get());
    bool useFXAA = IsFXAAEnabled();
    bool useTAA  = (m_AntiAliasing && m_AntiAliasing->IsEnabled());

    // ToneMap Pass（HDR → LDR，输出到 LDR 中间纹理或直接 BackBuffer）
    rg.AddPass("ToneMap",
        {},
        {{useFXAA ? ldrTarget : backBuf, ResourceAccess::Write}},
        [this, useTAA, useFXAA, w, h, anyPostActive](rhi::IRHICommandList* c) {
            if (useTAA) {
                m_ToneMap->SetInput(m_AntiAliasing->GetOutputTexture(),
                                    m_AntiAliasing->GetOutputSampler());
            } else if (anyPostActive) {
                // 读取后处理链最后一个激活 Pass 的输出
                auto* src = m_MotionBlur.IsEnabled() ? m_MotionBlur.GetOutput()
                          : m_DOF.IsEnabled()        ? m_DOF.GetOutput()
                          :                            m_Bloom.GetOutput();
                auto* smp = m_MotionBlur.IsEnabled() ? m_MotionBlur.GetOutputSampler()
                          : m_DOF.IsEnabled()        ? m_DOF.GetOutputSampler()
                          :                            m_Bloom.GetOutputSampler();
                m_ToneMap->SetInput(src, smp);
            } else {
                c->PipelineBarrier(rhi::PipelineStage::ColorAttachmentOutput, rhi::PipelineStage::FragmentShader,
                    rhi::ResourceState::RenderTarget, rhi::ResourceState::ShaderResource, m_HDRTarget.get());
                m_ToneMap->SetInput(m_HDRTarget.get(), m_HDRSampler.get());
            }
            m_ToneMap->PreBind(c);
            if (useFXAA) {
                // FXAA 启用 → ToneMap 写入 LDR 中间纹理（离屏），FXAA 再写入 BackBuffer
                rhi::ClearValue clr{};
                c->BeginOffscreenPass(m_LDRTarget->GetNativeHandle(),
                    m_LDRDummyDepth->GetNativeHandle(), w, h, &clr, false);
                m_ToneMap->Render(c);
                c->EndOffscreenPass();
            } else {
                // FXAA 禁用 → ToneMap 直接写入 SwapChain BackBuffer
                c->BeginRenderPass(1, rhi::Format::BGRA8_UNORM);
                m_ToneMap->Render(c);
                c->EndRenderPass();
            }
        });

    // FXAA Pass（LDR 空间后处理抗锯齿，ToneMap 之后、Present 之前）
    if (useFXAA) {
        rg.AddPass("FXAA",
            {{ldrTarget, ResourceAccess::Read}},
            {{backBuf, ResourceAccess::Write}},
            [this](rhi::IRHICommandList* c) {
                m_FXAA->SetInput(m_LDRTarget.get(), m_LDRSampler.get());
                // LDR 纹理从 RenderTarget 过渡到 ShaderResource（ToneMap 写入后）
                c->PipelineBarrier(rhi::PipelineStage::ColorAttachmentOutput,
                                   rhi::PipelineStage::FragmentShader,
                                   rhi::ResourceState::RenderTarget,
                                   rhi::ResourceState::ShaderResource,
                                   m_LDRTarget.get());
                c->BeginRenderPass(1, rhi::Format::BGRA8_UNORM);
                m_FXAA->Render(c);
                c->EndRenderPass();
            });
    }

    // ── 帧末：保存当前帧 VP 供下一帧使用 ──
    m_PrevViewProj = m_CurrViewProj;
}

void DeferredPipeline::CollectLights(PushConstantData& pc, he::World& world,
                                      he::SceneGraph& sg, const CameraData& camera) {
    pc.lightCount = 0;
    auto cl = [&](he::Entity e, he::LightComponent& lc) {
        u32 i = pc.lightCount; if (i >= MAX_LIGHTS || !lc.enabled) return;
        GPULight gl{};
        gl.colorIntensity = float4(lc.color, lc.intensity);
        gl.shadowIndex = m_ShadowSystem ? m_ShadowSystem->GetShadowIndex(e) : -1;
        switch (lc.type) {
        case he::LightType::Directional: {
            auto* dl = static_cast<he::DirectionalLight*>(&lc);
            gl.directionType = float4(dl->direction, 0.0f); break;
        }
        case he::LightType::Point: {
            auto* pl = static_cast<he::PointLight*>(&lc);
            gl.positionRange = float4(sg.GetWorldPosition(e), pl->range);
            gl.directionType.w = 1.0f; break;
        }
        case he::LightType::Spot: {
            auto* sl = static_cast<he::SpotLight*>(&lc);
            gl.positionRange = float4(sg.GetWorldPosition(e), sl->range);
            gl.directionType = float4(glm::normalize(sl->direction), 2.0f); // Spot
            gl.coneAngles = float2(sl->innerConeAngle, sl->outerConeAngle); break;
        }
        default: break;
        }
        auto* lights = static_cast<GPULight*>(m_LightBuffers[m_CurrentFrameSlot]->Map());
        if (lights) lights[i] = gl;
        m_LightBuffers[m_CurrentFrameSlot]->Unmap();
        pc.lightCount++;
    };
    world.ForEach<he::DirectionalLight>(cl);
    world.ForEach<he::PointLight>(cl);
    world.ForEach<he::SpotLight>(cl);
    if (pc.lightCount == 0) {
        pc.lightCount = 1;
        GPULight gl{}; gl.colorIntensity = float4(1,0.95,0.85,5); gl.directionType = float4(0.5,-1,1,0); gl.shadowIndex = -1;
        auto* lights = static_cast<GPULight*>(m_LightBuffers[m_CurrentFrameSlot]->Map());
        if (lights) lights[0] = gl;
        m_LightBuffers[m_CurrentFrameSlot]->Unmap();
    }
}

void DeferredPipeline::UpdateIBLBindings(GI_IBL* gi) {
    (void)gi; // Lighting pass 已在 BuildFrameGraph 中直接绑定 IBL 纹理
}

void DeferredPipeline::UpdateRSMBindings() {
    (void)this; // Lighting pass 已在 BuildFrameGraph 中直接绑定 RSM 纹理
}

} // namespace he::render
