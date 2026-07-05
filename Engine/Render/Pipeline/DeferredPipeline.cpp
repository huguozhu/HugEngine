#include "Pipeline/DeferredPipeline.h"
#include "GI/GI_IBL.h"
#include "GI/GI_RSM.h"
#include "Shadow/ShadowSystem.h"
#include "PostProcess/ToneMapPass.h"
#include "PostProcess/SkyboxPass.h"
#include "SceneRenderer.h"
#include "AntiAliasing/AA_TAA.h"
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

    // AA_TAA
    m_AntiAliasing = std::make_unique<AA_TAA>();
    if (!m_AntiAliasing->Initialize(device, m_Width, m_Height)) {
        HE_CORE_WARN("DeferredPipeline: AA_TAA init failed, anti-aliasing disabled");
        m_AntiAliasing.reset();
    }

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
    m_HDRTarget.reset(); m_HDRDepth.reset(); m_HDRSampler.reset();
    m_GBufferPSO.reset(); m_LightingPSO.reset();
    for (auto& b : m_LightBuffers) b.reset();
    for (auto& b : m_ObjectBuffers) b.reset();
    for (auto& b : m_ShadowBuffers) b.reset();
    for (auto& b : m_ShadowObjBuffers) b.reset();
    m_GPUCulling.Shutdown(m_Device);
    m_GPUScene.Shutdown();
    m_Device = nullptr; m_Ready = false;
    HE_CORE_INFO("DeferredPipeline shutdown");
}

// CreateTextureDescriptorSet 已移除 — 使用全局 bindless 纹理数组替代

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
}

void DeferredPipeline::Render(rhi::IRHICommandList* cmd, he::World& world,
                               he::SceneGraph& sg, const CameraData& camera) {
    RenderGraph rg;
    BuildFrameGraph(rg, world, sg, camera);
    rg.Compile();
    rg.Execute(cmd, m_Device);
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

    // GPU 剔除读回（上帧结果）+ 过滤可见物体
    bool useGPUVisible = false;
    if (m_GPUCulling.enabled) {
        m_GPUCulling.Readback(m_Device, m_GPUVisibleIndices);
        useGPUVisible = !m_GPUVisibleIndices.empty();
    }

    // GBuffer 4×MRT + SceneRenderer
    rg.AddPass("GB_Clear", {}, {{gbA, ResourceAccess::Write}, {gbB, ResourceAccess::Write},
        {gbC, ResourceAccess::Write}, {gbVel, ResourceAccess::Write}, {gbDepth, ResourceAccess::Write}},
        [&, w, h, useGPUVisible](rhi::IRHICommandList* c) {
            // 推送 bindless 纹理到全部已注册描述符集
            he::asset::BindlessTextureManager::Instance().FlushPending();

            m_Device->UpdateDescriptorSet(m_GBufferSet, 2, rhi::DescriptorType::StorageBuffer,
                m_ObjectBuffers[m_CurrentFrameSlot].get());
            c->SetPipeline(m_GBufferPSO.get());
            c->BindDescriptorSet(0, m_GBufferSet);
            rhi::ClearValue clears[5]{};
            clears[0].color[3] = 1.0f; clears[1].color[3] = 1.0f;
            clears[2].color[3] = 1.0f; clears[3].color[0] = 0.0f; // velocity=0
            clears[3].color[1] = 0.0f; clears[4].depth = 1.0f;
            void* cv[4] = {m_GBufferA->GetNativeHandle(), m_GBufferB->GetNativeHandle(),
                           m_GBufferC->GetNativeHandle(), m_GBufferD->GetNativeHandle()};
            c->BeginOffscreenPassMRT(cv, 4, m_GBufferDepth->GetNativeHandle(), w, h, clears, false);
            c->SetViewport({0,(float)h,(float)w,-(float)h,0,1});
            c->SetScissor({0,0,w,h});
            auto drawItems = m_SceneRenderer->Prepare(world, sg, camera, m_ObjectBuffers[m_CurrentFrameSlot].get());

            // GPU 剔除过滤（使用上帧结果）
            std::vector<DrawItem> filteredItems;
            if (useGPUVisible) {
                std::unordered_set<u32> visSet(m_GPUVisibleIndices.begin(), m_GPUVisibleIndices.end());
                for (auto& di : drawItems)
                    if (visSet.count(di.objectIndex)) filteredItems.push_back(di);
            } else {
                filteredItems = std::move(drawItems);
            }

            // GPU 剔除：GPUScene → Compute → Dispatch
            if (m_GPUCulling.enabled) {
                m_GPUScene.Collect(world, sg);
                m_GPUScene.Upload(m_Device);
                m_GPUCulling.SetSceneBuffer(m_Device, m_GPUScene.GetObjectBuffer());
                m_GPUCulling.Dispatch(c, camera.GetViewProjMatrix(), m_GPUScene.GetObjectCount());
                c->SetPipeline(m_GBufferPSO.get());
                c->BindDescriptorSet(0, m_GBufferSet);
            }

            float4x4 jitteredVP = camera.GetViewProjMatrix();
            for (auto& di : filteredItems) {
                struct {
                    float4x4 viewProjMatrix;
                    float4x4 prevViewProjMatrix;
                    u32      objectIndex;
                    u32      _pad[15];
                } pc;
                pc.viewProjMatrix     = jitteredVP;
                pc.prevViewProjMatrix = m_PrevViewProj;
                pc.objectIndex        = di.objectIndex;
                c->SetPushConstants(0, sizeof(pc), &pc);
                c->SetVertexBuffer(di.mesh->GetVertexBuffer().get(), 0);
                c->SetIndexBuffer(di.mesh->GetIndexBuffer().get());
                // 不再需要 bind set=1 — 纹理采样通过 bindless u_Textures[] 访问
                c->DrawIndexed(di.mesh->GetIndexCount());
            }
            c->EndOffscreenPass();
        });

    // Lighting Pass (全屏 PBR)
    rg.AddPass("Lighting",
        {{gbA, ResourceAccess::Read}, {gbB, ResourceAccess::Read}, {gbC, ResourceAccess::Read}},
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
            c->SetViewport({0,(float)h,(float)w,-(float)h,0,1}); c->SetScissor({0,0,w,h});
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

    // TAA Resolve Pass — 在 HDR 空间做时域抗锯齿
    rg.AddPass("TAA_Resolve",
        {{hdrC, ResourceAccess::Read}},
        {}, // TAA 写自己的 HistoryColor（自拥有），不写入 graph 管理的外部 RT
        [&, h = m_Height, w = m_Width](rhi::IRHICommandList* c) {
            if (!m_AntiAliasing || !m_AntiAliasing->IsEnabled()) return;

            // 设置输入：HDR 颜色 + GBuffer 辅助纹理
            m_AntiAliasing->SetInput(m_HDRTarget.get(), m_HDRSampler.get());
            auto* taa = static_cast<AA_TAA*>(m_AntiAliasing.get());
            taa->SetGBufferInputs(m_GBufferDepth.get(),
                                   m_GBufferB.get(),
                                   m_GBufferD.get());

            // 更新 TAA uniform buffer
            float4x4 invCurrVP = glm::inverse(m_CurrViewProj);
            taa->UpdateUniforms(
                m_PrevViewProj, invCurrVP, m_Width, m_Height);

            // Barrier：HDR 从 RenderTarget 转为 ShaderResource
            c->PipelineBarrier(rhi::PipelineStage::ColorAttachmentOutput,
                               rhi::PipelineStage::FragmentShader,
                               rhi::ResourceState::RenderTarget,
                               rhi::ResourceState::ShaderResource,
                               m_HDRTarget.get());

            m_AntiAliasing->Render(c);
        });

    rg.AddPass("ToneMap", {}, {{backBuf, ResourceAccess::Write}},
        [this](rhi::IRHICommandList* c) {
            if (m_AntiAliasing && m_AntiAliasing->IsEnabled()) {
                // TAA 已过渡 HDR → ShaderResource，ToneMap 直接采样 TAA 输出
                m_ToneMap->SetInput(m_AntiAliasing->GetOutputTexture(),
                                    m_AntiAliasing->GetOutputSampler());
            } else {
                // TAA 禁用时需手动过渡 HDR → ShaderResource
                c->PipelineBarrier(rhi::PipelineStage::ColorAttachmentOutput, rhi::PipelineStage::FragmentShader,
                    rhi::ResourceState::RenderTarget, rhi::ResourceState::ShaderResource, m_HDRTarget.get());
                m_ToneMap->SetInput(m_HDRTarget.get(), m_HDRSampler.get());
            }
            m_ToneMap->PreBind(c);
            c->BeginRenderPass(1, rhi::Format::BGRA8_UNORM);
            m_ToneMap->Render(c);
            c->EndRenderPass();
        });

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
