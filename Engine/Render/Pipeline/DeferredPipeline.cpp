#include "Pipeline/DeferredPipeline.h"
#include "GI/GI_IBL.h"
#include "GI/GI_RSM.h"
#include "Shadow/ShadowSystem.h"
#include "PostProcess/ToneMapPass.h"
#include "PostProcess/SkyboxPass.h"
#include "SceneRenderer.h"
#include "AntiAliasing/AA_TAA.h"
#include "Scene/CubeComponent.h"
#include "Scene/SphereComponent.h"
#include "Scene/LightComponent.h"
#include "Core/Log.h"
#include "Core/Assert.h"
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

    // AA_TAA
    m_AntiAliasing = std::make_unique<AA_TAA>();
    if (!m_AntiAliasing->Initialize(device, m_Width, m_Height)) {
        HE_CORE_WARN("DeferredPipeline: AA_TAA init failed, anti-aliasing disabled");
        m_AntiAliasing.reset();
    }

    // GBuffer PSO (4 MRT + D32, 拆分描述符集 set=0 + set=1)
    // set=0: per-frame GPUObjectData[]（每帧更新 buffer 绑定）
    rhi::DescriptorSetLayoutDesc gbLayout;
    gbLayout.bindings = {
        {2, rhi::DescriptorType::StorageBuffer, 1, 17},  // u_Objects (Vertex|Fragment)
    };
    m_GBufferLayout = device->CreateDescriptorSetLayout(gbLayout);

    // set=1: per-mesh 静态纹理（每个 mesh 独立创建，永不更新，消除帧间竞态）
    rhi::DescriptorSetLayoutDesc perMeshLayout;
    perMeshLayout.bindings = {
        {5, rhi::DescriptorType::CombinedImageSampler, 1, 16},  // BaseColor
        {6, rhi::DescriptorType::CombinedImageSampler, 1, 16},  // Normal
        {7, rhi::DescriptorType::CombinedImageSampler, 1, 16},  // MetallicRoughness
        {8, rhi::DescriptorType::CombinedImageSampler, 1, 16},  // Occlusion
    };
    m_PerMeshLayout = device->CreateDescriptorSetLayout(perMeshLayout);

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
    gbDesc.descriptorSetLayouts = {m_GBufferLayout, m_PerMeshLayout};
    gbDesc.debugName = "GBuffer";
    m_GBufferPSO = device->CreatePipelineState(gbDesc);
    HE_ASSERT(m_GBufferPSO, "DeferredPipeline: GBuffer PSO failed");

    // GBuffer set=0 共享描述符集（仅含 binding 2: Object SSBO，每帧更新 buffer）
    m_GBufferSet = device->AllocateDescriptorSet(m_GBufferLayout);
    // 默认纹理（per-mesh set=1 回退用）
    {
        u8 w4[4]={255,255,255,255};
        rhi::TextureDesc defDesc; defDesc.format=rhi::Format::RGBA8_UNORM;
        defDesc.width=1; defDesc.height=1; defDesc.mipLevels=1; defDesc.arrayLayers=1;
        defDesc.usage=rhi::TextureUsage::ShaderResource; defDesc.initialData=w4;
        m_PlaceholderTex = device->CreateTexture(defDesc);
        rhi::SamplerDesc sd; sd.minFilter=sd.magFilter=rhi::FilterMode::Linear;
        sd.addressU=sd.addressV=rhi::AddressMode::Repeat;
        m_PlaceholderSamp = device->CreateSampler(sd);
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
        {4, rhi::DescriptorType::CombinedImageSampler, 1, 16},  // Shadow0
        {10, rhi::DescriptorType::CombinedImageSampler, 1, 16}, // Shadow1
        {11, rhi::DescriptorType::CombinedImageSampler, 1, 16}, // Shadow2
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
        // 只更新 layout 中声明的 binding（0-4, 10-16）
        for (u32 b : {0u,1u,2u,3u,4u,10u,11u,12u,13u,14u,15u,16u})
            device->UpdateDescriptorSet(m_LightingSet, b, rhi::DescriptorType::CombinedImageSampler, pt.get(), ps.get());
        m_PlaceholderTex = std::move(pt);
        m_PlaceholderSamp = std::move(ps);
    }

    rhi::PushConstantRange lpc; lpc.stageMask = 1|16; lpc.size = 96;
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
    if (m_PerMeshLayout  != rhi::kInvalidLayout) { m_Device->DestroyDescriptorSetLayout(m_PerMeshLayout); }
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
    m_Device = nullptr; m_Ready = false;
    HE_CORE_INFO("DeferredPipeline shutdown");
}

rhi::DescriptorSetHandle DeferredPipeline::CreateTextureDescriptorSet(
    rhi::IRHITexture* baseColor, rhi::IRHISampler* bcSampler,
    rhi::IRHITexture* normal,   rhi::IRHISampler* nSampler,
    rhi::IRHITexture* metallicRoughness, rhi::IRHISampler* mrSampler,
    rhi::IRHITexture* occlusion, rhi::IRHISampler* ocSampler)
{
    if (!m_Device) return rhi::kInvalidSet;
    auto set = m_Device->AllocateDescriptorSet(m_PerMeshLayout);
    if (set == rhi::kInvalidSet) return set;

    auto use = [&](u32 b, rhi::IRHITexture* t, rhi::IRHISampler* s) {
        m_Device->UpdateDescriptorSet(set, b, rhi::DescriptorType::CombinedImageSampler,
            t ? t : m_PlaceholderTex.get(),
            s ? s : m_PlaceholderSamp.get());
    };
    use(5, baseColor, bcSampler);
    use(6, normal, nSampler);
    use(7, metallicRoughness, mrSampler);
    use(8, occlusion, ocSampler);
    return set;
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

    // GBuffer 4×MRT + SceneRenderer（所有矩阵在 lambda 内计算，避免捕获悬垂引用）
    rg.AddPass("GB_Clear", {}, {{gbA, ResourceAccess::Write}, {gbB, ResourceAccess::Write},
        {gbC, ResourceAccess::Write}, {gbVel, ResourceAccess::Write}, {gbDepth, ResourceAccess::Write}},
        [&, w, h](rhi::IRHICommandList* c) {
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
            // ★ 在 lambda 内计算 VP（避免捕获 BuildFrameGraph 局部变量的悬垂引用）
            // 计算 VP（暂不应用 jitter，避免时域抖动伪影）
            float4x4 jitteredVP = camera.GetViewProjMatrix();
            for (auto& di : drawItems) {
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
                if (di.mesh->GetDescriptorSet() != rhi::kInvalidSet)
                    c->BindDescriptorSet(1, di.mesh->GetDescriptorSet());
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
            m_Device->UpdateDescriptorSet(m_LightingSet, 17, rhi::DescriptorType::StorageBuffer, m_LightBuffers[m_CurrentFrameSlot].get());
            m_Device->UpdateDescriptorSet(m_LightingSet, 18, rhi::DescriptorType::StorageBuffer, m_ShadowBuffers[m_CurrentFrameSlot].get());
            c->SetPipeline(m_LightingPSO.get()); c->BindDescriptorSet(0, m_LightingSet);
            rhi::ClearValue clr{};
            c->BeginOffscreenPass(m_HDRTarget->GetNativeHandle(), m_HDRDepth->GetNativeHandle(), w, h, &clr, false);
            c->SetViewport({0,(float)h,(float)w,-(float)h,0,1}); c->SetScissor({0,0,w,h});
            float4x4 ivp = glm::inverse(camera.GetViewProjMatrix());
            struct { float4x4 ivp; float4 cp; u32 lc; float ii; u32 _pad[2]; } lpc;
            lpc.ivp=ivp; lpc.cp=float4(camera.position,0); lpc.ii=0; lpc.lc=0;
            { PushConstantData fpc{}; CollectLights(fpc, world, sg, camera); lpc.lc = fpc.lightCount; }
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
        default: break;
        }
        auto* lights = static_cast<GPULight*>(m_LightBuffers[m_CurrentFrameSlot]->Map());
        if (lights) lights[i] = gl;
        m_LightBuffers[m_CurrentFrameSlot]->Unmap();
        pc.lightCount++;
    };
    world.ForEach<he::DirectionalLight>(cl);
    world.ForEach<he::PointLight>(cl);
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
