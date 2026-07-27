// ============================================================
// HybridRTPipeline.cpp — 混合 Ray Tracing 管线实现
// GBuffer（光栅化）+ RT 阴影/反射/AO/GI（硬件 RT）+ DDGI
// ============================================================
#include "Pipeline/HybridRTPipeline.h"
#include "Scene/World.h"
#include "Scene/SceneGraph.h"
#include "Scene/LightComponent.h"
#include "Scene/MeshComponent.h"
#include "Core/Log.h"
#include "Core/Assert.h"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <cstring>

// RT 着色器 SPIR-V
#include "RT_Shadow.rgen.spv.h"
#include "RT_Common.rmiss.spv.h"
#include "RT_Common.rchit.spv.h"

namespace he::render {

bool HybridRTPipeline::Initialize(rhi::IRHIDevice* device) {
    m_Device = device;
    m_Width  = rhi::kDefaultBackBufferWidth;
    m_Height = rhi::kDefaultBackBufferHeight;

    // ── 共享组件 ──
    m_GBuffer = std::make_unique<GBufferRenderer>();
    m_GBuffer->Initialize(device, m_Width, m_Height);

    m_Lighting.Initialize(device, m_Width, m_Height);

    m_PostProcess.Initialize(device, m_Width, m_Height);

    // ── GPU Driven ──
    m_SceneRenderer = std::make_unique<SceneRenderer>();
    m_GPUCulling.Initialize(device);
    m_GPUScene.Initialize(device);
    m_DDGI.Initialize(device, m_Width, m_Height);
    m_ParticleRenderer.Initialize(device);

    // 注入子系统指针到 GBufferRenderer
    m_GBuffer->SetSceneRenderer(m_SceneRenderer.get());
    m_GBuffer->SetGPUCulling(&m_GPUCulling);
    m_GBuffer->SetGPUScene(&m_GPUScene);
    m_GBuffer->SetVisibleIndices(&m_GPUVisibleIndices);
    m_GBuffer->SetMeshBatcher(&m_MeshBatcher);

    // ── 三缓冲 ──
    for (u32 i = 0; i < 3; ++i) {
        m_LightBuffers[i]  = device->CreateBuffer({sizeof(GPULight) * MAX_LIGHTS, rhi::BufferUsage::Storage});
        m_ObjectBuffers[i] = device->CreateBuffer({sizeof(GPUObjectData) * MAX_OBJECTS, rhi::BufferUsage::Storage});
        m_ShadowBuffers[i] = device->CreateBuffer({sizeof(GPUShadowData) * MAX_SHADOWS, rhi::BufferUsage::Storage});
    }

    // ── Ray Tracing 子系统（可选）──
    m_RTEnabled = device->GetCaps().supportsRayTracing;
    if (m_RTEnabled) {
        m_RTPass = std::make_unique<RTPass>();

        std::vector<rhi::ShaderBytecode> rtShaders;
        {
            rhi::ShaderBytecode bc;
            bc.stage = rhi::ShaderStage::RayGen;
            bc.spirv = k_RT_Shadow_rgen_spv; bc.entryPoint = "main";
            rtShaders.push_back(bc);
        }
        {
            rhi::ShaderBytecode bc;
            bc.stage = rhi::ShaderStage::Miss;
            bc.spirv = k_RT_Common_rmiss_spv; bc.entryPoint = "main";
            rtShaders.push_back(bc);
        }
        {
            rhi::ShaderBytecode bc;
            bc.stage = rhi::ShaderStage::ClosestHit;
            bc.spirv = k_RT_Common_rchit_spv; bc.entryPoint = "main";
            rtShaders.push_back(bc);
        }

        std::vector<rhi::RTShaderGroup> groups;
        {
            rhi::RTShaderGroup rg;
            rg.type = rhi::RTShaderGroupType::RayGen;
            rg.generalShader = 0; rg.name = "RayGen";
            groups.push_back(rg);
        }
        {
            rhi::RTShaderGroup mg;
            mg.type = rhi::RTShaderGroupType::Miss;
            mg.generalShader = 1; mg.name = "Miss";
            groups.push_back(mg);
        }
        {
            rhi::RTShaderGroup hg;
            hg.type = rhi::RTShaderGroupType::Hit;
            hg.closestHitShader = 2; hg.name = "Hit";
            groups.push_back(hg);
        }

        if (m_RTPass->Initialize(device, rtShaders, groups)) {
            HE_CORE_INFO("HybridRTPipeline: RTPass 初始化完成");

            // 初始化 RT 效果 Pass
            m_RTShadow = std::make_unique<RTShadowPass>();
            m_RTShadow->Initialize(device, m_Width, m_Height, true);  // 默认半分辨率
        } else {
            m_RTEnabled = false;
            m_RTPass.reset();
            HE_CORE_WARN("HybridRTPipeline: RTPass 初始化失败，RT 禁用");
        }
    } else {
        HE_CORE_INFO("HybridRTPipeline: 设备不支持 RT，以光栅化模式运行");
    }

    m_Ready = true;
    HE_CORE_INFO("HybridRTPipeline: 初始化完成 ({}x{}, RT={})",
                 m_Width, m_Height, m_RTEnabled ? "on" : "off");
    return true;
}

void HybridRTPipeline::Shutdown() {
    m_DDGI.Shutdown();
    m_ParticleRenderer.Shutdown(m_Device);
    m_GPUCulling.Shutdown(m_Device);
    m_GPUScene.Shutdown();
    if (m_RTPass) { m_RTPass->Shutdown(); m_RTPass.reset(); }
    if (m_RTShadow) { m_RTShadow->Shutdown(); m_RTShadow.reset(); }
    m_PostProcess.Shutdown();
    m_Lighting.Shutdown();
    if (m_GBuffer) m_GBuffer->Shutdown();
    for (auto& b : m_LightBuffers) b.reset();
    for (auto& b : m_ObjectBuffers) b.reset();
    for (auto& b : m_ShadowBuffers) b.reset();
    m_Device = nullptr; m_Ready = false;
    HE_CORE_INFO("HybridRTPipeline: shutdown");
}

void HybridRTPipeline::NextFrame() {
    m_CurrentFrameSlot = (m_CurrentFrameSlot + 1) % 3;
}

void HybridRTPipeline::OnResize(u32 w, u32 h) {
    if (w == m_Width && h == m_Height) return;
    m_Width = w; m_Height = h;
    if (m_GBuffer) m_GBuffer->OnResize(w, h);
    m_Lighting.OnResize(m_Device, w, h);
    m_PostProcess.OnResize(m_Device, w, h);
    m_DDGI.OnResize(w, h);
}

void HybridRTPipeline::SetGBufferMode(GBufferRenderer::Mode m) {
    if (m_GBuffer) m_GBuffer->SetMode(m);
}

GBufferRenderer::Mode HybridRTPipeline::GetGBufferMode() const {
    return m_GBuffer ? m_GBuffer->GetMode() : GBufferRenderer::Mode::CPU;
}

// ============================================================
// Render — 主渲染入口
// ============================================================
void HybridRTPipeline::Render(rhi::IRHICommandList* cmd, he::World& world,
                               he::SceneGraph& sg, const CameraData& camera,
                               float deltaTime) {
    (void)deltaTime;

    RenderGraph rg;
    rg.SetSwapChain(m_SwapChain);
    rg.SetProfiler(&m_Profiler);

    BuildFrameGraph(rg, world, sg, camera);

    rg.Compile();
    rg.Execute(cmd, m_Device);
}

// ============================================================
// CollectLights — 收集场景光源数据到 SSBO
// ============================================================
void HybridRTPipeline::CollectLights(he::World& world, he::SceneGraph& sg,
                                      const CameraData& camera, u32& outLightCount) {
    (void)camera;
    u32 lightCount = 0;

    auto cl = [&](he::Entity e, he::LightComponent& lc) {
        if (lightCount >= MAX_LIGHTS || !lc.enabled) return;
        GPULight gl{};
        gl.colorIntensity = float4(lc.color, lc.intensity);
        gl.shadowIndex = -1;

        switch (lc.type) {
        case LightType::Directional: {
            auto* dl = static_cast<DirectionalLight*>(&lc);
            gl.directionType = float4(dl->direction, 0.0f);
            break;
        }
        case LightType::Point: {
            auto* pl = static_cast<PointLight*>(&lc);
            gl.positionRange = float4(sg.GetWorldPosition(e), pl->range);
            gl.directionType.w = 1.0f;
            break;
        }
        case LightType::Spot: {
            auto* sl = static_cast<SpotLight*>(&lc);
            gl.positionRange = float4(sg.GetWorldPosition(e), sl->range);
            gl.directionType = float4(sl->direction, 2.0f);
            gl.coneAngles = float2(sl->innerConeAngle, sl->outerConeAngle);
            break;
        }
        default: break;
        }

        auto* lights = static_cast<GPULight*>(m_LightBuffers[m_CurrentFrameSlot]->Map());
        if (lights) lights[lightCount] = gl;
        m_LightBuffers[m_CurrentFrameSlot]->Unmap();
        lightCount++;
    };

    world.ForEach<DirectionalLight>(cl);
    world.ForEach<PointLight>(cl);
    world.ForEach<SpotLight>(cl);
    outLightCount = lightCount;
}

// ============================================================
// BuildFrameGraph — 渲染图定义
// ============================================================
void HybridRTPipeline::BuildFrameGraph(RenderGraph& rg, he::World& world,
                                        he::SceneGraph& sg, const CameraData& camera) {
    u32 w = m_Width, h = m_Height;
    auto gb = m_GBuffer->ImportToRenderGraph(rg);
    auto gbA = gb.albedo, gbB = gb.normal, gbC = gb.emissive;
    auto gbDepth = gb.depth, gbVel = gb.velocity, gbWorldPos = gb.worldPos;
    auto hdrC   = rg.ImportTexture("HDR_C", m_Lighting.GetHDRTarget());
    auto backBuf = rg.ImportBackBuffer();

    m_CurrViewProj = camera.GetViewProjMatrix();
    static bool firstFrame = true;
    if (firstFrame) { m_PrevViewProj = m_CurrViewProj; firstFrame = false; }

    // ── Pass 0: AS Build — BLAS/TLAS 构建（RT 启用时）──
    if (m_RTEnabled && m_RTPass && m_RTPass->IsValid()) {
        rg.AddPass("AS_Build", {}, {},
            [this, &world, &sg](rhi::IRHICommandList* c) {
                m_RTPass->BuildAS(c, world, sg);
            });
    }

    // ── Pass 1: GBuffer（光栅化，与 DeferredPipeline 相同）──
    rg.AddPass("GB_Clear", {}, {{gbA, ResourceAccess::Write}, {gbB, ResourceAccess::Write},
        {gbC, ResourceAccess::Write}, {gbVel, ResourceAccess::Write},
        {gbWorldPos, ResourceAccess::Write}, {gbDepth, ResourceAccess::Write}},
        [&](rhi::IRHICommandList* c) {
            m_GBuffer->SetObjectBuffer(m_ObjectBuffers[m_CurrentFrameSlot].get());
            m_GBuffer->SetPrevViewProj(m_PrevViewProj);
            m_GBuffer->Render(c, world, sg, camera);
        });

    // ── Pass 2: RT Shadow — 对 GBuffer 中每个有效像素发射阴影射线 ──
    rhi::IRHITexture* rtShadowTex = nullptr;
    if (m_RTEnabled && m_RTShadow) {
        auto shadowMaskHandle = rg.ImportTexture("RT_ShadowMask",
            m_RTShadow->GetShadowMask());
        rtShadowTex = m_RTShadow->GetShadowMask();

        rg.AddPass("RT_Shadow",
            {{gbDepth, ResourceAccess::Read}, {gbB, ResourceAccess::Read}},
            {{shadowMaskHandle, ResourceAccess::Write}},
            [&](rhi::IRHICommandList* c) {
                float4x4 invVP = glm::inverse(camera.GetViewProjMatrix());
                m_RTShadow->Execute(c,
                    m_RTPass->GetTLAS(),
                    m_GBuffer->GetDepth(),
                    m_GBuffer->GetNormal(),
                    m_LightBuffers[m_CurrentFrameSlot].get(),
                    0,  // lightCount — 将由 shader 内部从 uniform 读取
                    invVP,
                    camera.position,
                    m_CurrentFrameSlot);
                // 实际 TraceRays 通过 RTPass 调度
                if (m_RTPass && m_RTPass->IsValid()) {
                    m_RTPass->BindPipeline(c);
                    m_RTPass->TraceRays(c,
                        m_RTShadow->GetWidth(),
                        m_RTShadow->GetHeight());
                }
            });
    }

    // ── Pass 3: DDGI 更新（探针 GI，RT 管线中保留用于远距离低频光照）──
    rg.AddPass("DDGI_Update", {{gbA, ResourceAccess::Read}, {gbB, ResourceAccess::Read},
        {gbDepth, ResourceAccess::Read}}, {},
        [&](rhi::IRHICommandList* c) {
            m_DDGI.SetGBufferInputs(m_GBuffer->GetDepth(),
                m_GBuffer->GetNormal(), m_GBuffer->GetAlbedo());
            SubsystemContext ctx;
            ctx.world = &world; ctx.sceneGraph = &sg;
            ctx.camera = &camera;
            ctx.viewportWidth = w; ctx.viewportHeight = h;
            m_DDGI.Update(ctx);
            m_DDGI.Render(c);
        }, RGPassQueue::Compute);

    // ── Pass 3: Lighting（延迟光照，RT 输入源 → 暂无 RT 纹理）──
    u32 lightCount = 0;
    CollectLights(world, sg, camera, lightCount);

    rg.AddPass("Lighting",
        {{gbA, ResourceAccess::Read}, {gbB, ResourceAccess::Read},
         {gbC, ResourceAccess::Read}, {gbWorldPos, ResourceAccess::Read}},
        {{hdrC, ResourceAccess::Write}},
        [&, w, h](rhi::IRHICommandList* c) {
            c->PipelineBarrier(rhi::PipelineStage::LateFragmentTests,
                rhi::PipelineStage::FragmentShader,
                rhi::ResourceState::DepthStencilWrite,
                rhi::ResourceState::DepthStencilRead,
                m_GBuffer->GetDepth());

            // 委托 LightingPass 执行（RT 纹理待 Phase 4 实现）
            m_Lighting.Render(c,
                m_GBuffer->GetAlbedo(), m_GBuffer->GetNormal(),
                m_GBuffer->GetEmissive(),
                m_GBuffer->GetDepth(), m_GBuffer->GetWorldPos(),
                nullptr, nullptr, nullptr, nullptr,  // 无 CSM 阴影贴图
                m_LightBuffers[m_CurrentFrameSlot].get(),
                m_ShadowBuffers[m_CurrentFrameSlot].get(),
                nullptr,  // 无 SSAO
                nullptr, nullptr,  // 无 SSGI
                nullptr, nullptr,  // 无 SSR
                m_DDGI.GetProbeBuffer(),
                nullptr, nullptr, nullptr, nullptr,  // 无 Clustered
                rtShadowTex, nullptr, nullptr, nullptr,  // RT 纹理
                float4(camera.position, 0), 1.0f, lightCount, w, h);
        });

    // ── DDGI CaptureHDR ──
    rg.AddPass("DDGI_CaptureHDR", {{hdrC, ResourceAccess::Read}}, {},
        [&](rhi::IRHICommandList* c) {
            m_DDGI.CaptureHDR(c, m_Lighting.GetHDRTarget());
        });

    // ── 后处理链（与 DeferredPipeline 相同）──
    // 自动曝光
    rg.AddPass("AutoExposure", {{hdrC, ResourceAccess::Read}}, {},
        [&](rhi::IRHICommandList* c) {
            m_PostProcess.GetAutoExposure().SetInput(
                m_Lighting.GetHDRTarget(), m_Lighting.GetHDRSampler());
            m_PostProcess.GetAutoExposure().Render(c);
        }, RGPassQueue::Compute);

    // Bloom → DOF → MotionBlur → TAA → ToneMap → ColorGrading → SMAA/FXAA
    bool bloomActive = m_PostProcess.GetBloom().IsEnabled()
                       && m_PostProcess.GetBloom().GetOutput() != nullptr;
    bool dofActive   = m_PostProcess.GetDOF().IsEnabled()
                       && m_PostProcess.GetDOF().GetOutput() != nullptr;
    bool mbActive    = m_PostProcess.GetMotionBlur().IsEnabled()
                       && m_PostProcess.GetMotionBlur().GetOutput() != nullptr;
    bool anyPostActive = bloomActive || dofActive || mbActive;

    if (bloomActive) {
        auto bloomOut = rg.ImportTexture("Bloom_Out",
            m_PostProcess.GetBloom().GetOutput());
        rg.AddPass("Bloom", {{hdrC, ResourceAccess::Read}},
            {{bloomOut, ResourceAccess::Write}},
            [&](rhi::IRHICommandList* c) {
                m_PostProcess.GetBloom().SetInput(
                    m_Lighting.GetHDRTarget(), m_Lighting.GetHDRSampler());
                m_PostProcess.GetBloom().Render(c);
            });
    }

    // 简化后处理：ToneMap → BackBuffer
    rg.AddPass("ToneMap", {}, {{backBuf, ResourceAccess::Write}},
        [&, w, h](rhi::IRHICommandList* c) {
            auto* src = m_Lighting.GetHDRTarget();
            auto* smp = m_Lighting.GetHDRSampler();
            m_PostProcess.GetToneMap()->SetInput(src, smp);
            m_PostProcess.GetToneMap()->SetExposure(
                m_PostProcess.GetAutoExposure().GetExposure()
                * exp2(camera.exposureBias));
            m_PostProcess.GetToneMap()->PreBind(c);
            rhi::ClearValue clr{};
            c->BeginRenderPass(1, rhi::Format::BGRA8_UNORM);
            m_PostProcess.GetToneMap()->Render(c);
            c->EndRenderPass();
        });

    m_PrevViewProj = m_CurrViewProj;
}

} // namespace he::render
