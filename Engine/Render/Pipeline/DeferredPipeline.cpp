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

// DGC 支持（仅在 Vulkan 后端启用）
#include "Vulkan/VulkanDGC.h"
#include "Vulkan/VulkanPipelineState.h"  // VulkanPipelineState
#include "Vulkan/VulkanDevice.h"        // VulkanDeviceAccess
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

// CVar: DGC 运行时开关（0=关闭，1=开启，默认关闭以保留传统 ExecuteIndirect 回退）
// 在控制台输入 "r.DGC.Enable 1" 可动态启用
static int32_t cvDGC_Enable = 0;
static const char* kCVar_DGC_Enable_Name = "r.DGC.Enable";

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

    // GBuffer E: worldPos.xyz（RGBA16_FLOAT，Lighting pass 直接读取）
    m_GBufferE = createGBuffer("GBufferE");

    // 硬件 MSAA：覆盖纹理和 PSO 的 sampleCount
    if (m_MSAAEnabled) {
        m_MSAA = std::make_unique<AA_MSAA>();
        m_MSAA->Initialize(device, m_Width, m_Height);
        HE_CORE_INFO("DeferredPipeline: MSAA {}x enabled (HDR 目标多采样，GBuffer 保持 1x)", m_MSAA->GetCurrentSampleCount());
    }

    // HDR target（MSAA 启用时使用多采样纹理）
    {
        rhi::TextureDesc d;
        d.format = rhi::Format::RGBA16_FLOAT;
        d.width = m_Width; d.height = m_Height;
        d.usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::ShaderResource;
        if (m_MSAA) m_MSAA->OverrideTextureDesc(d);
        m_HDRTarget = device->CreateTexture(d);
        rhi::TextureDesc dd;
        dd.format = rhi::Format::D32_FLOAT;
        dd.width = m_Width; dd.height = m_Height;
        dd.usage = rhi::TextureUsage::DepthStencil | rhi::TextureUsage::ShaderResource;  // 软粒子需要采样深度
        if (m_MSAA) m_MSAA->OverrideTextureDesc(dd);  // HDR 深度纹理与颜色纹理相同采样数
        m_HDRDepth = device->CreateTexture(dd);
        rhi::SamplerDesc s; s.minFilter = s.magFilter = rhi::FilterMode::Linear;
        s.addressU = s.addressV = rhi::AddressMode::ClampToEdge;
        m_HDRSampler = device->CreateSampler(s);

        // 点采样器（Nearest）：深度纹理精确读取，避免 Linear 插值
        // 在物体边缘混合背景深度导致 worldPos 重建错误
        rhi::SamplerDesc ptDesc;
        ptDesc.minFilter = ptDesc.magFilter = rhi::FilterMode::Nearest;
        ptDesc.addressU  = ptDesc.addressV  = rhi::AddressMode::ClampToEdge;
        m_PointSampler = device->CreateSampler(ptDesc);
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
    if (m_GPUCulling.usePTG) {
        m_GPUCulling.InitializePTG(device);
    }
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

    // 创建阴影 PSO（使用 GBuffer 的 descriptor set layout，
    // 阴影 VS 仅使用 binding=2 GPUObjectData[]，与 GBuffer layout 兼容）
    m_ShadowSystem->CreateShadowPSO(m_GBufferLayout);

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
    gbDesc.colorAttachmentCount = 5;
    gbDesc.colorFormats[0] = gbDesc.colorFormats[1] = gbDesc.colorFormats[2] = rhi::Format::RGBA16_FLOAT;
    gbDesc.colorFormats[3] = rhi::Format::RG16_FLOAT;  // velocity
    gbDesc.colorFormats[4] = rhi::Format::RGBA16_FLOAT; // worldPos.xyz
    gbDesc.pushConstantRanges = {pc};
    gbDesc.descriptorSetLayouts = {m_GBufferLayout};  // 仅 set=0，无 per-mesh
    gbDesc.debugName = "GBuffer";
    m_GBufferPSO = device->CreatePipelineState(gbDesc);
    HE_ASSERT(m_GBufferPSO, "DeferredPipeline: GBuffer PSO failed");

    // ── DGC 初始化（仅在硬件支持时）──
    if (device->GetCaps().supportsDGC) {
        // 获取 Vulkan 底层句柄
        auto* vkDev = static_cast<rhi::VulkanDevice*>(device);
        VkDevice vkDevice       = vkDev->GetVkDevice();
        VkPhysicalDevice vkPhysical = vkDev->GetVkPhysical();
        auto* vkPipelineState   = static_cast<rhi::VulkanPipelineState*>(m_GBufferPSO.get());
        VkPipeline vkPipeline   = vkPipelineState->GetPipeline();
        const auto& dgcFuncs    = vkDev->GetDGCFuncs();

        m_VulkanDGC = new rhi::VulkanDGC();
        bool dgcOK = m_VulkanDGC->Initialize(
            vkDevice, vkPhysical, vkPipeline,
            GPUCulling::kMaxObjects,  // maxSequences = 最大场景物体数
            GPUCulling::kMaxObjects,  // maxDraws = 最大绘制调用数
            dgcFuncs
        );
        if (dgcOK) {
            HE_CORE_INFO("DeferredPipeline: DGC 初始化成功，可通过 r.DGC.Enable 1 启用");
        } else {
            HE_CORE_WARN("DeferredPipeline: DGC 初始化失败，回退到 ExecuteIndirect 路径");
            delete m_VulkanDGC;
            m_VulkanDGC = nullptr;
        }
    } else {
        HE_CORE_INFO("DeferredPipeline: 硬件不支持 DGC，使用传统 ExecuteIndirect 路径");
    }

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
    m_GBufferCtx.gbWorldPos   = m_GBufferE.get();
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
    m_ProfilerPanel.SetProfiler(&m_Profiler);  // 绑定 ImGui 面板到 Profiler 数据源
    m_AutoExposure.Initialize(device, m_Width, m_Height);

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
        {23, rhi::DescriptorType::CombinedImageSampler, 1, 16}, // GBufferE (worldPos)
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
        for (u32 b : {0u,1u,2u,3u,4u,9u,10u,11u,12u,13u,14u,15u,16u,23u})
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
    if (m_MSAA) m_MSAA->OverridePSODesc(lDesc);  // 硬件 MSAA 覆盖 PSO sampleCount
    m_LightingPSO = device->CreatePipelineState(lDesc);
    HE_ASSERT(m_LightingPSO, "DeferredPipeline: Lighting PSO failed");

    // GPU 粒子系统
    m_ParticleRenderer.Initialize(device);
    m_ParticleRenderer.SetSceneDepth(m_HDRDepth.get(), m_PointSampler.get());  // 软粒子深度纹理

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
    m_GBufferD.reset(); m_GBufferE.reset();
    if (m_AntiAliasing) m_AntiAliasing->Shutdown();
    m_AntiAliasing.reset();
    if (m_FXAA) m_FXAA->Shutdown();
    m_FXAA.reset();
    if (m_SMAA) m_SMAA->Shutdown();
    m_SMAA.reset();
    if (m_MSAA) m_MSAA->Shutdown();
    m_MSAA.reset();
    m_LDRTarget.reset(); m_LDRSampler.reset(); m_LDRDummyDepth.reset();
    m_HDRTarget.reset(); m_HDRDepth.reset(); m_HDRSampler.reset(); m_PointSampler.reset();
    m_GBufferPSO.reset(); m_LightingPSO.reset();
    for (auto& b : m_LightBuffers) b.reset();
    for (auto& b : m_ObjectBuffers) b.reset();
    for (auto& b : m_ShadowBuffers) b.reset();
    for (auto& b : m_ShadowObjBuffers) b.reset();
    if (m_GPUCulling.usePTG) {
        m_GPUCulling.ShutdownPTG(m_Device);
    }
    m_GPUCulling.Shutdown(m_Device);
    m_GPUScene.Shutdown();
    if (m_GBufferRenderer) m_GBufferRenderer->Shutdown();
    m_GBufferRenderer.reset();

    // DGC 清理
    if (m_VulkanDGC) {
        auto* vkDev = static_cast<rhi::VulkanDevice*>(m_Device);
        m_VulkanDGC->Shutdown(vkDev->GetVkDevice());
        delete m_VulkanDGC;
        m_VulkanDGC = nullptr;
    }

    m_Profiler.Shutdown();
    m_AutoExposure.Shutdown();
    // AsyncCompute 清理
    if (m_CrossQueueFence != rhi::kInvalidFence) {
        m_Device->DestroyFence(m_CrossQueueFence);
        m_CrossQueueFence = rhi::kInvalidFence;
    }
    m_ComputeCmdList.reset();
    m_SSGI.Shutdown();
    m_SSR.Shutdown();
    m_DDGI.Shutdown();
    m_ParticleRenderer.Shutdown(m_Device);
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

void DeferredPipeline::EnableSMAA(bool enable) {
    m_SMAAEnabled = enable;
    if (!enable || !m_Device) return;
    // 懒初始化：首次 EnableSMAA(true) 时创建 GPU 资源
    if (!m_SMAA) {
        m_SMAA = std::make_unique<AA_SMAA>();
        if (!m_SMAA->Initialize(m_Device, m_Width, m_Height)) {
            HE_CORE_WARN("DeferredPipeline: AA_SMAA init failed");
            m_SMAA.reset();
        }
    }
}

void DeferredPipeline::EnableMSAA(bool enable) {
    // MSAA 需要修改 RT/PSO 采样数，仅在管线初始化前设置有效
    // 运行时切换需要重建管线（OnResize 路径会应用 OverrideTextureDesc）
    m_MSAAEnabled = enable;
    if (!enable || !m_Device) return;
    if (!m_MSAA) {
        m_MSAA = std::make_unique<AA_MSAA>();
        m_MSAA->Initialize(m_Device, m_Width, m_Height);
    }
    // 已有设备但管线未初始化：暂存标志，Initialize() 中生效
    // 已初始化：需重建 HDR 目标才能生效（警告用户）
    if (m_Ready) {
        HE_CORE_WARN("DeferredPipeline: MSAA toggled after init — 需重启应用生效");
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
    r(m_GBufferE, rhi::Format::RGBA16_FLOAT, rhi::TextureUsage::RenderTarget | rhi::TextureUsage::ShaderResource);
    r(m_GBufferDepth, rhi::Format::D32_FLOAT, rhi::TextureUsage::DepthStencil | rhi::TextureUsage::ShaderResource);
    r(m_HDRTarget, rhi::Format::RGBA16_FLOAT, rhi::TextureUsage::RenderTarget | rhi::TextureUsage::ShaderResource);
    r(m_HDRDepth, rhi::Format::D32_FLOAT, rhi::TextureUsage::DepthStencil | rhi::TextureUsage::ShaderResource);
    m_ParticleRenderer.SetSceneDepth(m_HDRDepth.get(), m_PointSampler.get());  // 软粒子深度纹理更新
    if (m_ToneMap) m_ToneMap->OnResize(w, h);
    if (m_Skybox)  { m_Skybox->Shutdown(); m_Skybox->Initialize(m_Device, w, h); }
    if (m_AntiAliasing) m_AntiAliasing->OnResize(w, h);
    if (m_FXAA) m_FXAA->OnResize(w, h);
    if (m_SMAA) m_SMAA->OnResize(w, h);    // SMAA 中间纹理随分辨率重建
    m_GBufferCtx.width  = w;
    m_GBufferCtx.height = h;
    m_GBufferCtx.gbA    = m_GBufferA.get();
    m_GBufferCtx.gbB    = m_GBufferB.get();
    m_GBufferCtx.gbC    = m_GBufferC.get();
    m_GBufferCtx.gbVel      = m_GBufferD.get();
    m_GBufferCtx.gbDepth    = m_GBufferDepth.get();
    m_GBufferCtx.gbWorldPos = m_GBufferE.get();
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
    m_AutoExposure.OnResize(w, h);
}

void DeferredPipeline::Render(rhi::IRHICommandList* cmd, he::World& world,
                               he::SceneGraph& sg, const CameraData& camera,
                               float deltaTime) {
    // ============================================================
    // AsyncCompute: RenderGraph 多阶段提交
    //
    // 当设备支持专用 Compute 队列时，RenderGraph 内部自动将 Pass
    // 拆分为三段提交：
    //   Phase 1: Graphics CmdList #1（Shadow + 前期 Pass）
    //   Phase 2: Compute CmdList（GPUCull, SSAO, DDGI, AutoExposure）
    //   Phase 3: Graphics CmdList #2（GBuffer, Lighting, PostProcess）
    //
    // 传统单队列回退：设备不支持 AsyncCompute 时自动降级。
    // ============================================================
    bool useAsyncCompute = m_Device->HasAsyncComputeQueue();

    if (useAsyncCompute && m_CrossQueueFence == rhi::kInvalidFence) {
        // 首次使用 AsyncCompute 时，创建跨队列时间线信号量
        m_CrossQueueFence = m_Device->CreateFence();
        HE_CORE_INFO("DeferredPipeline: AsyncCompute — CrossQueue fence created");
    }

    // ── 粒子模拟 (Compute，在 RenderGraph 之前) ──
    float4x4 viewProj = camera.GetViewProjMatrix();
    for (u32 pid : m_ParticleComponentIDs) {
        m_ParticleRenderer.DispatchCompute(cmd, pid, deltaTime, viewProj);
    }

    RenderGraph rg;
    rg.SetProfiler(&m_Profiler);
    rg.SetAsyncComputeEnabled(useAsyncCompute);
    if (useAsyncCompute) {
        // 将时间线信号量传入 RenderGraph，供多阶段提交使用
        rg.SetCrossQueueFence(m_CrossQueueFence);
        rg.SetTimelineBase(m_FrameCounter);
        m_FrameCounter += 2;  // 每帧消耗 2 个时间线值
    }
    BuildFrameGraph(rg, world, sg, camera);
    rg.Compile();

    // 统一入口：RenderGraph 根据 useAsyncCompute 自动分支
    rg.Execute(cmd, m_Device);
}

void DeferredPipeline::FlushComputeWork() {
    // 多阶段提交已在 RenderGraph::ExecuteWithAsyncCompute 内部自动完成
    // 保留此方法以兼容外部调用（Samples/04.Deferred.cpp line 1022）
}

// BuildFrameGraph 实现位于 DeferredPipeline_FrameGraph.cpp

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
    // 保底光源已注释：无光源时不再自动添加默认方向光
    //if (pc.lightCount == 0) {
    //    pc.lightCount = 1;
    //    GPULight gl{}; gl.colorIntensity = float4(1,0.95,0.85,5); gl.directionType = float4(0.5,-1,1,0); gl.shadowIndex = -1;
    //    auto* lights = static_cast<GPULight*>(m_LightBuffers[m_CurrentFrameSlot]->Map());
    //    if (lights) lights[0] = gl;
    //    m_LightBuffers[m_CurrentFrameSlot]->Unmap();
    //}
}

void DeferredPipeline::UpdateIBLBindings(GI_IBL* gi) {
    (void)gi; // Lighting pass 已在 BuildFrameGraph 中直接绑定 IBL 纹理
}

void DeferredPipeline::UpdateRSMBindings() {
    (void)this; // Lighting pass 已在 BuildFrameGraph 中直接绑定 RSM 纹理
}

} // namespace he::render
