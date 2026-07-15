// ============================================================
// DeferredPipeline_FrameGraph.cpp — RenderGraph Pass 定义
// 从 DeferredPipeline.cpp 拆分，包含 BuildFrameGraph()
// ============================================================

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

// DGC 支持
#include "Vulkan/VulkanDGC.h"
#include "Vulkan/VulkanPipelineState.h"
#include "Vulkan/VulkanDevice.h"
#include "Asset/BindlessTextureManager.h"
#include "Scene/LightComponent.h"
#include "Core/Log.h"
#include "Core/Assert.h"
#include <cstring>
#include <cmath>
#include "DeferredLighting.vert.spv.h"
#include "DeferredLighting.frag.spv.h"

// CVar: DGC 运行时开关（0=关闭，1=开启）
static int32_t cvDGC_Enable = 0;
static const char* kCVar_DGC_Enable_Name = "r.DGC.Enable";

namespace he::render {

// ============================================================
// BuildFrameGraph — 渲染图构建
// 定义延迟渲染管线中所有 Pass 的依赖关系与执行逻辑
// ============================================================

void DeferredPipeline::BuildFrameGraph(RenderGraph& rg, he::World& world,
                                        he::SceneGraph& sg, const CameraData& camera) {
    if (m_SwapChain) rg.SetSwapChain(m_SwapChain);
    u32 w = m_Width, h = m_Height;
    auto gbA = rg.ImportTexture("GB_A", m_GBufferA.get());
    auto gbB = rg.ImportTexture("GB_B", m_GBufferB.get());
    auto gbC = rg.ImportTexture("GB_C", m_GBufferC.get());
    auto gbDepth = rg.ImportTexture("GB_Depth", m_GBufferDepth.get());
    auto gbVel = rg.ImportTexture("GB_Vel", m_GBufferD.get());
    auto gbWorldPos = rg.ImportTexture("GB_WorldPos", m_GBufferE.get());
    auto hdrC = rg.ImportTexture("HDR_C", m_HDRTarget.get());
    auto backBuf = rg.ImportBackBuffer();

    (void)world; (void)sg;

    // ── 帧首：更新成员变量 ──
    m_CurrViewProj = camera.GetViewProjMatrix();
    static bool firstFrame = true;
    if (firstFrame) { m_PrevViewProj = m_CurrViewProj; firstFrame = false; }
    if (m_AntiAliasing) m_AntiAliasing->OnBeginFrame();

    // GPUScene 收集 → [GPU 模式: 填充 IndirectDraw 参数] → 上传
    m_GPUScene.Collect(world, sg);
    if (m_GBufferMode == GBufferMode::GPU) {
        if (!m_BatchBuilt) { m_MeshBatcher.Build(world); m_BatchBuilt = true; }
        m_MeshBatcher.FillGPUScene(m_GPUScene);
    }
    m_GPUScene.Upload(m_Device);

    // GPU 剔除读回（上帧结果）+ 过滤可见物体
    bool useGPUVisible = false;
    if (m_GPUCulling.enabled) {
        m_GPUCulling.Readback(m_Device, m_GPUVisibleIndices);
        useGPUVisible = !m_GPUVisibleIndices.empty();
    }

    // ── GPU 剔除 Compute Pass ──
    if (m_GPUCulling.useTwoPhase) {
        rg.AddPass("GPU_Cull_Phase1",
            {{gbDepth, ResourceAccess::Read}},
            {},
            [&, w, h](rhi::IRHICommandList* c) {
                if (!m_GPUCulling.enabled) return;
                m_GPUCulling.SetSceneBuffer(m_Device, m_GPUScene.GetObjectBuffer());
                if (m_GBufferDepth) m_GPUCulling.SetDepthTexture(m_Device, m_GBufferDepth.get(), w, h);
                m_GPUCulling.DispatchPhase1(c, camera.GetViewProjMatrix(),
                                            m_GPUScene.GetObjectCount(), w, h);
                c->SetPipeline(m_GBufferPSO.get());
            },
            RGPassQueue::Compute);
    } else {
        rg.AddPass("GPU_Cull",
            {{gbDepth, ResourceAccess::Read}},
            {},
            [&, w, h](rhi::IRHICommandList* c) {
                if (!m_GPUCulling.enabled) return;
                m_GPUCulling.SetSceneBuffer(m_Device, m_GPUScene.GetObjectBuffer());
                if (m_GBufferDepth) m_GPUCulling.SetDepthTexture(m_Device, m_GBufferDepth.get(), w, h);
                if (m_GPUCulling.usePTG) {
                    m_GPUCulling.SignalPTG(c, camera.GetViewProjMatrix(),
                                          m_GPUScene.GetObjectCount(), w, h);
                } else {
                    m_GPUCulling.Dispatch(c, camera.GetViewProjMatrix(),
                                          m_GPUScene.GetObjectCount(), w, h);
                }
                c->SetPipeline(m_GBufferPSO.get());
            },
            RGPassQueue::Compute);
    }

    // ── Shadow Pass ──
    {
        u32 slot = m_CurrentFrameSlot;
        m_ShadowSystem->SetRenderResources(
            m_ShadowObjBuffers[slot].get(),
            m_ShadowBuffers[slot].get(),
            m_GBufferSet);

        SubsystemContext sctx;
        sctx.world       = &world;
        sctx.sceneGraph  = &sg;
        sctx.camera      = &camera;
        m_ShadowSystem->Update(sctx);
    }

    ResourceHandle csmMaps[3];
    for (u32 c = 0; c < 3; ++c) {
        auto* tex = m_ShadowSystem->GetShadowMap(c);
        if (tex) {
            char name[32];
            snprintf(name, sizeof(name), "CSM_Shadow_C%u", c);
            csmMaps[c] = rg.ImportTexture(name, tex);
        } else {
            csmMaps[c] = kInvalidHandle;
        }
    }
    auto* spotSTex = m_ShadowSystem->GetShadowMap(4);
    auto spotShadowHandle = spotSTex ? rg.ImportTexture("SpotShadow", spotSTex) : kInvalidHandle;

    {
        std::vector<PassResource> shadowWrites;
        for (u32 c = 0; c < 3; ++c)
            if (csmMaps[c] != kInvalidHandle)
                shadowWrites.push_back(RG_WRITE(csmMaps[c]));
        if (spotShadowHandle != kInvalidHandle)
            shadowWrites.push_back(RG_WRITE(spotShadowHandle));
        shadowWrites.push_back(RG_WRITE(gbDepth));
        shadowWrites.push_back(RG_WRITE(gbWorldPos));

        rg.AddPass("Shadow", {}, std::move(shadowWrites),
            [this](rhi::IRHICommandList* c) {
                u32 slot = m_CurrentFrameSlot;
                m_Device->UpdateDescriptorSet(m_GBufferSet, 2,
                    rhi::DescriptorType::StorageBuffer,
                    m_ShadowObjBuffers[slot].get());

                m_ShadowSystem->Render(c);

                m_Device->UpdateDescriptorSet(m_GBufferSet, 2,
                    rhi::DescriptorType::StorageBuffer,
                    m_ObjectBuffers[slot].get());
            });
    }

    // ── GBuffer Pass ──
    rg.AddPass("GB_Clear", {}, {{gbA, ResourceAccess::Write}, {gbB, ResourceAccess::Write},
        {gbC, ResourceAccess::Write}, {gbVel, ResourceAccess::Write}, {gbWorldPos, ResourceAccess::Write},
        {gbDepth, ResourceAccess::Write}},
        [&](rhi::IRHICommandList* c) {
            m_GBufferCtx.objectBuffer = m_ObjectBuffers[m_CurrentFrameSlot].get();
            m_GBufferCtx.prevViewProj = m_PrevViewProj;

            m_DGCEnabled = (cvDGC_Enable != 0)
                && m_VulkanDGC && m_VulkanDGC->IsInitialized()
                && m_GPUCulling.GetIndirectBuffer()
                && m_GPUCulling.enabled;
            if (m_DGCEnabled) {
                auto& dgcCtx = m_GBufferCtx.dgc;
                dgcCtx.enabled               = true;
                dgcCtx.indirectCommandsLayout = reinterpret_cast<void*>(m_VulkanDGC->GetLayout());
                dgcCtx.indirectExecutionSet   = reinterpret_cast<void*>(m_VulkanDGC->GetExecutionSet());
                dgcCtx.preprocessBufferAddr   = m_VulkanDGC->GetPreprocessAddress();
                dgcCtx.preprocessBufferSize   = m_VulkanDGC->GetPreprocessSize();
                dgcCtx.maxSequenceCount       = m_VulkanDGC->GetMaxSequences();
                dgcCtx.sequenceBuffer         = m_GPUCulling.GetIndirectBuffer();
                dgcCtx.countBuffer            = m_GPUCulling.GetDrawCountBuffer();
            } else {
                m_GBufferCtx.dgc = {};
            }

            m_GBufferRenderer->Render(c, m_GBufferCtx, world, sg, camera);
        });

    // ── 两阶段剔除 Phase 2 ──
    if (m_GPUCulling.useTwoPhase && m_GPUCulling.enabled) {
        rg.AddPass("HiZ_Build",
            {{gbDepth, ResourceAccess::Read}},
            {},
            [&, w, h](rhi::IRHICommandList* c) {
                m_GPUCulling.BuildHiZPyramid(c, w, h);
            });

        rg.AddPass("GPU_Cull_Phase2",
            {{gbDepth, ResourceAccess::Read}},
            {},
            [&](rhi::IRHICommandList* c) {
                m_Device->UpdateDescriptorSet(m_GPUCulling.GetPhase2Set(), 3,
                    rhi::DescriptorType::CombinedImageSampler,
                    m_GBufferDepth.get(), m_GPUCulling.GetHiZSampler());
                m_GPUCulling.DispatchPhase2(c, m_Width, m_Height);
            });
    }

    // ── DDGI Probe Update ──
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
                c->SetPipeline(m_LightingPSO.get());
            }
        },
        RGPassQueue::Compute);

    // ── SSAO Pass ──
    auto ssaoOut = rg.ImportTexture("SSAO_Output", m_SSAO.GetAOTexture());
    rg.AddPass("SSAO", {}, {{ssaoOut, ResourceAccess::Write}},
        [&, w, h](rhi::IRHICommandList* c) {
            m_SSAO.PreBind(c);
            rhi::ClearValue aoClear; aoClear.color[0]=aoClear.color[1]=aoClear.color[2]=aoClear.color[3]=1.0f;
            c->BeginOffscreenPass(m_SSAO.GetAOTexture()->GetNativeHandle(), nullptr, w, h, &aoClear, false);
            if (m_SSAO.enabled) {
                m_SSAO.SetInputs(m_GBufferDepth.get(), m_GBufferB.get());
                m_SSAO.Render(c);
            }
            c->EndOffscreenPass();
        });

    // ── SSR Pass ──
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

    // ── SSGI Pass ──
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

    // ── Lighting Pass ──
    rg.AddPass("Lighting",
        {{gbA, ResourceAccess::Read}, {gbB, ResourceAccess::Read}, {gbC, ResourceAccess::Read},
         {gbWorldPos, ResourceAccess::Read},
         {ssgiDenoised, ResourceAccess::Read},
         {ssrDenoised, ResourceAccess::Read}},
        {{hdrC, ResourceAccess::Write}},
        [&, w, h](rhi::IRHICommandList* c) {
            c->PipelineBarrier(rhi::PipelineStage::LateFragmentTests, rhi::PipelineStage::FragmentShader,
                rhi::ResourceState::DepthStencilWrite, rhi::ResourceState::DepthStencilRead, m_GBufferDepth.get());
            auto bindTex = [&](u32 b, rhi::IRHITexture* t) { if(t) m_Device->UpdateDescriptorSet(m_LightingSet, b, rhi::DescriptorType::CombinedImageSampler, t, m_HDRSampler.get()); };
            bindTex(0, m_GBufferA.get()); bindTex(1, m_GBufferB.get()); bindTex(2, m_GBufferC.get());
            m_Device->UpdateDescriptorSet(m_LightingSet, 23, rhi::DescriptorType::CombinedImageSampler,
                m_GBufferE.get(), m_PointSampler.get());
            m_Device->UpdateDescriptorSet(m_LightingSet, 3, rhi::DescriptorType::CombinedImageSampler,
                m_GBufferDepth.get(), m_PointSampler.get());
            if (m_ShadowSystem && m_ShadowSystem->GetShadowMap(0))
                bindTex(4, m_ShadowSystem->GetShadowMap(0));
            if (m_ShadowSystem && m_ShadowSystem->GetShadowMap(1))
                bindTex(10, m_ShadowSystem->GetShadowMap(1));
            if (m_ShadowSystem && m_ShadowSystem->GetShadowMap(2))
                bindTex(11, m_ShadowSystem->GetShadowMap(2));
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
            m_Device->UpdateDescriptorSet(m_LightingSet, 22, rhi::DescriptorType::StorageBuffer,
                m_DDGI.GetProbeBuffer());

            // Clustered Shading
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
                auto& grid = m_ClusteredShading.GetLightGrid();
                if (!m_LightGridBuffer || m_LightGridBuffer->GetSize() < grid.size() * 8) {
                    rhi::BufferDesc d; d.size = grid.size() * 8; d.usage = rhi::BufferUsage::Storage; d.cpuAccess = true;
                    m_LightGridBuffer = m_Device->CreateBuffer(d);
                }
                void* m = m_LightGridBuffer->Map();
                if (m) { memcpy(m, grid.data(), grid.size() * 8); m_LightGridBuffer->Unmap(); }
                m_Device->UpdateDescriptorSet(m_LightingSet, 7, rhi::DescriptorType::StorageBuffer, m_LightGridBuffer.get());
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
            DeferredLightingPushConstant lpc{};
            float iblIntensity = m_GI ? m_GI->GetSettings().intensity : 1.0f;
            lpc.cameraPosition  = float4(camera.position, 0);
            lpc.iblIntensity    = iblIntensity;
            lpc.lightCount      = fpc.lightCount;
            lpc.useClustered    = useClustered;
            lpc.clusterTilesX   = useClustered ? m_ClusteredShading.GetTileCountX() : 0u;
            lpc.clusterTilesY   = useClustered ? m_ClusteredShading.GetTileCountY() : 0u;
            float n = camera.nearPlane, f = camera.farPlane;
            lpc.clusterNear     = n;
            lpc.clusterFar      = f;
            lpc.clusterLogFactor = std::log(f / n);
            c->SetPushConstants(0, sizeof(lpc), &lpc);
            c->Draw(3, 1, 0, 0);
            c->EndOffscreenPass();
        });

    // ── 后处理链入口 ──
    rhi::IRHITexture* postInput = m_HDRTarget.get();
    rhi::IRHITexture* priorOut  = nullptr;

    // Bloom
    if (m_Bloom.enabled) {
        auto bloomOut = rg.ImportTexture("Bloom_Out", m_Bloom.GetOutput());
        rg.AddPass("Bloom",
            {{hdrC, ResourceAccess::Read}},
            {{bloomOut, ResourceAccess::Write}},
            [&](rhi::IRHICommandList* c) {
                m_Bloom.SetInput(postInput);
                m_Bloom.Render(c);
            });
        priorOut = m_Bloom.GetOutput();
        postInput = m_Bloom.GetOutput();
    }

    // DOF
    if (m_DOF.enabled) {
        auto dofOut = rg.ImportTexture("DOF_Out", m_DOF.GetOutput());
        rg.AddPass("DOF",
            {{hdrC, ResourceAccess::Read}},
            {{dofOut, ResourceAccess::Write}},
            [&, w, h](rhi::IRHICommandList* c) {
                m_DOF.SetInput(postInput, m_GBufferDepth.get(), m_GBufferCtx.gbVel, w, h);
                m_DOF.Render(c);
            });
        priorOut = m_DOF.GetOutput();
        postInput = m_DOF.GetOutput();
    }

    // MotionBlur
    if (m_MotionBlur.enabled) {
        auto mbOut = rg.ImportTexture("MB_Out", m_MotionBlur.GetOutput());
        rg.AddPass("MotionBlur",
            {{hdrC, ResourceAccess::Read}},
            {{mbOut, ResourceAccess::Write}},
            [&, w, h](rhi::IRHICommandList* c) {
                m_MotionBlur.SetInput(postInput, m_GBufferCtx.gbVel, m_GBufferDepth.get(), w, h);
                m_MotionBlur.Render(c);
            });
        priorOut = m_MotionBlur.GetOutput();
        postInput = m_MotionBlur.GetOutput();
    }

    // ToneMap
    auto ldrTarget = rg.ImportTexture("LDR", m_LDRTarget.get());
    bool hasPostToneMap = m_ColorGrading.enabled || m_FXAAEnabled || m_SMAAEnabled || IsMSAAEnabled();
    rg.AddPass("ToneMap",
        {{hdrC, ResourceAccess::Read}, {gbDepth, ResourceAccess::Read}},
        hasPostToneMap ? PassResourceList{{ldrTarget, ResourceAccess::Write}}
                       : PassResourceList{{backBuf, ResourceAccess::Write}},
        [&, w, h, hasPostToneMap](rhi::IRHICommandList* c) {
            m_Device->UpdateDescriptorSet(m_ToneMap->GetDescriptorSet(), 1,
                rhi::DescriptorType::CombinedImageSampler, postInput, m_HDRSampler.get());
            m_ToneMap->SetExposure(m_AutoExposure.GetExposure());
            c->SetPipeline(m_ToneMap->GetPipeline());
            c->BindDescriptorSet(0, m_ToneMap->GetDescriptorSet());
            if (hasPostToneMap) {
                c->BeginOffscreenPass(m_LDRTarget->GetNativeHandle(), m_LDRDummyDepth->GetNativeHandle(),
                                      w, h, nullptr, false);
            } else {
                c->BeginRenderPass(1, rhi::Format::BGRA8_UNORM);
            }
            c->SetViewport({0,(float)h,(float)w,-(float)h,0,1});
            c->SetScissor({0,0,w,h});
            c->Draw(3, 1, 0, 0);
            c->EndOffscreenPass();
        });

    rhi::IRHITexture* aaInput = hasPostToneMap ? m_LDRTarget.get() : nullptr;

    // ColorGrading
    if (m_ColorGrading.enabled && hasPostToneMap) {
        auto cgOut = rg.ImportTexture("CG_Out", m_ColorGrading.GetOutput());
        rg.AddPass("ColorGrading",
            {{ldrTarget, ResourceAccess::Read}},
            {{cgOut, ResourceAccess::Write}},
            [&](rhi::IRHICommandList* c) {
                m_ColorGrading.SetInput(aaInput);
                m_ColorGrading.Render(c);
            });
        aaInput = m_ColorGrading.GetOutput();
    }

    // TAA
    if (m_AntiAliasing && m_AntiAliasing->IsEnabled()) {
        rg.AddPass("TAA_Resolve",
            {{gbDepth, ResourceAccess::Read}, {gbVel, ResourceAccess::Read}},
            {{backBuf, ResourceAccess::Write}},
            [&, w, h](rhi::IRHICommandList* c) {
                auto* taa = static_cast<AA_TAA*>(m_AntiAliasing.get());
                taa->SetGBufferInputs(m_GBufferDepth.get(), m_GBufferB.get(), m_GBufferD.get());
                taa->UpdateUniforms(m_PrevViewProj, camera.GetInvViewProjMatrix(), w, h);
                if (hasPostToneMap) {
                    taa->Render(c, aaInput);
                } else {
                    HE_CORE_WARN("TAA requires LDR input from ToneMap→LDR path (FXAA off?)");
                }
            });
    }

    // SMAA
    if (IsSMAAEnabled() && hasPostToneMap) {
        auto smaaOut = rg.ImportTexture("SMAA_Out", m_SMAA->GetOutput());
        rg.AddPass("SMAA",
            {{ldrTarget, ResourceAccess::Read}},
            {{smaaOut, ResourceAccess::Write}},
            [&](rhi::IRHICommandList* c) {
                m_SMAA->SetInput(aaInput);
                m_SMAA->Render(c);
            });
        rg.AddPass("SMAA_Present",
            {{smaaOut, ResourceAccess::Read}},
            {{backBuf, ResourceAccess::Write}},
            [&](rhi::IRHICommandList* c) {
                rhi::ClearValue clr{};
                c->BeginRenderPass(1, rhi::Format::BGRA8_UNORM);
                m_SMAA->CopyToBackBuffer(c);
                c->EndRenderPass();
            });
    } else if (IsFXAAEnabled() && hasPostToneMap) {
        rg.AddPass("FXAA",
            {{ldrTarget, ResourceAccess::Read}},
            {{backBuf, ResourceAccess::Write}},
            [&](rhi::IRHICommandList* c) {
                rhi::IRHITexture* fxaaInput = aaInput;
                c->PipelineBarrier(rhi::PipelineStage::ColorAttachmentOutput,
                                   rhi::PipelineStage::FragmentShader,
                                   rhi::ResourceState::RenderTarget,
                                   rhi::ResourceState::ShaderResource, fxaaInput);
                c->BeginRenderPass(1, rhi::Format::BGRA8_UNORM);
                m_FXAA->Render(c);
                c->EndRenderPass();
            });
    }

    // ── 帧末：保存当前帧 VP 供下一帧使用 ──
    m_PrevViewProj = m_CurrViewProj;
}

} // namespace he::render
