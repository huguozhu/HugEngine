#include "Pipeline/DeferredPipeline.h"
#include "GI/GI_IBL.h"
#include "GI/GI_RSM.h"
#include "Shadow/ShadowSystem.h"
#include "PostProcess/ToneMapPass.h"
#include "PostProcess/SkyboxPass.h"
#include "SceneRenderer.h"
#include "AntiAliasing/AA_TAA.h"
#include "AntiAliasing/AA_FXAA.h"
#include "Scene/CubeComponent.h"
#include "Scene/SphereComponent.h"
#include "Scene/LightComponent.h"
#include "Scene/SkyboxComponent.h"
#include "Scene/PhysicalSkyComponent.h"
#include "Core/Log.h"
#include "Core/Assert.h"
#include <cmath>
#include <cstring>
#include <unordered_set>
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
    he::SyncPhysicalSkyToSun(world);  // 物理天空太阳→方向光同步（阴影/光照收集前）
    if (m_SwapChain) rg.SetSwapChain(m_SwapChain);
    u32 w = m_Width, h = m_Height;
    // 交换链颜色格式（SDR=BGRA8，HDR=A2B10G10R10），同步到 ToneMap 输出格式与 HDR 开关
    rhi::Format swapFmt = m_SwapChain ? m_SwapChain->GetColorFormat() : rhi::Format::BGRA8_UNORM;
    m_PostProcess.GetToneMap()->SetOutputFormat(swapFmt);
    m_PostProcess.GetToneMap()->SetHDREnabled(swapFmt == rhi::Format::A2B10G10R10_UNORM_PACK32);
    // HDR 输出时，LDR 后处理（FXAA/SMAA/ColorGrading/CameraEffects）与 A2B10G10R10 后备缓冲不兼容，需禁用
    bool isHDR = (swapFmt == rhi::Format::A2B10G10R10_UNORM_PACK32);
    // 从 GBufferRenderer 导入所有 GBuffer 纹理
    auto gb = m_GBuffer->ImportToRenderGraph(rg);
    auto gbA = gb.albedo;
    auto gbB = gb.normal;
    auto gbC = gb.emissive;
    auto gbDepth = gb.depth;
    auto gbVel = gb.velocity;
    auto gbWorldPos = gb.worldPos;
    auto gbDisneyA = gb.disneyA;
    auto gbDisneyB = gb.disneyB;
    // Disney BSDF 参数通道;
    auto hdrC = rg.ImportTexture("HDR_C", m_Lighting.GetHDRTarget());
    auto backBuf = rg.ImportBackBuffer();

    (void)world;
    (void)sg;

    // ── 帧首：更新成员变量（lambda 内通过 this 安全访问，无悬垂引用风险）──
    m_CurrViewProj = camera.GetViewProjMatrix();
    static bool firstFrame = true;
    if (firstFrame) { m_PrevViewProj = m_CurrViewProj; firstFrame = false; }
    if (m_PostProcess.GetTAA()) m_PostProcess.GetTAA()->OnBeginFrame();

    // ── 物理相机参数推导（根据 camera.exposureBias 等字段判断是否启用）──
    // 非零 exposureBias 表示使用了物理相机参数，将其传递到 DOF/MotionBlur/Exposure
    if (camera.apertureDiameter > 0.0f) {
        m_PostProcess.GetDOF().SetFocusDepth(camera.focusDistance);
        m_PostProcess.GetDOF().SetIntensity(camera.maxCoC);
    }
    m_PostProcess.GetMotionBlur().SetIntensity(camera.motionBlurIntensity);
    // exposureBias 叠加到 AutoExposure 输出（在 ToneMap Pass 前处理）

    // GPUScene 收集 → [GPU 模式: 填充 IndirectDraw 参数] → 上传
    m_GPUScene.Collect(world, sg, camera);
    if (m_GBuffer->GetMode() == GBufferRenderer::Mode::GPU) {
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
                if (m_GBuffer->GetDepth()) m_GPUCulling.SetDepthTexture(m_Device, m_GBuffer->GetDepth(), w, h);
                m_GPUCulling.DispatchPhase1(c, camera.GetViewProjMatrix(),
                                            m_GPUScene.GetObjectCount(), w, h);
                c->SetPipeline(m_GBuffer->GetPSO());
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
                if (m_GBuffer->GetDepth()) m_GPUCulling.SetDepthTexture(m_Device, m_GBuffer->GetDepth(), w, h);
                if (m_GPUCulling.usePTG) {
                    m_GPUCulling.SignalPTG(c, camera.GetViewProjMatrix(),
                                          m_GPUScene.GetObjectCount(), w, h);
                } else {
                    m_GPUCulling.Dispatch(c, camera.GetViewProjMatrix(),
                                          m_GPUScene.GetObjectCount(), w, h);
                }
                c->SetPipeline(m_GBuffer->GetPSO());
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
            m_GBuffer->GetDescriptorSet());

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
                m_Device->UpdateDescriptorSet(m_GBuffer->GetDescriptorSet(), rhi::kBindingObjectData,
                    rhi::DescriptorType::StorageBuffer,
                    m_ShadowObjBuffers[slot].get());

                m_ShadowSystem->Render(c);  // 使用光源 VP 矩阵渲染所有阴影贴图

                // 恢复场景 Object Buffer 供后续 GBuffer pass 使用
                m_Device->UpdateDescriptorSet(m_GBuffer->GetDescriptorSet(), rhi::kBindingObjectData,
                    rhi::DescriptorType::StorageBuffer,
                    m_ObjectBuffers[slot].get());
            });
    }

    // GBuffer 4×MRT + 绘制（委托给 IGBufferRenderer，支持 CPU/GPU 双模式）
    rg.AddPass("GB_Clear", {}, {{gbA, ResourceAccess::Write}, {gbB, ResourceAccess::Write},
        {gbC, ResourceAccess::Write}, {gbVel, ResourceAccess::Write}, {gbWorldPos, ResourceAccess::Write},
        {gbDisneyA, ResourceAccess::Write}, {gbDisneyB, ResourceAccess::Write},
        {gbDepth, ResourceAccess::Write}},
        [&](rhi::IRHICommandList* c) {
            // 更新每帧动态参数
            m_GBuffer->SetObjectBuffer(m_ObjectBuffers[m_CurrentFrameSlot].get());
            m_GBuffer->SetPrevViewProj(m_PrevViewProj);

            // ── DGC 模式上下文注入（通过 RHI 统一接口）──
            m_DGCEnabled = (cvDGC_Enable != 0)
                && m_Device->IsDGCReady()
                && m_GPUCulling.GetIndirectBuffer()
                && m_GPUCulling.enabled;
            if (m_DGCEnabled) {
                GBufferContext::DGCContext dgcCtx;
                dgcCtx.enabled                 = true;
                dgcCtx.indirectCommandsLayout  = m_Device->GetDGCLayout();
                dgcCtx.indirectExecutionSet    = m_Device->GetDGCExecutionSet();
                dgcCtx.preprocessBufferAddr    = m_Device->GetDGCPreprocessAddr();
                dgcCtx.preprocessBufferSize    = m_Device->GetDGCPreprocessSize();
                dgcCtx.maxSequenceCount        = m_Device->GetDGCMaxSequences();
                dgcCtx.sequenceBuffer          = m_GPUCulling.GetIndirectBuffer();
                dgcCtx.countBuffer            = m_GPUCulling.GetDrawCountBuffer();
                m_GBuffer->SetDGCContext(dgcCtx);
            } else {
                m_GBuffer->ClearDGCContext();
            }

            m_GBuffer->Render(c, world, sg, camera);
        });

    // ── 两阶段剔除 Phase 2 + SSR Hi-Z（GBuffer 后，读取当前帧深度）──
    // Hi-Z 金字塔同时服务 GPUCulling 精筛与 SSR 层次追踪
    bool needHiZ = (m_GPUCulling.useTwoPhase && m_GPUCulling.enabled)
                || m_GIConfig.ShouldRunSpecular();
    if (needHiZ) {
        // 构建当前帧 Hi-Z 深度金字塔（从 GBuffer 刚写入的深度缓冲下采样）
        rg.AddPass("HiZ_Build",
            {{gbDepth, ResourceAccess::Read}},
            {},
            [&, w, h](rhi::IRHICommandList* c) {
                if (m_GBuffer->GetDepth()) {
                    m_GPUCulling.SetDepthTexture(m_Device, m_GBuffer->GetDepth(), w, h);
                }
                m_GPUCulling.BuildHiZPyramid(c, w, h);
                // 喂给 SSR（层次追踪加速）
                if (m_GIConfig.ShouldRunSpecular() && m_GPUCulling.GetHiZTexture()) {
                    m_SSR.SetHiZ(m_GPUCulling.GetHiZTexture(), m_GPUCulling.GetHiZSampler());
                }
            });

        if (m_GPUCulling.useTwoPhase && m_GPUCulling.enabled) {
            // Phase 2: 读取当前帧 Hi-Z，验证 Phase 1 候选 → 输出 IndirectDraw 命令
            rg.AddPass("GPU_Cull_Phase2",
                {{gbDepth, ResourceAccess::Read}},
                {},
                [&](rhi::IRHICommandList* c) {
                    // 更新 Phase 2 的深度/Hi-Z 绑定为当前帧 GBuffer 深度
                    m_Device->UpdateDescriptorSet(m_GPUCulling.GetPhase2Set(), GPUCulling::kPhase2BindHiZ,
                        rhi::DescriptorType::CombinedImageSampler,
                        m_GBuffer->GetDepth(), m_GPUCulling.GetHiZSampler());
                    m_GPUCulling.DispatchPhase2(c, m_Width, m_Height);
                });
        }
    }

    // ============================================================
    // DDGI 探针的辐射度来源绑定
    //   - IBL 辐照度：RSM 不可用时的回退（世界空间、视角无关）
    //   - RSM：有方向阴影时优先（单次反弹 VPL，视角无关）
    // ============================================================
    if (m_GIConfig.ShouldRunDDGI()) {
        if (auto* giIBL = dynamic_cast<GI_IBL*>(m_GI.get())) {
            m_DDGI.SetIBL(giIBL->GetIrradianceMap(), giIBL->GetIBLSampler());
        }
    }

    // ============================================================
    // RSM 渲染（B 路径：DDGI 探针的世界辐射度来源——视角无关）
    // 必须在 DDGI_Update 之前：探针从 RSM 采样单次反弹辐射度，
    // 替代屏幕 HDR（视锥外采样点被跳过 → 视角相关）
    // ============================================================
    if (m_GIConfig.ShouldRunDDGI() && m_RSM && m_ShadowSystem
        && m_ShadowSystem->HasActiveShadows()) {
        // 固定光源视锥（不随相机）：CSM 的 lightViewProj 拟合相机视锥，
        // 视角变化会让 RSM 内容随之变化 → 探针辐射度视角相关。
        // RSM 改用覆盖场景的固定光源视锥，保证探针数据与视角无关。
        float3 ldir = float3(0.3f, -1.0f, 0.4f);   // 无方向光时的默认方向
        world.ForEach<he::DirectionalLight>([&](he::Entity, he::DirectionalLight& l) {
            if (l.enabled && l.castShadow) {
                ldir = glm::normalize(l.direction);
            }
        });
        const float3 sceneCenter = float3(0.0f, 3.0f, 0.0f);   // 场景中心（Sponza）
        const float  sceneRadius = 60.0f;                       // 覆盖半径
        float3 eye = sceneCenter - ldir * sceneRadius * 2.0f;
        float3 up  = (glm::abs(ldir.y) > 0.99f) ? float3(0.0f, 0.0f, 1.0f) : float3(0.0f, 1.0f, 0.0f);
        float4x4 lview = glm::lookAt(eye, sceneCenter, up);
        float4x4 lproj = glm::orthoRH_ZO(-sceneRadius, sceneRadius,
                                          -sceneRadius, sceneRadius,
                                          0.1f, sceneRadius * 4.0f);
        float4x4 lightVP = lproj * lview;
        m_RSM->SetLightViewProj(lightVP, m_RSM->GetRSMPositionMap()->GetWidth(),
                                m_ObjectBuffers[m_CurrentFrameSlot].get(),
                                m_ShadowSystem->GetShadowSampler(),
                                rhi::kInvalidSet);
        // ── RSM pass：遍历 Provider 注册（Wave 2 推广）──
        // Provider 自报「是否需要本帧的 pass」（层栈含 RSM ∧ 源有效），
        // 帧图只负责按注册顺序建 pass 并注入执行上下文。
        bool rsmPassRegistered = false;
        for (auto& prov : m_GIProviders) {
            if (!prov->Handles(GISourceId::RSM)) continue;
            prov->SyncToStack(m_GIConfig.diffuse);
            if (!prov->NeedsPass(m_GIConfig.diffuse)) continue;
            rg.AddPass(prov->GetName(), {}, {},
                [&, p = prov.get()](rhi::IRHICommandList* c) {
                    GIProviderContext ctx{ &world, &sg, &camera, m_CurrentFrameSlot };
                    p->Render(c, ctx);
                });
            rsmPassRegistered = true;
        }
        // 喂 DDGI：探针改用 RSM 世界辐射度
        // 【必须与上面的 pass 注册条件一致】RSM 未入漫反射层栈时本帧**不会渲染** RSM，
        // 若仍把 position/flux 图交给 DDGI，探针会采到空数据；且 m_RSMPositionMap 一旦
        // 置位永不清除 ⇒ useRSM 恒为 1 ⇒ DDGI 永久走空 RSM 路径、静默丢掉全部 GI
        // （表现为 DDGI.comp.slang 的硬编码兜底常数被当成 GI 结果，见 §11.3.1）。
        if (rsmPassRegistered) {
            m_DDGI.SetRSM(m_RSM->GetRSMPositionMap(), m_RSM->GetRSMFluxMap(), lightVP);
        }
    }

    // ============================================================
    // DDGI Probe Update（Compute Shader：必须放在所有 offscreen pass 之前，
    // 避免 compute pipeline 切换影响后续 render pass 状态）
    // Wave 2 阶段 4：改为遍历 Provider（compute 类源，无通道纹理输出）
    // ============================================================
    for (auto& prov : m_GIProviders) {
        if (!prov->Handles(GISourceId::DDGI)) continue;
        prov->SyncToStack(m_GIConfig.diffuse);
        if (!prov->NeedsPass(m_GIConfig.diffuse)) continue;
        prov->SetInputs(m_GBuffer->GetDepth(), m_GBuffer->GetNormal(), m_GBuffer->GetAlbedo());
        if (auto* dp = dynamic_cast<DDGIProvider*>(prov.get())) dp->SetCamera(&camera);
        rg.AddPass(prov->GetName(),
            {{gbA, ResourceAccess::Read}, {gbB, ResourceAccess::Read}, {gbDepth, ResourceAccess::Read}},
            {},
            [&, p = prov.get(), cam = &camera](rhi::IRHICommandList* c) {
                p->Render(c, GIProviderContext{ &world, &sg, cam, m_CurrentFrameSlot });
                c->SetPipeline(m_Lighting.GetPSO());
            },
            RGPassQueue::Graphics);  // 与 RSM 同队列顺序执行：探针采样 RSM 前必须确保 RSM 完成
    }

    // ── AO Pass：遍历已注册的 Provider（P4 / Wave 2）──
    // 替代原先手写的「ShouldRunAO + useGTAO 赋值 + 固定 pass 名」：
    // 只要 Provider 声明处理该通道的源且 NeedsPass 为真，就为它注册 pass。
    // 新增 AO 类算法（如 HBAO/VXAO）只需注册一个新 Provider，此处不再改动。
    for (auto& prov : m_GIProviders) {
        if (!prov->Handles(GISourceId::SSAO) && !prov->Handles(GISourceId::GTAO)) continue;  // 非 AO 通道
        prov->SyncToStack(m_GIConfig.ao);                       // 层栈要求 GTAO → 切 pass 模式
        if (!prov->NeedsPass(m_GIConfig.ao)) continue;
        rhi::IRHITexture* aoTex = prov->GetAOOutput();
        if (!aoTex) continue;
        auto ssaoOut = rg.ImportTexture("AO_Output", aoTex);
        // halfRes：AO 纹理可能为半分辨率，pass 尺寸用纹理实际尺寸
        u32 aoW = aoTex->GetWidth();
        u32 aoH = aoTex->GetHeight();
        rg.AddPass(prov->GetName(), {}, {{ssaoOut, ResourceAccess::Write}},
            [&, aoW, aoH, p = prov.get(), aoCtx = GIProviderContext{ &world, &sg, &camera, m_CurrentFrameSlot }](rhi::IRHICommandList* c) {
                p->PreBind(c);                                  // 绑定该源 pass 的管线状态
                p->SetInputs(m_GBuffer->GetDepth(), m_GBuffer->GetNormal(), m_GBuffer->GetAlbedo());
                rhi::ClearValue aoClear;
                aoClear.color[0]=aoClear.color[1]=aoClear.color[2]=aoClear.color[3]=1.0f;
                c->BeginOffscreenPass(p->GetAOOutput()->GetNativeHandle(), nullptr, aoW, aoH, &aoClear, false);
                p->Render(c, aoCtx);
                c->EndOffscreenPass();
            });
    }

    // ── 屏幕空间反射源：遍历 Provider（主 pass + 附属 pass，与 SSGI 同构）──
    render::ResourceHandle ssrDenoised = kInvalidHandle;   // 通道未启用时保持无效句柄
    rhi::IRHITexture* ssrFinalTex = nullptr;   // Provider 给出的最终输出
    for (auto& prov : m_GIProviders) {
        if (!prov->Handles(GISourceId::SSR)) continue;
        prov->SyncToStack(m_GIConfig.specular);
        if (!prov->NeedsPass(m_GIConfig.specular)) continue;
        prov->SetInputs(m_GBuffer->GetDepth(), m_GBuffer->GetNormal(), m_GBuffer->GetAlbedo());

        rhi::IRHITexture* mainTex = prov->GetSpecularOutput();
        if (!mainTex) continue;
        const u32 pw = mainTex->GetWidth();
        const u32 ph = mainTex->GetHeight();
        const auto mainH = rg.ImportTexture(prov->GetName(), mainTex);
        rg.AddPass(prov->GetName(), {}, {{mainH, ResourceAccess::Write}},
            [&, p = prov.get(), pw, ph](rhi::IRHICommandList* c) {
                p->PreBind(c);
                rhi::ClearValue clr{};
                c->BeginOffscreenPass(p->GetSpecularOutput()->GetNativeHandle(), nullptr, pw, ph, &clr, false);
                p->Render(c, GIProviderContext{ &world, &sg, &camera, m_CurrentFrameSlot });
                c->EndOffscreenPass();
            });

        render::ResourceHandle lastOut = mainH;
        for (u32 i = 0; i < prov->GetAuxPassCount(); i++) {
            rhi::IRHITexture* auxTex = prov->GetAuxPassOutput(i);
            if (!auxTex) continue;
            const auto auxH = rg.ImportTexture(prov->GetAuxPassName(i), auxTex);
            const u32 aw = auxTex->GetWidth();
            const u32 ah = auxTex->GetHeight();
            rg.AddPass(prov->GetAuxPassName(i),
                {{lastOut, ResourceAccess::Read}}, {{auxH, ResourceAccess::Write}},
                [&, p = prov.get(), i, aw, ah](rhi::IRHICommandList* c) {
                    p->PreBindAux(c, i);
                    rhi::ClearValue clr{};
                    c->BeginOffscreenPass(p->GetAuxPassOutput(i)->GetNativeHandle(), nullptr, aw, ah, &clr, false);
                    p->RenderAux(c, i, GIProviderContext{});
                    c->EndOffscreenPass();
                });
            lastOut = auxH;
        }
        ssrDenoised = lastOut;
        ssrFinalTex = prov->GetFinalSpecularOutput();
    }

    // ── 屏幕空间漫反射源：遍历 Provider（主 pass + 附属 pass）──
    // Wave 2 阶段 2：SSGI 的「主 pass + 降噪」链路由 Provider 自报（GetAuxPass*），
    // 帧图不再为其手写降噪 pass；新增屏幕空间 GI 只需注册 Provider。
    render::ResourceHandle ssgiDenoised = kInvalidHandle;  // 通道未启用时保持无效句柄
    rhi::IRHITexture* ssgiFinalTex = nullptr;   // Provider 给出的最终输出（含降噪/半分辨率选择）
    for (auto& prov : m_GIProviders) {
        if (!prov->Handles(GISourceId::SSGI)) continue;
        prov->SyncToStack(m_GIConfig.diffuse);
        if (!prov->NeedsPass(m_GIConfig.diffuse)) continue;
        prov->SetInputs(m_GBuffer->GetDepth(), m_GBuffer->GetNormal(), m_GBuffer->GetAlbedo());

        rhi::IRHITexture* mainTex = prov->GetDiffuseOutput();
        if (!mainTex) continue;
        const u32 pw = mainTex->GetWidth();
        const u32 ph = mainTex->GetHeight();
        const auto mainH = rg.ImportTexture(prov->GetName(), mainTex);
        rg.AddPass(prov->GetName(), {}, {{mainH, ResourceAccess::Write}},
            [&, p = prov.get(), pw, ph](rhi::IRHICommandList* c) {
                p->PreBind(c);
                rhi::ClearValue clr{};
                c->BeginOffscreenPass(p->GetDiffuseOutput()->GetNativeHandle(), nullptr, pw, ph, &clr, false);
                p->Render(c, GIProviderContext{ &world, &sg, &camera, m_CurrentFrameSlot });
                c->EndOffscreenPass();
            });

        // 附属 pass（降噪等）：依次串联，最后一个输出即该源的最终输出
        render::ResourceHandle lastOut = mainH;
        for (u32 i = 0; i < prov->GetAuxPassCount(); i++) {
            rhi::IRHITexture* auxTex = prov->GetAuxPassOutput(i);
            if (!auxTex) continue;
            const auto auxH = rg.ImportTexture(prov->GetAuxPassName(i), auxTex);
            const u32 aw = auxTex->GetWidth();
            const u32 ah = auxTex->GetHeight();
            rg.AddPass(prov->GetAuxPassName(i),
                {{lastOut, ResourceAccess::Read}}, {{auxH, ResourceAccess::Write}},
                [&, p = prov.get(), i, aw, ah](rhi::IRHICommandList* c) {
                    p->PreBindAux(c, i);
                    rhi::ClearValue clr{};
                    c->BeginOffscreenPass(p->GetAuxPassOutput(i)->GetNativeHandle(), nullptr, aw, ah, &clr, false);
                    p->RenderAux(c, i, GIProviderContext{});
                    c->EndOffscreenPass();
                });
            lastOut = auxH;
        }
        ssgiDenoised = lastOut;
        ssgiFinalTex = prov->GetFinalDiffuseOutput();
    }

    // ============================================================
    // RT 效果段（Wave 2 阶段 5：四种 RT 效果共用 Provider，按层栈/阴影枚举启用）
    //   AS_Build（共享前置，不是"源"）→ 各效果主 pass + 时域/空间降噪附属 pass
    //   新增 RT 效果只需注册一个 RTEffectProvider 实例，帧图无需改动
    // ============================================================
    rhi::IRHITexture* rtGITex = nullptr;
    rhi::IRHITexture* rtShadowTex = nullptr;
    rhi::IRHITexture* rtAOTex = nullptr;
    rhi::IRHITexture* rtReflectionTex = nullptr;
    if (m_RTEnabled && m_GIConfig.AnyRTSource() && m_RTPass) {
        // RT 效果需要光源数据（光照缓冲已在帧首填充）
        PushConstantData rtfpc{};
        CollectLights(rtfpc, world, sg, camera);

        // 加速结构（TLAS）构建：每帧一次，由所有 RT 效果共享
        rg.AddPass("AS_Build", {}, {},
            [this, &world, &sg](rhi::IRHICommandList* c) { m_RTPass->BuildAS(c, world, sg); });

        // 场景材质纹理（ClosestHit 材质查询）：首帧延迟构建一次（CPU 侧）
        if (!m_SceneMaterialBuilt) {
            if (m_RTPass->BuildSceneMaterialTexture(m_Device, world)) {
                m_SceneMaterialBuilt = true;
            } else {
                HE_CORE_WARN("DeferredPipeline: 场景材质纹理构建失败，RT 材质查询不可用");
            }
        }

        const GIProviderContext rtCtx{ &world, &sg, &camera, m_CurrentFrameSlot,
                                       m_LightBuffers[m_CurrentFrameSlot].get(), rtfpc.lightCount,
                                       m_RTPass->GetTLAS() };

        for (auto& prov : m_GIProviders) {
            auto* rtp = dynamic_cast<RTEffectProvider*>(prov.get());
            if (!rtp) continue;   // 只处理光追效果源
            rtp->SetRTShadowWanted(m_GIConfig.ShouldRunRTShadow());
            rtp->SetVelocity(m_GBuffer->GetVelocity());
            rtp->SetInputs(m_GBuffer->GetDepth(), m_GBuffer->GetNormal(), m_GBuffer->GetAlbedo());

            // 该效果对应的层栈通道（RT 阴影为独立枚举，不属三通道）
            const GIChannelStack& stack =
                  rtp->IsShadowEffect()                            ? m_GIConfig.diffuse
                : (rtp->GetSourceId() == GISourceId::RTAO)         ? m_GIConfig.ao
                : (rtp->GetSourceId() == GISourceId::RTReflection) ? m_GIConfig.specular
                                                                   : m_GIConfig.diffuse;
            if (!rtp->NeedsPass(stack)) continue;

            rhi::IRHITexture* mainTex =
                  rtp->IsShadowEffect()                            ? rtp->GetShadowOutput()
                : (rtp->GetSourceId() == GISourceId::RTAO)         ? rtp->GetAOOutput()
                : (rtp->GetSourceId() == GISourceId::RTReflection) ? rtp->GetSpecularOutput()
                                                                   : rtp->GetDiffuseOutput();
            if (!mainTex) continue;

            const u32 pw = mainTex->GetWidth();
            const u32 ph = mainTex->GetHeight();
            const auto mainH = rg.ImportTexture(prov->GetName(), mainTex);
            rg.AddPass(prov->GetName(),
                {{gbDepth, ResourceAccess::Read}, {gbB, ResourceAccess::Read}},
                {{mainH, ResourceAccess::UAV}},
                [p = prov.get(), rtCtx](rhi::IRHICommandList* c) { p->Render(c, rtCtx); });

            // 附属 pass（时域累积 → 空间滤波）
            render::ResourceHandle lastOut = mainH;
            for (u32 i = 0; i < rtp->GetAuxPassCount(); i++) {
                rhi::IRHITexture* auxTex = rtp->GetAuxPassOutput(i);
                if (!auxTex) continue;
                const auto auxH = rg.ImportTexture(rtp->GetAuxPassName(i), auxTex);
                const u32 aw = auxTex->GetWidth();
                const u32 ah = auxTex->GetHeight();
                rg.AddPass(rtp->GetAuxPassName(i),
                    {{lastOut, ResourceAccess::Read}, {gbDepth, ResourceAccess::Read},
                     {gbB, ResourceAccess::Read}, {gbVel, ResourceAccess::Read}},
                    {{auxH, ResourceAccess::Write}},
                    [p = prov.get(), i, aw, ah](rhi::IRHICommandList* c) {
                        p->PreBindAux(c, i);
                        rhi::ClearValue clr{};
                        c->BeginOffscreenPass(p->GetAuxPassOutput(i)->GetNativeHandle(),
                            nullptr, aw, ah, &clr, false);
                        p->RenderAux(c, i, GIProviderContext{});
                        c->EndOffscreenPass();
                    });
                lastOut = auxH;
            }

            // 记录各通道最终输出（供 Lighting 采样）
            if (rtp->IsShadowEffect())                               rtShadowTex     = rtp->GetShadowOutput();
            else if (rtp->GetSourceId() == GISourceId::RTAO)         rtAOTex         = rtp->GetFinalAOOutput();
            else if (rtp->GetSourceId() == GISourceId::RTReflection) rtReflectionTex = rtp->GetFinalSpecularOutput();
            else                                                     rtGITex         = rtp->GetFinalDiffuseOutput();
        }
    }
    {
        auto* giIBL = dynamic_cast<GI_IBL*>(m_GI.get());
        if (giIBL) {
            world.ForEach<he::SkyboxComponent>([&](he::Entity, he::SkyboxComponent& sc) {
                if (sc.enabled && sc.GetCubemap()) {
                    giIBL->SetIBLSkybox(sc.GetCubemap(), sc.GetCubemapSampler());
                }
            });
        }
    }

    // Lighting Pass (全屏 PBR + 降噪后 SSGI/SSR/DDGI 读取，委托给 LightingPass 共享组件)

    // 空中透视参数：从物理天空组件读取太阳方向 + 浑浊度（无物理天空时保持 0=关闭）
    float3 atmSunDir = float3(0, 1, 0);
    float atmTurbidity = 0.0f;
    he::GetPhysicalSkySun(world, atmSunDir, atmTurbidity);   // 无条件更新，天空移除时复位浑浊度=0（与 Forward 一致）
    m_Lighting.SetAtmosphere(atmSunDir, atmTurbidity);

    // ── 低频环境源（IBL）烘焙：遍历 Provider ──
    // IBL 无独立 offscreen pass，其辐照度/预滤波贴图由天空盒烘焙而来（脏时重建）；
    // 产物由 Lighting 直接采样，故本 pass 必须排在 Lighting 之前。
    for (auto& prov : m_GIProviders) {
        if (!prov->Handles(GISourceId::IBL)) continue;
        prov->SyncToStack(m_GIConfig.diffuse);
        if (!prov->NeedsPass(m_GIConfig.diffuse)) continue;
        rg.AddPass("IBL_Bake", {}, {},
            [&, p = prov.get()](rhi::IRHICommandList* c) {
                p->Render(c, GIProviderContext{ &world, &sg, &camera, m_CurrentFrameSlot });
            });
    }

    // Lighting 读取依赖：仅在对应通道注册了 pass 时才加入（避免无效句柄）
    std::vector<render::PassResource> lightingReads = {
        {gbA, ResourceAccess::Read}, {gbB, ResourceAccess::Read}, {gbC, ResourceAccess::Read},
        {gbWorldPos, ResourceAccess::Read},
        {gbDisneyA, ResourceAccess::Read}, {gbDisneyB, ResourceAccess::Read},
    };
    if (ssgiDenoised != kInvalidHandle) {
        lightingReads.push_back({ssgiDenoised, ResourceAccess::Read});
    }
    if (ssrDenoised != kInvalidHandle) {
        lightingReads.push_back({ssrDenoised, ResourceAccess::Read});
    }
    // RT GI 纹理（层栈启用 RTGI 时）需声明读取依赖，保证屏障正确
    ResourceHandle rtGILightingHandle = kInvalidHandle;
    if (rtGITex) {
        rtGILightingHandle = rg.ImportTexture("RT_GI_LightingIn", rtGITex);
        lightingReads.push_back({rtGILightingHandle, ResourceAccess::Read});
    }
    rg.AddPass("Lighting",
        lightingReads,
        {{hdrC, ResourceAccess::Write}},
        [&, w, h](rhi::IRHICommandList* c) {
            // IBL 生成（天空盒 → Irradiance/Prefilter/BRDF LUT，脏时才重建）+ 绑定到 Lighting 描述符集
            auto* giIBL = dynamic_cast<GI_IBL*>(m_GI.get());
            if (giIBL) {
                // 烘焙已由帧图主线的 IBL_Bake pass（Provider 遍历）完成，此处只做绑定
                m_Lighting.SetIBLTextures(giIBL->GetIrradianceMap(), giIBL->GetPrefilterMap(),
                                          giIBL->GetBRDF_LUT(), giIBL->GetIBLSampler());
            }

            // GBuffer 深度屏障（在 Lighting 采样前完成 GBuffer 写入的可见性）
            // 注意 srcState 必须是 DepthStencilRead：GBuffer 的 render pass 在创建时把深度附件的
            // finalLayout 声明为 DEPTH_STENCIL_READ_ONLY_OPTIMAL（见 VulkanPipeline.cpp），
            // 因此写入完成后真实布局就是 READ_ONLY；若声明 DepthStencilWrite（ATTACHMENT）
            // 会给出错误的 oldLayout（VUID-VkImageMemoryBarrier-oldLayout-01197，实测每帧触发）
            c->PipelineBarrier(rhi::PipelineStage::LateFragmentTests, rhi::PipelineStage::FragmentShader,
                rhi::ResourceState::DepthStencilRead, rhi::ResourceState::DepthStencilRead,
                m_GBuffer->GetDepth());

            // 收集光源数据
            PushConstantData fpc{}; CollectLights(fpc, world, sg, camera);
            float iblIntensity = m_GI ? m_GI->GetSettings().intensity : 1.0f;

            // 聚集着色：GPU 光源数据 → CPU 缓存
            if (m_ClusteredShading.enabled && fpc.lightCount > 0) {
                m_CachedLights.resize(fpc.lightCount);
                auto* gpuLights = static_cast<const GPULight*>(
                    m_LightBuffers[m_CurrentFrameSlot]->Map());
                if (gpuLights) {
                    memcpy(m_CachedLights.data(), gpuLights, fpc.lightCount * sizeof(GPULight));
                }
                m_LightBuffers[m_CurrentFrameSlot]->Unmap();
            }

            // 委托 LightingPass 执行完整光照（M1.1：LightingInputs 打包输入）
            render::LightingInputs in{};
            in.gbA        = m_GBuffer->GetAlbedo();
            in.gbB        = m_GBuffer->GetNormal();
            in.gbC        = m_GBuffer->GetEmissive();
            in.gbDepth    = m_GBuffer->GetDepth();
            in.gbE        = m_GBuffer->GetWorldPos();
            in.gbDisneyA  = m_GBuffer->GetDisneyA();
            in.gbDisneyB  = m_GBuffer->GetDisneyB();
            in.csmShadow0 = m_ShadowSystem ? m_ShadowSystem->GetShadowMap(0) : nullptr;
            in.csmShadow1 = m_ShadowSystem ? m_ShadowSystem->GetShadowMap(1) : nullptr;
            in.csmShadow2 = m_ShadowSystem ? m_ShadowSystem->GetShadowMap(2) : nullptr;
            in.spotShadow = m_ShadowSystem ? m_ShadowSystem->GetShadowMap(4) : nullptr;
            in.lightBuffer  = m_LightBuffers[m_CurrentFrameSlot].get();
            in.shadowBuffer = m_ShadowBuffers[m_CurrentFrameSlot].get();
            in.ssaoTex    = m_SSAO.GetAOTexture();
            // 屏幕空间源的最终输出由 Provider 给出（已封装「有降噪取降噪、halfRes 取原始」
            // 的选择），帧图不再重复判断 halfRes——避免两处逻辑不一致
            in.ssgiTex     = ssgiFinalTex ? ssgiFinalTex : m_SSGI.GetIndirectDiffuseTexture();
            in.ssgiSampler = m_SSGI.GetOutputSampler();
            in.ssrTex      = ssrFinalTex ? ssrFinalTex : m_SSR.GetIndirectSpecularTexture();
            in.ssrSampler  = m_SSR.GetOutputSampler();
            in.ddgiProbeBuffer = m_DDGI.GetProbeBuffer();
            in.ddgiGridUniform = m_DDGI.GetGridUniform();
            // RSM 间接光（有 RSM 渲染时喂给 Lighting——shader 内 rsmIndirect 分支）
            in.rsmPositionMap = m_RSM ? m_RSM->GetRSMPositionMap() : nullptr;
            in.rsmFluxMap     = m_RSM ? m_RSM->GetRSMFluxMap()     : nullptr;
            // overlay 与 Pass 门控同源（避免 pass 跳过但 shader 仍采样陈旧探针）
            in.clusteredShading     = &m_ClusteredShading;
            in.lightGridBuffer      = m_LightGridBuffer.get();
            in.lightIndexListBuffer = m_LightIndexListBuffer.get();
            in.cachedLights         = &m_CachedLights;
            // RT 纹理暂未使用（保持 nullptr）
            in.cameraPos    = float4(camera.position, 0);
            in.iblIntensity = iblIntensity;
            in.giIntensity  = m_GIConfig.giIntensity;   // M2：间接漫反射总强度（GIConfig 数据驱动）
            in.aoIntensity  = m_GIConfig.aoIntensity;   // M2：AO 强度
            // ── 分层合成（P3）：从各通道层栈派生混合参数（UBO）──
            // 每通道的「屏幕空间源 / 光追源 / 低频环境源」权重取自层栈；
            // 距离让位取对应源的 falloffDistance（0=不启用）
            {
                // Wave 1：层栈直传为「源数组」——每通道的源（IBL/DDGI/SSGI/RSM/RTGI…）
                // 逐项写入 UBO 槽位，shader 按 id 分派采样；新增算法无需改动此处
                auto fillSlots = [](GIChannelBlendData& b, const GIChannelStack& st) {
                    b.count = 0;
                    b.mode  = (u32)st.mode;
                    for (u32 i = 0; i < st.count; i++) {
                        const GISourceDesc& s = st.sources[i];
                        b.Add((u32)s.id, s.weight, s.falloffDistance);
                    }
                };
                fillSlots(in.diffuseBlend,  m_GIConfig.diffuse);
                fillSlots(in.specularBlend, m_GIConfig.specular);
                fillSlots(in.aoBlend,       m_GIConfig.ao);
                // 白炉数值测试（Wave 0.2）：置位后由 shader 代入白炉条件（见 DeferredLighting.frag）
                in.diffuseBlend.furnaceMode = m_GIConfig.furnaceMode ? 1u : 0u;
            }
            // RT 输出（层栈启用对应源且降噪完成时非空 → shader 走光追路径）
            if (rtGITex)        in.rtGI         = rtGITex;
            if (rtShadowTex)    in.rtShadowMask = rtShadowTex;
            if (rtAOTex)        in.rtAO         = rtAOTex;
            if (rtReflectionTex) in.rtReflection = rtReflectionTex;
            in.lightCount   = fpc.lightCount;
            in.width = w;
            in.height = h;
            m_Lighting.Render(c, in);
        });

    // ── Skybox Pass（背景天空盒/物理天空，Lighting 之后合成，depth=Equal 只画无几何处）──
    // 用 LoadOp=Load 保留 Lighting 结果，仅覆盖背景（GBuffer depth == 1.0）
    // 白炉数值测试下**跳过**：白炉判据要求背景与物体亮度都等于环境真值（1.0），
    // 画出真实天空会把背景改写成非 1，破坏读回判据
    rg.AddPass("Skybox",
        {{gbDepth, ResourceAccess::Read}, {hdrC, ResourceAccess::Read}},
        {{hdrC, ResourceAccess::Write}},
        [&, w, h](rhi::IRHICommandList* c) {
            if (m_GIConfig.furnaceMode) return;   // 白炉模式：不画天空（见上）
            SubsystemContext sctx;
            sctx.world = &world;
            sctx.camera = &camera;
            m_PostProcess.GetSkybox()->Update(sctx);
            m_PostProcess.GetSkybox()->PreBind(c);
            c->BeginOffscreenPass(m_Lighting.GetHDRTarget()->GetNativeHandle(),
                                  m_GBuffer->GetDepth()->GetNativeHandle(), w, h, nullptr, false);
            m_PostProcess.GetSkybox()->Render(c);
            c->EndOffscreenPass();
        });

    // ── 前帧 HDR 辐射度捕获（将当前 Lighting 输出下采样存一份，供下帧 GI 源采样真实辐射度）──
    // 从 DDGI 自有一份改为 GI 源共享，避免每个源各付一次全屏下采样（§3.5）。
    // 门控：目前只有 DDGI 消费它，故与 DDGI 是否启用一致（SSGI 接入后再加入其条件）。
    rg.AddPass("DDGI_CaptureHDR",
        {{hdrC, ResourceAccess::Read}},  // 读 HDR 作为拷贝源
        {},                               // 无 RenderGraph 管理的输出
        [&](rhi::IRHICommandList* c) {
            if (m_DDGI.IsEnabled()) {
                m_RadianceHistory.Capture(c, m_Lighting.GetHDRTarget());
            }
        });

    // ── AutoExposure（Compute reduction → SSBO，Bloom 之前）──
    rg.AddPass("AutoExposure", {{hdrC, ResourceAccess::Read}}, {},
        [&](rhi::IRHICommandList* c) {
            m_PostProcess.GetAutoExposure().SetInput(m_Lighting.GetHDRTarget(), m_Lighting.GetHDRSampler());
            m_PostProcess.GetAutoExposure().Render(c);
            // 恢复 graphics pipeline state（compute dispatch 后 m_CurrentRenderPass 为空）
            c->SetPipeline(m_Lighting.GetPSO());
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
                    m_Lighting.GetHDRTarget()->GetNativeHandle(),
                    m_Lighting.GetHDRDepth()->GetNativeHandle(),
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
    bool bloomActive = m_PostProcess.GetBloom().IsEnabled() && m_PostProcess.GetBloom().GetOutput() != nullptr;
    bool dofActive   = m_PostProcess.GetDOF().IsEnabled()   && m_PostProcess.GetDOF().GetOutput()   != nullptr;
    bool mbActive    = m_PostProcess.GetMotionBlur().IsEnabled() && m_PostProcess.GetMotionBlur().GetOutput() != nullptr;
    bool anyPostActive = bloomActive || dofActive || mbActive;

    // Bloom
    if (bloomActive) {
        auto bloomOut = rg.ImportTexture("Bloom_Out", m_PostProcess.GetBloom().GetOutput());
        rg.AddPass("Bloom", {{hdrC, ResourceAccess::Read}}, {{bloomOut, ResourceAccess::Write}},
            [&](rhi::IRHICommandList* c) {
                c->PipelineBarrier(rhi::PipelineStage::ColorAttachmentOutput, rhi::PipelineStage::FragmentShader,
                    rhi::ResourceState::RenderTarget, rhi::ResourceState::ShaderResource, m_Lighting.GetHDRTarget());
                m_PostProcess.GetBloom().SetInput(m_Lighting.GetHDRTarget(), m_Lighting.GetHDRSampler());
                m_PostProcess.GetBloom().Render(c);
            });
    }

    // DOF（景深）：读取 Bloom 输出或原始 HDR
    if (dofActive) {
        auto dofOut = rg.ImportTexture("DOF_Out", m_PostProcess.GetDOF().GetOutput());
        rg.AddPass("DOF", {{hdrC, ResourceAccess::Read}}, {{dofOut, ResourceAccess::Write}},
            [&, bloomActive](rhi::IRHICommandList* c) {
                auto* src = bloomActive ? m_PostProcess.GetBloom().GetOutput() : m_Lighting.GetHDRTarget();
                auto* smp = bloomActive ? m_PostProcess.GetBloom().GetOutputSampler() : m_Lighting.GetHDRSampler();
                m_PostProcess.GetDOF().SetInputs(src, smp, m_GBuffer->GetDepth());
                c->PipelineBarrier(rhi::PipelineStage::ColorAttachmentOutput, rhi::PipelineStage::FragmentShader,
                    rhi::ResourceState::RenderTarget, rhi::ResourceState::ShaderResource, src);
                m_PostProcess.GetDOF().Render(c);
            });
    }

    // MotionBlur：读取 DOF 输出 > Bloom 输出 > 原始 HDR
    if (mbActive) {
        auto mbOut = rg.ImportTexture("MB_Out", m_PostProcess.GetMotionBlur().GetOutput());
        rg.AddPass("MotionBlur", {{hdrC, ResourceAccess::Read}}, {{mbOut, ResourceAccess::Write}},
            [&, bloomActive, dofActive](rhi::IRHICommandList* c) {
                auto* src = dofActive   ? m_PostProcess.GetDOF().GetOutput()
                           : bloomActive ? m_PostProcess.GetBloom().GetOutput()
                           :               m_Lighting.GetHDRTarget();
                auto* smp = dofActive   ? m_PostProcess.GetDOF().GetOutputSampler()
                           : bloomActive ? m_PostProcess.GetBloom().GetOutputSampler()
                           :               m_Lighting.GetHDRSampler();
                m_PostProcess.GetMotionBlur().SetInputs(src, smp, m_GBuffer->GetVelocity(), m_Lighting.GetHDRSampler());
                c->PipelineBarrier(rhi::PipelineStage::ColorAttachmentOutput, rhi::PipelineStage::FragmentShader,
                    rhi::ResourceState::RenderTarget, rhi::ResourceState::ShaderResource, src);
                m_PostProcess.GetMotionBlur().Render(c);
            });
    }

    // TAA Resolve — 读取后处理链最后一个激活 Pass 的输出
    rg.AddPass("TAA_Resolve",
        {{hdrC, ResourceAccess::Read}},
        {},
        [&, bloomActive, dofActive, mbActive, anyPostActive, h = m_Height, w = m_Width](rhi::IRHICommandList* c) {
            if (!m_PostProcess.GetTAA() || !m_PostProcess.GetTAA()->IsEnabled()) return;
            auto* src = mbActive ? m_PostProcess.GetMotionBlur().GetOutput()
                      : dofActive ? m_PostProcess.GetDOF().GetOutput()
                      : bloomActive ? m_PostProcess.GetBloom().GetOutput()
                      : m_Lighting.GetHDRTarget();
            auto* smp = mbActive ? m_PostProcess.GetMotionBlur().GetOutputSampler()
                      : dofActive ? m_PostProcess.GetDOF().GetOutputSampler()
                      : bloomActive ? m_PostProcess.GetBloom().GetOutputSampler()
                      : m_Lighting.GetHDRSampler();
            m_PostProcess.GetTAA()->SetInput(src, smp);
            if (anyPostActive) {
                c->PipelineBarrier(rhi::PipelineStage::ColorAttachmentOutput, rhi::PipelineStage::FragmentShader,
                    rhi::ResourceState::RenderTarget, rhi::ResourceState::ShaderResource, src);
            } else {
                c->PipelineBarrier(rhi::PipelineStage::ColorAttachmentOutput, rhi::PipelineStage::FragmentShader,
                    rhi::ResourceState::RenderTarget, rhi::ResourceState::ShaderResource, m_Lighting.GetHDRTarget());
            }
            auto* taa = static_cast<AA_TAA*>(m_PostProcess.GetTAA());
            taa->SetGBufferInputs(m_GBuffer->GetDepth(), m_GBuffer->GetNormal(), m_GBuffer->GetVelocity());
            float4x4 invCurrVP = glm::inverse(m_CurrViewProj);
            taa->UpdateUniforms(m_PrevViewProj, invCurrVP, m_Width, m_Height);
            m_PostProcess.GetTAA()->Render(c);
        });

    // LDR 管线：ToneMap → [ColorGrading] → [SMAA | FXAA] → BackBuffer
    // SMAA 与 FXAA 互斥（二选一），均为 LDR 空间后处理抗锯齿终端 Pass
    auto ldrTarget = rg.ImportTexture("LDR", m_PostProcess.GetLDRTarget());
    bool useFXAA  = IsFXAAEnabled();
    bool useSMAA  = IsSMAAEnabled();                                             // SMAA 互斥选项
    bool useColor = m_PostProcess.GetColorGrading().IsEnabled() && m_PostProcess.GetColorGrading().GetOutput() != nullptr;
    bool useFX    = m_PostProcess.GetCameraEffects().IsEnabled() && m_PostProcess.GetCameraEffects().GetOutput() != nullptr;
    bool useTAA   = (m_PostProcess.GetTAA() && m_PostProcess.GetTAA()->IsEnabled());
    // HDR 与 LDR 后处理互斥：FXAA/SMAA/ColorGrading/CameraEffects 都是 BGRA8 LDR 空间，
    // 与 A2B10G10R10 后备缓冲格式不匹配，HDR 下强制关闭（TAA 在 HDR 空间，不受影响）
    if (isHDR) {
        useFXAA = useSMAA = useColor = useFX = false;
    }
    bool needLDR  = useFXAA || useSMAA || useColor || useFX;  // 任一 LDR 后处理启用就需要 LDR 中间纹理

    // ToneMap Pass（HDR → LDR，输出到 LDR 或 BackBuffer）
    rg.AddPass("ToneMap",
        {},
        {{needLDR ? ldrTarget : backBuf, ResourceAccess::Write}},
        [this, &camera, useTAA, needLDR, w, h, anyPostActive, swapFmt](rhi::IRHICommandList* c) {
            if (useTAA) {
                m_PostProcess.GetToneMap()->SetInput(m_PostProcess.GetTAA()->GetOutputTexture(),
                                    m_PostProcess.GetTAA()->GetOutputSampler());
            } else if (anyPostActive) {
                auto* src = m_PostProcess.GetMotionBlur().IsEnabled() ? m_PostProcess.GetMotionBlur().GetOutput()
                          : m_PostProcess.GetDOF().IsEnabled()        ? m_PostProcess.GetDOF().GetOutput()
                          :                            m_PostProcess.GetBloom().GetOutput();
                auto* smp = m_PostProcess.GetMotionBlur().IsEnabled() ? m_PostProcess.GetMotionBlur().GetOutputSampler()
                          : m_PostProcess.GetDOF().IsEnabled()        ? m_PostProcess.GetDOF().GetOutputSampler()
                          :                            m_PostProcess.GetBloom().GetOutputSampler();
                m_PostProcess.GetToneMap()->SetInput(src, smp);
            } else {
                c->PipelineBarrier(rhi::PipelineStage::ColorAttachmentOutput, rhi::PipelineStage::FragmentShader,
                    rhi::ResourceState::RenderTarget, rhi::ResourceState::ShaderResource, m_Lighting.GetHDRTarget());
                m_PostProcess.GetToneMap()->SetInput(m_Lighting.GetHDRTarget(), m_Lighting.GetHDRSampler());
            }
            // 物理相机曝光偏置叠加到自动曝光（EV 偏移 → 曝光倍率）
            float physicalExposure = m_PostProcess.GetAutoExposure().GetExposure()
                * std::exp2f(camera.exposureBias);
            m_PostProcess.GetToneMap()->SetExposure(physicalExposure);
            m_PostProcess.GetToneMap()->PreBind(c);
            if (needLDR) {
                rhi::ClearValue clr{};
                c->BeginOffscreenPass(m_PostProcess.GetLDRTarget()->GetNativeHandle(),
                    m_PostProcess.GetLDRDummyDepth()->GetNativeHandle(), w, h, &clr, false);
                m_PostProcess.GetToneMap()->Render(c);
                c->EndOffscreenPass();
            } else {
                c->BeginRenderPass(1, swapFmt);
                m_PostProcess.GetToneMap()->Render(c);
                c->EndRenderPass();
            }
        });

    // ColorGrading Pass（LDR 色彩分级，ToneMap 之后、AA 之前）
    if (useColor) {
        auto cgOut = rg.ImportTexture("CG_Out", m_PostProcess.GetColorGrading().GetOutput());
        rg.AddPass("ColorGrading",
            {{ldrTarget, ResourceAccess::Read}},
            {{cgOut, ResourceAccess::Write}},
            [this, w, h](rhi::IRHICommandList* c) {
                m_PostProcess.GetColorGrading().SetInput(m_PostProcess.GetLDRTarget(), m_PostProcess.GetLDRSampler());
                c->PipelineBarrier(rhi::PipelineStage::ColorAttachmentOutput, rhi::PipelineStage::FragmentShader,
                    rhi::ResourceState::RenderTarget, rhi::ResourceState::ShaderResource, m_PostProcess.GetLDRTarget());
                m_PostProcess.GetColorGrading().PreBind(c);
                rhi::ClearValue clr{};
                c->BeginOffscreenPass(m_PostProcess.GetColorGrading().GetOutput()->GetNativeHandle(), nullptr, w, h, &clr, false);
                m_PostProcess.GetColorGrading().Render(c);
                c->EndOffscreenPass();
            });
    }

    // CameraEffects Pass（LDR 镜头后处理，ColorGrading 之后、AA 之前）
    if (useFX) {
        auto fxOut = rg.ImportTexture("FX_Out", m_PostProcess.GetCameraEffects().GetOutput());
        rg.AddPass("CameraEffects",
            {{ldrTarget, ResourceAccess::Read}},
            {{fxOut, ResourceAccess::Write}},
            [this, useColor, w, h](rhi::IRHICommandList* c) {
                auto* fxIn = useColor ? m_PostProcess.GetColorGrading().GetOutput() : m_PostProcess.GetLDRTarget();
                auto* fxSp = useColor ? m_PostProcess.GetColorGrading().GetOutputSampler() : m_PostProcess.GetLDRSampler();
                m_PostProcess.GetCameraEffects().SetInput(fxIn, fxSp);
                c->PipelineBarrier(rhi::PipelineStage::ColorAttachmentOutput, rhi::PipelineStage::FragmentShader,
                    rhi::ResourceState::RenderTarget, rhi::ResourceState::ShaderResource, fxIn);
                m_PostProcess.GetCameraEffects().PreBind(c);
                rhi::ClearValue clr{};
                c->BeginOffscreenPass(m_PostProcess.GetCameraEffects().GetOutput()->GetNativeHandle(), nullptr, w, h, &clr, false);
                m_PostProcess.GetCameraEffects().Render(c);
                c->EndOffscreenPass();
            });
    }

    // SMAA Pass（LDR 空间形态学抗锯齿，ColorGrading 之后、直接写 BackBuffer）
    // 与 FXAA 互斥：SMAA 启用时跳过后面的 FXAA Pass
    if (useSMAA) {
        auto* smaaInput = useFX    ? m_PostProcess.GetCameraEffects().GetOutput()
                        : useColor ? m_PostProcess.GetColorGrading().GetOutput()
                        :           m_PostProcess.GetLDRTarget();
        auto* smaaSamp  = useFX    ? m_PostProcess.GetCameraEffects().GetOutputSampler()
                        : useColor ? m_PostProcess.GetColorGrading().GetOutputSampler()
                        :           m_PostProcess.GetLDRSampler();
        rg.AddPass("SMAA",
            {{ldrTarget, ResourceAccess::Read}},
            {{backBuf, ResourceAccess::Write}},
            [this, smaaInput, smaaSamp, swapFmt](rhi::IRHICommandList* c) {
                m_PostProcess.GetSMAA()->SetInput(smaaInput, smaaSamp);
                // Barrier: 输入纹理 RT → SRV（供 SMAA Pass 1 采样）
                c->PipelineBarrier(rhi::PipelineStage::ColorAttachmentOutput,
                                   rhi::PipelineStage::FragmentShader,
                                   rhi::ResourceState::RenderTarget,
                                   rhi::ResourceState::ShaderResource, smaaInput);
                // Pass 1+2（离屏渲染：边缘检测 + 混合权重）
                m_PostProcess.GetSMAA()->Render(c);
                // Pass 3（邻域混合 → BackBuffer）
                c->BeginRenderPass(1, swapFmt);
                m_PostProcess.GetSMAA()->RenderFinalPass(c);
                c->EndRenderPass();
            });
    }
    // FXAA Pass（LDR 空间后处理抗锯齿，仅 SMAA 未启用时执行）
    else if (useFXAA) {
        auto* fxaaInput = useFX    ? m_PostProcess.GetCameraEffects().GetOutput()
                        : useColor ? m_PostProcess.GetColorGrading().GetOutput()
                        :           m_PostProcess.GetLDRTarget();
        auto* fxaaSamp  = useFX    ? m_PostProcess.GetCameraEffects().GetOutputSampler()
                        : useColor ? m_PostProcess.GetColorGrading().GetOutputSampler()
                        :           m_PostProcess.GetLDRSampler();
        rg.AddPass("FXAA",
            {{ldrTarget, ResourceAccess::Read}},
            {{backBuf, ResourceAccess::Write}},
            [this, fxaaInput, fxaaSamp, swapFmt](rhi::IRHICommandList* c) {
                m_PostProcess.GetFXAA()->SetInput(fxaaInput, fxaaSamp);
                c->PipelineBarrier(rhi::PipelineStage::ColorAttachmentOutput,
                                   rhi::PipelineStage::FragmentShader,
                                   rhi::ResourceState::RenderTarget,
                                   rhi::ResourceState::ShaderResource, fxaaInput);
                c->BeginRenderPass(1, swapFmt);
                m_PostProcess.GetFXAA()->Render(c);
                c->EndRenderPass();
            });
    }

    // ── 帧末：保存当前帧 VP 供下一帧使用 ──
    m_PrevViewProj = m_CurrViewProj;
}

} // namespace he::render
