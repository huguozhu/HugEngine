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
#include <cmath>
#include <cstring>
#include <unordered_set>
#include "GBuffer.vert.spv.h"
#include "GBuffer.frag.spv.h"
#include "DeferredLighting.vert.spv.h"
#include "DeferredLighting.frag.spv.h"

// CVar: DGC 运行时开关（0=关闭，1=开启，默认关闭以保留传统 ExecuteIndirect 回退）
// 在控制台输入 "r.DGC.Enable 1" 可动态启用
static int32_t cvDGC_Enable = 0;

// CVar: 瞬态资源路径验证开关（与 DeferredPipeline.cpp 中同步）
static int32_t cvTransientTest = 0;  // 瞬态资源路径验证开关（1=启用测试 Pass）
static const char* kCVar_DGC_Enable_Name = "r.DGC.Enable";


// 从 DeferredPipeline.cpp 提取 — BuildFrameGraph 渲染图定义

namespace he::render {

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

    // ── 帧首：更新成员变量（lambda 内通过 this 安全访问，无悬垂引用风险）──
    m_CurrViewProj = camera.GetViewProjMatrix();
    static bool firstFrame = true;
    if (firstFrame) { m_PrevViewProj = m_CurrViewProj; firstFrame = false; }
    if (m_AntiAliasing) m_AntiAliasing->OnBeginFrame();

    // ── 物理相机参数推导（根据 camera.exposureBias 等字段判断是否启用）──
    // 非零 exposureBias 表示使用了物理相机参数，将其传递到 DOF/MotionBlur/Exposure
    if (camera.apertureDiameter > 0.0f) {
        m_DOF.SetFocusDepth(camera.focusDistance);
        m_DOF.SetIntensity(camera.maxCoC);
    }
    m_MotionBlur.SetIntensity(camera.motionBlurIntensity);
    // exposureBias 叠加到 AutoExposure 输出（在 ToneMap Pass 前处理）

    // GPUScene 收集 → [GPU 模式: 填充 IndirectDraw 参数] → 上传
    m_GPUScene.Collect(world, sg);
    if (m_GBufferMode == GBufferMode::GPU) {
        if (!m_BatchBuilt) { m_MeshBatcher.Build(world); m_BatchBuilt = true; }
        m_MeshBatcher.FillGPUScene(m_GPUScene);  // 在 Upload 前写入 draw 参数
    }
    m_GPUScene.Upload(m_Device);

    // GPU 剔除读回（上帧结果）+ 过滤可见物体
    // 禁用时必须清空，避免 GBufferRenderer_CPU 使用脏数据过滤物体
    bool useGPUVisible = false;
    if (m_GPUCulling.enabled) {
        m_GPUCulling.Readback(m_Device, m_GPUVisibleIndices);
        useGPUVisible = !m_GPUVisibleIndices.empty();
        // 首帧启用 GPU Culling 时输出 Readback 统计
        static bool readbackLogged = false;
        if (!readbackLogged && m_GPUCulling.enabled) {
            readbackLogged = true;
            HE_CORE_INFO("GPU Cull Readback: {} visible / {} gpuScene objects",
                m_GPUVisibleIndices.size(), m_GPUScene.GetObjectCount());
        }
    } else {
        m_GPUVisibleIndices.clear();
    }

    // ── GPU 剔除 Compute Pass（读上帧 GBuffer 深度 → 调度下帧剔除）──
    // 单阶段模式：Dispatch() 直接输出 IndirectDraw 命令
    // 两阶段模式：DispatchPhase1() 仅做粗筛，输出候选列表
    // 必须在 GBuffer 之前：此时 gbDepth 保留上帧数据且未作为渲染目标
    if (m_GPUCulling.useTwoPhase) {
        // Phase 1: 视锥 + 上帧 Hi-Z 粗筛 → 候选列表（AsyncCompute 队列，帧首执行）
        rg.AddPass("GPU_Cull_Phase1",
            {{gbDepth, ResourceAccess::Read}},  // 读上一帧深度做粗筛遮挡测试
            {},
            [&, w, h](rhi::IRHICommandList* c) {
                if (!m_GPUCulling.enabled) return;
                m_GPUCulling.SetSceneBuffer(m_Device, m_GPUScene.GetObjectBuffer());
                if (m_GBufferDepth) m_GPUCulling.SetDepthTexture(m_Device, m_GBufferDepth.get(), w, h);
                m_GPUCulling.DispatchPhase1(c, camera.GetViewProjMatrix(),
                                            m_GPUScene.GetObjectCount(), w, h);
                c->SetPipeline(m_GBufferPSO.get());
            },
            RGPassQueue::Compute);  // AsyncCompute: Phase 1 在 Compute 队列执行
    } else {
        // 单阶段模式：完整剔除 → IndirectDraw 命令
        // PTG per-frame dispatch 模式可用 AsyncCompute（与普通 Dispatch 相同路径）
        rg.AddPass("GPU_Cull",
            {{gbDepth, ResourceAccess::Read}},  // 读上一帧深度做 Hi-Z 遮挡剔除
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

    // ── Shadow Pass（使用光源 VP 矩阵渲染 CSM + Spot shadow maps）──
    // 必须在 GBuffer 之前完成，确保阴影贴图在 Lighting Pass 中可采样
    {
        u32 slot = m_CurrentFrameSlot;
        // 设置阴影渲染资源：Object Buffer + ShadowData Buffer + DescriptorSet
        m_ShadowSystem->SetRenderResources(
            m_ShadowObjBuffers[slot].get(),
            m_ShadowBuffers[slot].get(),
            m_GBufferSet);

        SubsystemContext sctx;
        sctx.world       = &world;
        sctx.sceneGraph  = &sg;
        sctx.camera      = &camera;
        m_ShadowSystem->Update(sctx);  // 收集光源 → 填充 GPUShadowData（光源 VP 矩阵）
    }

    // 导入阴影贴图到 RenderGraph（Shadow pass 写入，Lighting pass 隐式读取）
    ResourceHandle csmMaps[CASCADE_COUNT];
    for (u32 c = 0; c < CASCADE_COUNT; ++c) {
        auto* tex = m_ShadowSystem->GetShadowMap(c);
        if (tex) {
            char name[32];
            snprintf(name, sizeof(name), "CSM_Shadow_C%u", c);
            csmMaps[c] = rg.ImportTexture(name, tex);
        } else {
            csmMaps[c] = kInvalidHandle;
        }
    }
    auto* spotSTex = m_ShadowSystem->GetShadowMap(4);  // Spot 阴影在索引 4
    auto spotShadowHandle = spotSTex ? rg.ImportTexture("SpotShadow", spotSTex) : kInvalidHandle;

    {
        std::vector<PassResource> shadowWrites;
        for (u32 c = 0; c < 3; ++c)
            if (csmMaps[c] != kInvalidHandle)
                shadowWrites.push_back(RG_WRITE(csmMaps[c]));
        if (spotShadowHandle != kInvalidHandle)
            shadowWrites.push_back(RG_WRITE(spotShadowHandle));
        // WAW 假依赖：写入 gbDepth/gbWorldPos 确保 Shadow → GB_Clear 执行顺序
        shadowWrites.push_back(RG_WRITE(gbDepth));
        shadowWrites.push_back(RG_WRITE(gbWorldPos));

        rg.AddPass("Shadow", {}, std::move(shadowWrites),
            [this](rhi::IRHICommandList* c) {
                u32 slot = m_CurrentFrameSlot;
                // 切换到阴影专用 Object Buffer（binding 2），渲染完成后恢复
                m_Device->UpdateDescriptorSet(m_GBufferSet, rhi::kBindingObjectData,
                    rhi::DescriptorType::StorageBuffer,
                    m_ShadowObjBuffers[slot].get());

                m_ShadowSystem->Render(c);  // 使用光源 VP 矩阵渲染所有阴影贴图

                // 恢复场景 Object Buffer 供后续 GBuffer pass 使用
                m_Device->UpdateDescriptorSet(m_GBufferSet, rhi::kBindingObjectData,
                    rhi::DescriptorType::StorageBuffer,
                    m_ObjectBuffers[slot].get());
            });
    }

    // GBuffer 4×MRT + 绘制（委托给 IGBufferRenderer，支持 CPU/GPU 双模式）
    rg.AddPass("GB_Clear", {}, {{gbA, ResourceAccess::Write}, {gbB, ResourceAccess::Write},
        {gbC, ResourceAccess::Write}, {gbVel, ResourceAccess::Write}, {gbWorldPos, ResourceAccess::Write},
        {gbDepth, ResourceAccess::Write}},
        [&](rhi::IRHICommandList* c) {
            // 更新运行时 context
            m_GBufferCtx.objectBuffer = m_ObjectBuffers[m_CurrentFrameSlot].get();
            m_GBufferCtx.prevViewProj = m_PrevViewProj;

            // ── DGC 模式上下文注入（通过 RHI 统一接口）──
            m_DGCEnabled = (cvDGC_Enable != 0)
                && m_Device->IsDGCReady()
                && m_GPUCulling.GetIndirectBuffer()
                && m_GPUCulling.enabled;
            if (m_DGCEnabled) {
                auto& dgcCtx = m_GBufferCtx.dgc;
                dgcCtx.enabled                 = true;
                dgcCtx.indirectCommandsLayout  = m_Device->GetDGCLayout();
                dgcCtx.indirectExecutionSet    = m_Device->GetDGCExecutionSet();
                dgcCtx.preprocessBufferAddr    = m_Device->GetDGCPreprocessAddr();
                dgcCtx.preprocessBufferSize    = m_Device->GetDGCPreprocessSize();
                dgcCtx.maxSequenceCount        = m_Device->GetDGCMaxSequences();
                dgcCtx.sequenceBuffer          = m_GPUCulling.GetIndirectBuffer();
                dgcCtx.countBuffer            = m_GPUCulling.GetDrawCountBuffer();
            } else {
                m_GBufferCtx.dgc = {};  // 重置 DGC 上下文
            }

            m_GBufferRenderer->Render(c, m_GBufferCtx, world, sg, camera);
        });

    // ── 两阶段剔除 Phase 2: 当前帧 Hi-Z 精筛（GBuffer 后，读取当前帧深度）──
    if (m_GPUCulling.useTwoPhase && m_GPUCulling.enabled) {
        // 构建当前帧 Hi-Z 深度金字塔（从 GBuffer 刚写入的深度缓冲下采样）
        rg.AddPass("HiZ_Build",
            {{gbDepth, ResourceAccess::Read}},
            {},
            [&, w, h](rhi::IRHICommandList* c) {
                m_GPUCulling.BuildHiZPyramid(c, w, h);
            });

        // Phase 2: 读取当前帧 Hi-Z，验证 Phase 1 候选 → 输出 IndirectDraw 命令
        rg.AddPass("GPU_Cull_Phase2",
            {{gbDepth, ResourceAccess::Read}},
            {},
            [&](rhi::IRHICommandList* c) {
                // 更新 Phase 2 的深度/Hi-Z 绑定为当前帧 GBuffer 深度
                m_Device->UpdateDescriptorSet(m_GPUCulling.GetPhase2Set(), 3,
                    rhi::DescriptorType::CombinedImageSampler,
                    m_GBufferDepth.get(), m_GPUCulling.GetHiZSampler());
                m_GPUCulling.DispatchPhase2(c, m_Width, m_Height);
            });
    }

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
            // 始终以白色清除（AO=1.0=无遮蔽），enabled 时 SSAO 在上面绘制 AO 结果
            rhi::ClearValue aoClear; aoClear.color[0]=aoClear.color[1]=aoClear.color[2]=aoClear.color[3]=1.0f;
            c->BeginOffscreenPass(m_SSAO.GetAOTexture()->GetNativeHandle(), nullptr, w, h, &aoClear, false);
            if (m_SSAO.enabled) {
                m_SSAO.SetInputs(m_GBufferDepth.get(), m_GBufferB.get());
                m_SSAO.Render(c);
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
    // worldPos 直接从 GBuffer MRT4 读取，不再需要 Camera invViewProj 做深度重建
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
            // GBufferE: worldPos.xyz（直接从 GBuffer 读取世界坐标，无需 invViewProj 重建）
            m_Device->UpdateDescriptorSet(m_LightingSet, 23, rhi::DescriptorType::CombinedImageSampler,
                m_GBufferE.get(), m_PointSampler.get());
            // 深度缓冲区使用点采样（Nearest），避免 Linear 插值在物体边缘混合背景深度
            m_Device->UpdateDescriptorSet(m_LightingSet, 3, rhi::DescriptorType::CombinedImageSampler,
                m_GBufferDepth.get(), m_PointSampler.get());
            if (m_ShadowSystem && m_ShadowSystem->GetShadowMap(0))
                bindTex(4, m_ShadowSystem->GetShadowMap(0));
            // CSM 级联 1/2（绑定 10/11，与 Shader layout 一致）
            if (m_ShadowSystem && m_ShadowSystem->GetShadowMap(1))
                bindTex(10, m_ShadowSystem->GetShadowMap(1));
            if (m_ShadowSystem && m_ShadowSystem->GetShadowMap(2))
                bindTex(11, m_ShadowSystem->GetShadowMap(2));
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
                m_Device->UpdateDescriptorSet(m_LightingSet, rhi::kBindingLightGrid, rhi::DescriptorType::StorageBuffer, m_LightGridBuffer.get());
                // 上传 LightIndexList
                auto& list = m_ClusteredShading.GetLightIndexList();
                if (!m_LightIndexListBuffer || m_LightIndexListBuffer->GetSize() < list.size() * 4) {
                    rhi::BufferDesc d; d.size = std::max<usize>(list.size() * 4, 64); d.usage = rhi::BufferUsage::Storage; d.cpuAccess = true;
                    m_LightIndexListBuffer = m_Device->CreateBuffer(d);
                }
                void* m2 = m_LightIndexListBuffer->Map();
                if (m2) { memcpy(m2, list.data(), list.size() * 4); m_LightIndexListBuffer->Unmap(); }
                m_Device->UpdateDescriptorSet(m_LightingSet, rhi::kBindingLightIndexList, rhi::DescriptorType::StorageBuffer, m_LightIndexListBuffer.get());
                useClustered = 1u;
            }

            c->SetPipeline(m_LightingPSO.get()); c->BindDescriptorSet(rhi::kDescSetPerFrame, m_LightingSet);
            rhi::ClearValue clr{};
            c->BeginOffscreenPass(m_HDRTarget->GetNativeHandle(), m_HDRDepth->GetNativeHandle(), w, h, &clr, false);
            c->SetViewport({0,(float)h,(float)w,-(float)h,0,1});
            c->SetScissor({0,0,w,h});
            // Push constant: 使用 ShaderTypes.slang 统一定义的 DeferredLightingPushConstant
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
            c->Draw(3);
            c->EndOffscreenPass();
        });

    // ── DDGI 前帧 HDR 捕获（将当前 Lighting 输出拷贝到 DDGI，供下帧探针采样真实辐射度）──
    rg.AddPass("DDGI_CaptureHDR",
        {{hdrC, ResourceAccess::Read}},  // 读 HDR 作为拷贝源
        {},                               // 无 RenderGraph 管理的输出
        [&](rhi::IRHICommandList* c) {
            if (m_DDGI.IsEnabled()) {
                m_DDGI.CaptureHDR(c, m_HDRTarget.get());
            }
        });

    // ── AutoExposure（Compute reduction → SSBO，Bloom 之前）──
    rg.AddPass("AutoExposure", {{hdrC, ResourceAccess::Read}}, {},
        [&](rhi::IRHICommandList* c) {
            m_AutoExposure.SetInput(m_HDRTarget.get(), m_HDRSampler.get());
            m_AutoExposure.Render(c);
            // 恢复 graphics pipeline state（compute dispatch 后 m_CurrentRenderPass 为空）
            c->SetPipeline(m_LightingPSO.get());
        },
        RGPassQueue::Compute);  // AsyncCompute: 自动曝光在 Compute 队列执行

    // ── Particle Render（粒子写入 HDR Target，Lighting 之后）──
    for (u32 pid : m_ParticleComponentIDs) {
        rg.AddPass("ParticleRender",
            {{hdrC, ResourceAccess::Read}},
            {{hdrC, ResourceAccess::Write}},
            [this, pid, &camera, w, h](rhi::IRHICommandList* c) {
                // 先设置粒子 PSO（BeginOffscreenPass 需要预绑定 PSO 来创建 RenderPass）
                c->SetPipeline(m_ParticleRenderer.GetRenderPSO());
                c->BeginOffscreenPass(
                    m_HDRTarget->GetNativeHandle(),
                    m_HDRDepth->GetNativeHandle(),
                    w, h, nullptr, false);  // LoadOp::Load 保留 Light 结果
                c->SetViewport({0, (float)h, (float)w, -(float)h, 0, 1});
                c->SetScissor({0, 0, w, h});
                m_ParticleRenderer.Render(c, pid, camera.GetViewProjMatrix(), camera);
                c->EndOffscreenPass();
            });
    }

    // ── 瞬态资源路径验证（r.TransientTest 1 启用，默认关闭）──
    // 创建两个半分辨率瞬态纹理，声明非重叠生命周期并写入 HDRTarget（防死 Pass 裁剪），
    // 验证 ApplyAliasing → CreateTransientTexture → VkImage 缓存 → 双缓冲 Heap 切换 端到端路径
    if (cvTransientTest) {
        rhi::TextureDesc tDesc;
        tDesc.width       = w / 2;     // 半分辨率
        tDesc.height      = h / 2;
        tDesc.format      = rhi::Format::RGBA16_FLOAT;
        tDesc.usage       = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::ShaderResource;
        tDesc.mipLevels   = 1;
        tDesc.arrayLayers = 1;
        tDesc.sampleCount = 1;

        // 两个相同大小的瞬态纹理，非重叠生命周期 → ApplyAliasing 归入同一池
        auto transientA = rg.CreateTexture("TransientTest_A", tDesc);
        auto transientB = rg.CreateTexture("TransientTest_B", tDesc);

        // Pass A: 声明写入 transient_A + 写入 HDRTarget（确保不被 CullDeadPasses 裁剪）
        rg.AddPass("TransientTest_A",
            {{hdrC, ResourceAccess::Read}},  // 读取 HDR 建立与前序 Pass 的依赖
            {{transientA, ResourceAccess::Write}, {hdrC, ResourceAccess::Write}},  // 写入 HDR 连接输出链
            [](rhi::IRHICommandList* c) {
                // 空操作：Pass 存活仅用于触发 transient 纹理创建和别名分析
                // HDRTarget 写入声明仅用于防 CullDeadPasses 裁剪，不产生实际渲染
            });

        // Pass B: 声明写入 transient_B + 写入 HDRTarget（与 A 的 transient 生命周期不重叠）
        rg.AddPass("TransientTest_B",
            {{hdrC, ResourceAccess::Read}},
            {{transientB, ResourceAccess::Write}, {hdrC, ResourceAccess::Write}},
            [](rhi::IRHICommandList* c) {
                // 空操作：transient_B 与 transient_A 同大小/不重叠 → 共享别名池
            });
    }

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

    // LDR 管线：ToneMap → [ColorGrading] → [SMAA | FXAA] → BackBuffer
    // SMAA 与 FXAA 互斥（二选一），均为 LDR 空间后处理抗锯齿终端 Pass
    auto ldrTarget = rg.ImportTexture("LDR", m_LDRTarget.get());
    bool useFXAA  = IsFXAAEnabled();
    bool useSMAA  = IsSMAAEnabled();                                             // SMAA 互斥选项
    bool useColor = m_ColorGrading.IsEnabled() && m_ColorGrading.GetOutput() != nullptr;
    bool useTAA   = (m_AntiAliasing && m_AntiAliasing->IsEnabled());
    bool needLDR  = useFXAA || useSMAA || useColor;  // 任一启用就需要 LDR 中间纹理

    // ToneMap Pass（HDR → LDR，输出到 LDR 或 BackBuffer）
    rg.AddPass("ToneMap",
        {},
        {{needLDR ? ldrTarget : backBuf, ResourceAccess::Write}},
        [this, &camera, useTAA, needLDR, w, h, anyPostActive](rhi::IRHICommandList* c) {
            if (useTAA) {
                m_ToneMap->SetInput(m_AntiAliasing->GetOutputTexture(),
                                    m_AntiAliasing->GetOutputSampler());
            } else if (anyPostActive) {
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
            // 物理相机曝光偏置叠加到自动曝光（EV 偏移 → 曝光倍率）
            float physicalExposure = m_AutoExposure.GetExposure()
                * std::exp2f(camera.exposureBias);
            m_ToneMap->SetExposure(physicalExposure);
            m_ToneMap->PreBind(c);
            if (needLDR) {
                rhi::ClearValue clr{};
                c->BeginOffscreenPass(m_LDRTarget->GetNativeHandle(),
                    m_LDRDummyDepth->GetNativeHandle(), w, h, &clr, false);
                m_ToneMap->Render(c);
                c->EndOffscreenPass();
            } else {
                c->BeginRenderPass(1, rhi::Format::BGRA8_UNORM);
                m_ToneMap->Render(c);
                c->EndRenderPass();
            }
        });

    // ColorGrading Pass（LDR 色彩分级，ToneMap 之后、AA 之前）
    if (useColor) {
        auto cgOut = rg.ImportTexture("CG_Out", m_ColorGrading.GetOutput());
        rg.AddPass("ColorGrading",
            {{ldrTarget, ResourceAccess::Read}},
            {{cgOut, ResourceAccess::Write}},
            [this, w, h](rhi::IRHICommandList* c) {
                m_ColorGrading.SetInput(m_LDRTarget.get(), m_LDRSampler.get());
                c->PipelineBarrier(rhi::PipelineStage::ColorAttachmentOutput, rhi::PipelineStage::FragmentShader,
                    rhi::ResourceState::RenderTarget, rhi::ResourceState::ShaderResource, m_LDRTarget.get());
                m_ColorGrading.PreBind(c);
                rhi::ClearValue clr{};
                c->BeginOffscreenPass(m_ColorGrading.GetOutput()->GetNativeHandle(), nullptr, w, h, &clr, false);
                m_ColorGrading.Render(c);
                c->EndOffscreenPass();
            });
    }

    // SMAA Pass（LDR 空间形态学抗锯齿，ColorGrading 之后、直接写 BackBuffer）
    // 与 FXAA 互斥：SMAA 启用时跳过后面的 FXAA Pass
    if (useSMAA) {
        auto* smaaInput = useColor ? m_ColorGrading.GetOutput() : m_LDRTarget.get();
        auto* smaaSamp  = useColor ? m_ColorGrading.GetOutputSampler() : m_LDRSampler.get();
        rg.AddPass("SMAA",
            {{ldrTarget, ResourceAccess::Read}},
            {{backBuf, ResourceAccess::Write}},
            [this, smaaInput, smaaSamp](rhi::IRHICommandList* c) {
                m_SMAA->SetInput(smaaInput, smaaSamp);
                // Barrier: 输入纹理 RT → SRV（供 SMAA Pass 1 采样）
                c->PipelineBarrier(rhi::PipelineStage::ColorAttachmentOutput,
                                   rhi::PipelineStage::FragmentShader,
                                   rhi::ResourceState::RenderTarget,
                                   rhi::ResourceState::ShaderResource, smaaInput);
                // Pass 1+2（离屏渲染：边缘检测 + 混合权重）
                m_SMAA->Render(c);
                // Pass 3（邻域混合 → BackBuffer）
                c->BeginRenderPass(1, rhi::Format::BGRA8_UNORM);
                m_SMAA->RenderFinalPass(c);
                c->EndRenderPass();
            });
    }
    // FXAA Pass（LDR 空间后处理抗锯齿，仅 SMAA 未启用时执行）
    else if (useFXAA) {
        auto* fxaaInput = useColor ? m_ColorGrading.GetOutput() : m_LDRTarget.get();
        auto* fxaaSamp  = useColor ? m_ColorGrading.GetOutputSampler() : m_LDRSampler.get();
        rg.AddPass("FXAA",
            {{ldrTarget, ResourceAccess::Read}},
            {{backBuf, ResourceAccess::Write}},
            [this, fxaaInput, fxaaSamp](rhi::IRHICommandList* c) {
                m_FXAA->SetInput(fxaaInput, fxaaSamp);
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
