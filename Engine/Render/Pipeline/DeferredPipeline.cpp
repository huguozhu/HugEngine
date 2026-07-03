#include "Pipeline/DeferredPipeline.h"
#include "GI/GI_IBL.h"
#include "GI/GI_RSM.h"
#include "Shadow/ShadowSystem.h"
#include "PostProcess/ToneMapPass.h"
#include "PostProcess/SkyboxPass.h"
#include "SceneRenderer.h"
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

    // GBuffer PSO (3 MRT + D32, 纹理描述符共享 Forward 布局)
    rhi::DescriptorSetLayoutDesc gbLayout;
    gbLayout.bindings = {
        {2, rhi::DescriptorType::StorageBuffer, 1, 17},  // u_Objects (Vertex|Fragment)
        {5, rhi::DescriptorType::CombinedImageSampler, 1, 16},
        {6, rhi::DescriptorType::CombinedImageSampler, 1, 16},
        {7, rhi::DescriptorType::CombinedImageSampler, 1, 16},
        {8, rhi::DescriptorType::CombinedImageSampler, 1, 16},
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
    rhi::PushConstantRange pc; pc.stageMask = 1|16; pc.size = 128;
    rhi::PipelineStateDesc gbDesc;
    gbDesc.vertexShader = &gbVS; gbDesc.pixelShader = &gbFS;
    gbDesc.vertexLayout = vl;
    gbDesc.depthTest = true; gbDesc.depthWrite = true;
    gbDesc.depthFormat = rhi::Format::D32_FLOAT;
    gbDesc.colorAttachmentCount = 3;
    gbDesc.colorFormats[0] = gbDesc.colorFormats[1] = gbDesc.colorFormats[2] = rhi::Format::RGBA16_FLOAT;
    gbDesc.pushConstantRanges = {pc};
    gbDesc.descriptorSetLayouts = {m_GBufferLayout};
    gbDesc.debugName = "GBuffer";
    m_GBufferPSO = device->CreatePipelineState(gbDesc);
    HE_ASSERT(m_GBufferPSO, "DeferredPipeline: GBuffer PSO failed");

    // GBuffer 描述符集（绑定 Object SSBO + 默认纹理）
    m_GBufferSet = device->AllocateDescriptorSet(m_GBufferLayout);
    // 默认 1x1 白纹理 + 线性采样器
    std::unique_ptr<rhi::IRHITexture> defTex;
    std::unique_ptr<rhi::IRHISampler> defSamp;
    {
        u8 w4[4]={255,255,255,255};
        rhi::TextureDesc defDesc; defDesc.format=rhi::Format::RGBA8_UNORM;
        defDesc.width=1; defDesc.height=1; defDesc.mipLevels=1; defDesc.arrayLayers=1;
        defDesc.usage=rhi::TextureUsage::ShaderResource; defDesc.initialData=w4;
        defTex = device->CreateTexture(defDesc);
        rhi::SamplerDesc sd; sd.minFilter=sd.magFilter=rhi::FilterMode::Linear;
        sd.addressU=sd.addressV=rhi::AddressMode::Repeat;
        defSamp = device->CreateSampler(sd);
    }
    device->UpdateDescriptorSet(m_GBufferSet, 5, rhi::DescriptorType::CombinedImageSampler, defTex.get(), defSamp.get());
    device->UpdateDescriptorSet(m_GBufferSet, 6, rhi::DescriptorType::CombinedImageSampler, defTex.get(), defSamp.get());
    device->UpdateDescriptorSet(m_GBufferSet, 7, rhi::DescriptorType::CombinedImageSampler, defTex.get(), defSamp.get());
    device->UpdateDescriptorSet(m_GBufferSet, 8, rhi::DescriptorType::CombinedImageSampler, defTex.get(), defSamp.get());

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
    if (m_LightingLayout != rhi::kInvalidLayout) { m_Device->DestroyDescriptorSetLayout(m_LightingLayout); }
    m_GBufferA.reset(); m_GBufferB.reset(); m_GBufferC.reset(); m_GBufferDepth.reset();
    m_HDRTarget.reset(); m_HDRDepth.reset(); m_HDRSampler.reset();
    m_GBufferPSO.reset(); m_LightingPSO.reset();
    for (auto& b : m_LightBuffers) b.reset();
    for (auto& b : m_ObjectBuffers) b.reset();
    for (auto& b : m_ShadowBuffers) b.reset();
    for (auto& b : m_ShadowObjBuffers) b.reset();
    m_Device = nullptr; m_Ready = false;
    HE_CORE_INFO("DeferredPipeline shutdown");
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
    r(m_GBufferDepth, rhi::Format::D32_FLOAT, rhi::TextureUsage::DepthStencil | rhi::TextureUsage::ShaderResource);
    r(m_HDRTarget, rhi::Format::RGBA16_FLOAT, rhi::TextureUsage::RenderTarget | rhi::TextureUsage::ShaderResource);
    r(m_HDRDepth, rhi::Format::D32_FLOAT, rhi::TextureUsage::DepthStencil);
    if (m_ToneMap) m_ToneMap->OnResize(w, h);
    if (m_Skybox)  { m_Skybox->Shutdown(); m_Skybox->Initialize(m_Device, w, h); }
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
    auto gbD = rg.ImportTexture("GB_D", m_GBufferDepth.get());
    auto hdrC = rg.ImportTexture("HDR_C", m_HDRTarget.get());
    auto hdrD = rg.ImportTexture("HDR_D", m_HDRDepth.get());
    auto backBuf = rg.ImportBackBuffer();

    auto* giIBL = dynamic_cast<GI_IBL*>(m_GI.get());
    ResourceHandle irr = kInvalidHandle, pref = kInvalidHandle, lut = kInvalidHandle;
    bool hasIBL = (giIBL && giIBL->IsReady());
    if (hasIBL && giIBL->GetIrradianceMap() && giIBL->GetPrefilterMap() && giIBL->GetBRDF_LUT()) {
        irr  = rg.ImportTexture("IBL_Irr", giIBL->GetIrradianceMap());
        pref = rg.ImportTexture("IBL_Pref", giIBL->GetPrefilterMap());
        lut  = rg.ImportTexture("IBL_LUT", giIBL->GetBRDF_LUT());
    }
    ResourceHandle sm0 = kInvalidHandle;
    if (m_ShadowSystem && m_ShadowSystem->GetShadowMapCount() > 0)
        sm0 = rg.ImportTexture("SM0", m_ShadowSystem->GetShadowMap(0));

    // GBuffer Pass (几何→MRT+Depth)
    rg.AddPass("GBuffer", {}, {{gbA, ResourceAccess::Write}, {gbB, ResourceAccess::Write},
        {gbC, ResourceAccess::Write}, {gbD, ResourceAccess::Write}},
        [&, w, h](rhi::IRHICommandList* c) {
            m_Device->UpdateDescriptorSet(m_GBufferSet, 2, rhi::DescriptorType::StorageBuffer,
                m_ObjectBuffers[m_CurrentFrameSlot].get());
            c->SetPipeline(m_GBufferPSO.get());
            c->BindDescriptorSet(0, m_GBufferSet);
            rhi::ClearValue clears[4]{};
            clears[3].depth = 1.0f;
            void* cv[3] = {m_GBufferA->GetNativeHandle(), m_GBufferB->GetNativeHandle(), m_GBufferC->GetNativeHandle()};
            c->BeginOffscreenPassMRT(cv, 3, m_GBufferDepth->GetNativeHandle(), w, h, clears, false);
            c->SetViewport({0,(float)h,(float)w,-(float)h,0,1});
            c->SetScissor({0,0,w,h});
            auto drawItems = m_SceneRenderer->Prepare(world, sg, camera, m_ObjectBuffers[m_CurrentFrameSlot].get());
            for (auto& di : drawItems) {
                struct { float4x4 vp; u32 oi; u32 _pad[12]; } pc;
                pc.vp = camera.GetViewProjMatrix(); pc.oi = di.objectIndex;
                c->SetPushConstants(0, sizeof(pc), &pc);
                c->SetVertexBuffer(di.mesh->GetVertexBuffer().get(), 0);
                c->SetIndexBuffer(di.mesh->GetIndexBuffer().get());
                c->DrawIndexed(di.mesh->GetIndexCount());
            }
            c->EndOffscreenPass();
        });

    // Lighting Pass (全屏 PBR)
    rg.AddPass("Lighting",
        {{gbA, ResourceAccess::Read}, {gbB, ResourceAccess::Read}, {gbC, ResourceAccess::Read},
         {irr, ResourceAccess::Read}, {pref, ResourceAccess::Read},
         {lut, ResourceAccess::Read}, {sm0, ResourceAccess::Read}},
        {{hdrC, ResourceAccess::Write}, {hdrD, ResourceAccess::Write}},
        [&, w, h](rhi::IRHICommandList* c) {
            // 显式 Barrier: DepthStencilWrite → DepthStencilRead (深度采样用)
            c->PipelineBarrier(rhi::PipelineStage::LateFragmentTests, rhi::PipelineStage::FragmentShader,
                rhi::ResourceState::DepthStencilWrite, rhi::ResourceState::DepthStencilRead, m_GBufferDepth.get());
            // 绑定 Lighting 描述符 (GBuffer + Shadow + IBL)
            m_Device->UpdateDescriptorSet(m_LightingSet, 0, rhi::DescriptorType::CombinedImageSampler,
                m_GBufferA.get(), m_HDRSampler.get());
            m_Device->UpdateDescriptorSet(m_LightingSet, 1, rhi::DescriptorType::CombinedImageSampler,
                m_GBufferB.get(), m_HDRSampler.get());
            m_Device->UpdateDescriptorSet(m_LightingSet, 2, rhi::DescriptorType::CombinedImageSampler,
                m_GBufferC.get(), m_HDRSampler.get());
            m_Device->UpdateDescriptorSet(m_LightingSet, 3, rhi::DescriptorType::CombinedImageSampler,
                m_GBufferDepth.get(), m_HDRSampler.get());
            if (m_ShadowSystem->GetShadowMap(0))
                m_Device->UpdateDescriptorSet(m_LightingSet, 4, rhi::DescriptorType::CombinedImageSampler,
                    m_ShadowSystem->GetShadowMap(0), m_ShadowSystem->GetShadowSampler());
            if (giIBL && giIBL->GetIrradianceMap() && giIBL->GetIBLSampler())
                m_Device->UpdateDescriptorSet(m_LightingSet, 12, rhi::DescriptorType::CombinedImageSampler,
                    giIBL->GetIrradianceMap(), giIBL->GetIBLSampler());
            if (giIBL && giIBL->GetPrefilterMap() && giIBL->GetIBLSampler())
                m_Device->UpdateDescriptorSet(m_LightingSet, 13, rhi::DescriptorType::CombinedImageSampler,
                    giIBL->GetPrefilterMap(), giIBL->GetIBLSampler());
            if (giIBL && giIBL->GetBRDF_LUT() && giIBL->GetIBLSampler())
                m_Device->UpdateDescriptorSet(m_LightingSet, 14, rhi::DescriptorType::CombinedImageSampler,
                    giIBL->GetBRDF_LUT(), giIBL->GetIBLSampler());
            m_Device->UpdateDescriptorSet(m_LightingSet, 17, rhi::DescriptorType::StorageBuffer,
                m_LightBuffers[m_CurrentFrameSlot].get());
            m_Device->UpdateDescriptorSet(m_LightingSet, 18, rhi::DescriptorType::StorageBuffer,
                m_ShadowBuffers[m_CurrentFrameSlot].get());

            c->SetPipeline(m_LightingPSO.get());
            c->BindDescriptorSet(0, m_LightingSet);
            rhi::ClearValue clr{};
            c->BeginOffscreenPass(m_HDRTarget->GetNativeHandle(), m_HDRDepth->GetNativeHandle(), w, h, &clr, false);
            c->SetViewport({0,(float)h,(float)w,-(float)h,0,1});
            c->SetScissor({0,0,w,h});

            // Push constants
            float4x4 invVP = glm::inverse(camera.GetViewProjMatrix());
            struct { float4x4 ivp; float4 cp; u32 lc; float ii; u32 _pad[2]; } lpc;
            lpc.ivp = invVP; lpc.cp = float4(camera.position, 0.0f);
            lpc.lc = 0; lpc.ii = m_GI ? m_GI->GetSettings().intensity : 1.0f;
            // 填充光源数据
            {
                PushConstantData fpc{};
                CollectLights(fpc, world, sg, camera);
                lpc.lc = fpc.lightCount;
                auto* ll = static_cast<GPULight*>(m_LightBuffers[m_CurrentFrameSlot]->Map());
                (void)ll; // CollectLights 已填充
            }
            c->SetPushConstants(0, sizeof(lpc), &lpc);
            c->Draw(3);
            c->EndOffscreenPass();
        });

    // ToneMap
    rg.AddPass("ToneMap", {}, {{backBuf, ResourceAccess::Write}},
        [this](rhi::IRHICommandList* c) {
            c->PipelineBarrier(rhi::PipelineStage::ColorAttachmentOutput, rhi::PipelineStage::FragmentShader,
                rhi::ResourceState::RenderTarget, rhi::ResourceState::ShaderResource, m_HDRTarget.get());
            m_ToneMap->SetInput(m_HDRTarget.get(), m_HDRSampler.get());
            m_ToneMap->PreBind(c);
            c->BeginRenderPass(1, rhi::Format::BGRA8_UNORM);
            m_ToneMap->Render(c);
            c->EndRenderPass();
        });
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
