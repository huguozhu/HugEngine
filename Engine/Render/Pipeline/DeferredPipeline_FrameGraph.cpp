#include "Pipeline/DeferredPipeline.h"
#include "GI/GI_IBL.h"
#include "GI/GI_RSM.h"
#include "GI/RSMFrustum.h"   // RSM 光源视锥拟合 + VPL 采样缩放（任务 30 / §9.2-AA）
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
#include "Core/CVar.h"   // 任务 24：投影贴花 CVar（r.Decal.Project）
#include "Core/Assert.h"
#include <cmath>
#include <cstdio>
#include <chrono>   // 步骤 37：Lumen 计算 pass 的 CPU 录制分解
#include <cstring>
#include <algorithm>
#include <string>
#include <unordered_set>
#include "DeferredLighting.vert.spv.h"
#include "DeferredLighting.frag.spv.h"

// CVar: DGC 运行时开关（0=关闭，1=开启，默认关闭以保留传统 ExecuteIndirect 回退）
// 在控制台输入 "r.DGC.Enable 1" 可动态启用
static int32_t cvDGC_Enable = 0;

// CVar: 瞬态资源路径验证开关（与 DeferredPipeline.cpp 中同步）
static int32_t cvTransientTest = 0;  // 瞬态资源路径验证开关（1=启用测试 Pass）
static const char* kCVar_DGC_Enable_Name = "r.DGC.Enable";

// CVar: GBuffer 投影贴花开关（任务 24，默认开启）。
// 关闭后 Deferred 路径不再绘制贴花（贴花卡片已从 GBuffer 排除，见 DeferredPipeline::Initialize），
// 想要对比"投射片 vs 投影"可以切到 Forward 路径（Forward 用卡片）。
static he::CVar<int> cvDecalProject("r.Decal.Project", 1,
    "GBuffer 投影贴花（任务 24）：0=关闭，1=开启（Deferred 路径）");


// 从 DeferredPipeline.cpp 提取 — BuildFrameGraph 渲染图定义

namespace he::render {

// 步骤 29：Lumen 远场光追的独立计时下标。GITimer 有 32 个源位（第 31 个是"公共项 TLAS"），
// Provider 只用到 0..10，故取 30 作为"Lumen 远场"专用位，能在同一套查询池里单独读数。
static constexpr he::u32 kLumenFarFieldTimerIdx = 30u;   // Lumen 远场光追（在计算 pass 内）
static constexpr he::u32 kLumenComputeTimerIdx  = 29u;   // Lumen 计算 pass 整体（SDF 构建 → 辐照度）
static constexpr he::u32 kLumenSdfTimerIdx      = 25u;   // 其中：SDF 构建（mesh 场 + clipmap 注入/洪泛）
static constexpr he::u32 kLumenCacheTimerIdx    = 26u;   // 其中：页表推进 + Card 捕获 + 反馈
static constexpr he::u32 kLumenProbeTimerIdx    = 27u;   // 其中：探针布置/追踪/着色/SH/逐像素辐照度
static constexpr he::u32 kLumenDebugTimerIdx    = 28u;   // 其中：SDF 逐像素追踪可视化（调试视图）

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
    auto gbLightmapKey = gb.lightmapKey;   // 光照图键（任务 31）：uv0.xy + objectIndex
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
    // MeshBatcher 的构建条件有两条：① GPU 模式要靠它填 IndirectDraw 参数；
    // ② **Lumen 的 Mesh SDF 构建需要这份 CPU 侧几何**（步骤 8）——CPU GBuffer 模式下
    //    绘制不走它，但 SDF 仍然要有几何输入，否则距离场队列为空（实测就是这么发现的）。
    const bool lumenNeedsGeometry = m_GIConfig.diffuse.Has(GISourceId::Lumen)
                                 || m_GIConfig.specular.Has(GISourceId::Lumen);
    if (m_GBuffer->GetMode() == GBufferRenderer::Mode::GPU) {
        if (!m_BatchBuilt) { m_MeshBatcher.Build(world, m_ExcludeDecalCards); m_BatchBuilt = true; }
        m_MeshBatcher.FillGPUScene(m_GPUScene);  // 在 Upload 前写入 draw 参数
    } else if (lumenNeedsGeometry && !m_BatchBuilt) {
        m_MeshBatcher.Build(world, m_ExcludeDecalCards);   // 仅供 Lumen 的 SDF 使用
        m_BatchBuilt = true;
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

    // ════════════════════════════════════════════════════════════════════
    // Nanite 模块接入点（§14.4「门控点只有一个」；§14.8 任务 1 = N0）
    //
    // 独立开关的**唯一**门控在这里：关闭 ⇒ 本模块一个 pass 都不注册，帧图与转储与今天
    // 逐位相同（§14.2 不变式 1，也不产生新的每帧 CPU 开销）；开启且模块就绪 ⇒ 模块自注册
    // 它的 pass（任务 3：`Nanite_Cull` + `Nanite_Raster`，只写模块自持缓冲与自建的 1×1 目标）。
    //
    // 【为什么 else 分支不接管既有 GBuffer 写入（与 §14.4 伪码的唯一差异）】
    //   §14.4 的伪码把既有 `GPU_Cull / GB_Clear / ...` 放进 else，那是**任务 4 起**的形态：
    //   等模块的软光栅真的写 GBuffer（不变式 3：几何写入者唯一）时，既有写入路径才该让位。
    //   任务 3 的模块仍**不写** GBuffer；若此刻就让既有 `GB_Clear` 停摆，开启档的 GBuffer 将
    //   整帧无人写（GBuffer 纹理内容未定义）⇒ 画面与转储必然与关闭档不同，直接违反任务 3
    //   的验收「开启 ⇒ 画面不变 / 转储逐位一致」。故当前的接入点写成"模块**追加**注册"，
    //   既有 GBuffer 段在两种档位下都原样执行（下面这段既有代码一行未动）。
    // ════════════════════════════════════════════════════════════════════
    if (m_Nanite.GetSettings().enabled && m_Nanite.IsReady()) {
        NaniteGBufferHandles naniteGB;
        naniteGB.albedo      = gbA;
        naniteGB.normal      = gbB;
        naniteGB.emissive    = gbC;
        naniteGB.velocity    = gbVel;
        naniteGB.worldPos    = gbWorldPos;
        naniteGB.disneyA     = gbDisneyA;
        naniteGB.disneyB     = gbDisneyB;
        naniteGB.lightmapKey = gbLightmapKey;
        naniteGB.depth       = gbDepth;
        m_Nanite.AddPasses(rg, naniteGB);
    } else {
        // 关闭档（或模块未就绪）：一个 pass 都不注册 —— 这就是"开关关闭 ⇒ 逐位一致"的实现。
        // 既有 GBuffer 写入路径在**两种档位下都照常执行**，理由见上面的说明。
    }

    // GBuffer 8×MRT + 绘制（委托给 IGBufferRenderer，支持 CPU/GPU 双模式）
    rg.AddPass("GB_Clear", {}, {{gbA, ResourceAccess::Write}, {gbB, ResourceAccess::Write},
        {gbC, ResourceAccess::Write}, {gbVel, ResourceAccess::Write}, {gbWorldPos, ResourceAccess::Write},
        {gbDisneyA, ResourceAccess::Write}, {gbDisneyB, ResourceAccess::Write},
        {gbLightmapKey, ResourceAccess::Write},
        {gbDepth, ResourceAccess::Write}},
        [&](rhi::IRHICommandList* c) {
            // 更新每帧动态参数
            m_GBuffer->SetObjectBuffer(m_ObjectBuffers[m_CurrentFrameSlot].get());
            m_GBuffer->SetPrevViewProj(m_PrevViewProj);
            // 任务 25：逐实例剔除器 + 当前飞行帧槽位（可见列表/间接命令按槽位分开）
            m_GBuffer->SetInstanceCuller(&m_InstanceCuller, m_CurrentFrameSlot);

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

    // ── GBuffer 投影贴花（任务 24）──
    // 位置：GBuffer 之后、所有"读 albedo/法线"的消费者（GI / Lighting）之前。
    // 依赖：读 gbWorldPos（该像素真实表面点）+ gbDepth（天空判定），写 gbA/gbB（albedo+metallic / normal+roughness）。
    // 说明：写入用的是 **Load** 渲染通道 + per-MRT writeMask（只写 MRT0/1），
    //       所以 GBuffer 其余 6 个通道（emissive/velocity/worldPos/disney/lightmapKey）原样保留。
    if (cvDecalProject.Get() != 0) {
        rg.AddPass("Decal_Project",
            {{gbWorldPos, ResourceAccess::Read}, {gbDepth, ResourceAccess::Read}},
            {{gbA, ResourceAccess::Write}, {gbB, ResourceAccess::Write}},
            [&](rhi::IRHICommandList* c) {
                m_DecalPass.Render(c, world, sg, camera, *m_GBuffer);
            });
    }

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
    // 场景包围盒 → DDGI 探针网格自动拟合（任务 14 / §9.2-K）+ RSM 光源视锥（任务 30 / §9.2-AA ④）
    //   【为什么要拟合】网格是固定参数时，覆盖不到的区域会被 SampleDDGI 的 clamp 变成
    //   **贴边常数外推**（对世界坐标超界的查询，8 个采样坐标全被钳到同一个边界探针）。
    //   实测：默认网格 8×4×8 格距 3 只覆盖 21×9×21 世界单位，场景却是 3720.9×1555.9×2288.2
    //   —— 加上覆盖语义（kGIConfProbeGrid）后 DDGI 贡献从 0.019594 掉到 1.1e-7，
    //   即此前那个贡献**整个**来自那次 clamp。网格必须真的罩住场景。
    //   包围盒每 30 帧重算一次：场景几何很少变，而遍历带变换的网格包围盒不是零成本。
    //   【不要用 m_FrameCounter 当这个计时器】它只在启用异步计算时才自增，普通路径上恒为 0
    //   —— 用它做 `% 30` 判据会变成"每帧都重算"（实测日志每帧一行）。
    //   【为什么 RSM 也要它】RSM 的光源正交视锥此前硬编码 sceneCenter=(0,3,0)/radius=60：
    //   只覆盖场景的 1/60，于是绝大多数接收像素在 RSM 里**找不到邻近 VPL**，`1/d²` 把整项
    //   压到 1e-8（噪声底），而 pass 的成本照付（§9.2-AA ①④）。两个消费者共用同一份包围盒。
    // ============================================================
    if (m_SceneBoundsCountdown == 0u) {
        he::AABB sceneBounds;
        world.ForEach<he::MeshComponent>([&](he::Entity e, he::MeshComponent& mesh) {
            if (auto* tf = world.GetComponent<TransformComponent>(e)) {
                sceneBounds.Expand(mesh.GetBounds().Transform(tf->GetLocalMatrix()));
            }
        });
        if (sceneBounds.IsValid()) m_SceneBounds = sceneBounds;
        m_SceneBoundsCountdown = kSceneBoundsRefreshFrames;
    }
    --m_SceneBoundsCountdown;
    if (m_DDGI.autoFitGrid && m_SceneBounds.IsValid()) {
        m_DDGI.FitGridToBounds(m_SceneBounds.min, m_SceneBounds.max);
    }
    // SSR 的 march 参数按同一份场景尺度推导（任务 32）：默认值（maxDistance=50、thickness=0.1）
    // 是给"1 单位 ≈ 1 米"的世界写的，在 3720 单位宽的 Sponza 上射线走 50 单位就停、命中容差
    // 只有 0.1 单位（600 单位距离处一个像素足迹 ≈0.4 单位）⇒ 几乎不可能命中。与任务 30 的
    // RSM 光锥同一类缺陷：场景尺度假设被写成了常数。
    if (m_SSR.autoScaleMarch && m_SceneBounds.IsValid()) {
        const float diag = glm::length(m_SceneBounds.Size());
        if (diag > 1.0f) {
            m_SSR.maxDistance = diag;                       // 覆盖整个场景对角线
            m_SSR.thickness   = diag * 0.0025f;             // ≈1080p / fov60 下 2~3 个像素足迹
            // 线性回退路径的步长必须**不超过命中容差**，否则射线会直接穿过薄几何
            // （步长 72 单位、容差 11.6 单位时实测：目标立方的反射整片丢失）。
            // 代价是线性回退的射程 = maxSteps × stepSize（Sponza 下约 740 单位）——
            // 默认路径是 Hi-Z 层次 march，它不受这个限制。
            m_SSR.stepSize    = m_SSR.thickness;
            // ── 步数预算（任务 35 / §9.2-AE）──
            // Hi-Z 的层次 march 是**屏幕空间** DDA：每步 2^level 像素。修好"屏幕参数 ≠ 射线
            // 参数"的透视校正之后，射线不再靠错误的深度"蒙"到目标附近，而是老老实实按层级
            // 推进与细化 ⇒ 需要更多步才能覆盖同一段屏幕距离。地面镜解析对照实测（同一场景、
            // 同一套场景尺度参数，参照是线性 march 的 30676 / 13489 个物体色像素）：
            //     64 步  : 红 0 / 绿 0（一个都找不到）
            //     128 步 : 红 17719 / 绿 7971
            //     256 步 : 红 30137 / 绿 15847（与线性参照差 2% / 18%）
            //     600 步 : 红 32579 / 绿 15902
            // 故默认提到 256。注意它同时是**线性回退**路径的迭代上限：射程从
            // 64×11.59≈742 单位变成 256×11.59≈2967 单位（覆盖更远，代价是回退路径的循环次数）。
            if (m_SSR.maxSteps < 256) m_SSR.maxSteps = 256;
        }
    }

    //   - IBL 辐照度：RSM 不可用时的回退（世界空间、视角无关）
    //   - RSM：有方向阴影时优先（单次反弹 VPL，视角无关）
    // ============================================================
    if (m_GIConfig.ShouldRunDDGI()) {
        if (auto* giIBL = dynamic_cast<GI_IBL*>(m_GI.get())) {
            m_DDGI.SetIBL(giIBL->GetIrradianceMap(), giIBL->GetIBLSampler());
        }
    }

    // ============================================================
    // 光源数据收集（CPU → GPULight SSBO + push constant 结构）
    //   【为什么提到这里】RSM 的生成着色器要从 u_Lights 读方向光的颜色与强度来算通量，
    //   而光源缓冲原本是在 Lighting 的 pass 里才填的 —— RSM 排在它前面，只能读到上一帧的
    //   内容（或首帧的未初始化显存）。现在提前到所有消费者之前收集一次，Lighting 侧
    //   按值捕获同一份结果，不再自己再收一遍。
    // ============================================================
    PushConstantData fpc{};
    CollectLights(fpc, world, sg, camera);

    // ============================================================
    // RSM 渲染（两个独立消费方，见下方 rsmNeeded）
    //   - Lighting 的漫反射间接光：层栈含 RSM 时作为单次反弹 VPL
    //   - DDGI 探针的世界辐射度来源（B 路径，视角无关）
    // 必须在 DDGI_Update 之前：探针从 RSM 采样单次反弹辐射度，
    // 替代屏幕 HDR（视锥外采样点被跳过 → 视角相关）
    // ============================================================
    // 【单一真值】本帧 RSM pass 是否真的注册。它是 RSM 相关的**所有**消费方唯一的判据：
    // 喂 DDGI（下方）与绑给 Lighting（LightingInputs）都读它，避免"pass 没跑但描述符
    // 仍绑着真实纹理"——那样采样到的是未初始化显存（§9.2-T）。
    bool rsmPassRegistered = false;
    bool rsmDirLightValid  = false;   // 存在「启用且投影」的方向光（RSM 间接光的适用前提，见下）
    float4x4 rsmLightViewProj(1.0f);   // 光源 VP：喂给 DDGI 时与 RSM pass 同源
    float    rsmHalfExtent = 0.0f;     // 该视锥的正交半宽（世界单位）→ VPL 采样面积（任务 30）
    // 【独立门控】RSM 有两个消费方，任一需要就要渲染（§9.2-F）：
    //   1) Lighting 的漫反射间接光——由 ShouldRunRSM()（层栈含 RSM ∧ rsmIndirect）表达，
    //      与 Forward 侧用的是**同一个谓词**；
    //   2) DDGI 探针的世界辐射度来源（RSM 不可用时才回退 IBL 辐照度）。
    // 此前整段被嵌套在 ShouldRunDDGI() 之内，于是「只勾 RSM、关掉 DDGI」时 RSM **永不注册**，
    // 而 Forward 侧的 ShouldRunRSM() 本来就是独立判据 —— 同一份配置在两套管线下行为不同，
    // 就是"配置说谎"。改为上面的并集：门控谓词与 Provider 侧 NeedsPass 同源。
    const bool rsmNeeded = m_GIConfig.ShouldRunDDGI() || m_GIConfig.ShouldRunRSM();
    if (rsmNeeded && m_RSM && m_ShadowSystem
        && m_ShadowSystem->HasActiveShadows()) {
        // 固定光源视锥（不随相机）：CSM 的 lightViewProj 拟合相机视锥，
        // 视角变化会让 RSM 内容随之变化 → 探针辐射度视角相关。
        // RSM 改用覆盖场景的固定光源视锥，保证探针数据与视角无关。
        // 【任务 30 / §9.2-AA ④】覆盖范围由**场景包围盒**推出（纯几何在 GI/RSMFrustum.h，
        // 有单测）：此前是硬编码的 sceneCenter=(0,3,0) / sceneRadius=60 —— 那等于把 RSM
        // 钉在一个半径 60 的球里，几乎没有接收像素能找到邻近 VPL。
        float3 ldir = float3(0.3f, -1.0f, 0.4f);   // 无方向光时的默认方向
        world.ForEach<he::DirectionalLight>([&](he::Entity, he::DirectionalLight& l) {
            if (l.enabled && l.castShadow) {
                ldir = glm::normalize(l.direction);
                rsmDirLightValid = true;           // RSM 间接光只在有方向光投影时才有意义
            }
        });
        // 场景包围盒还没算出来时（首帧 / 空场景）退回一个覆盖相机附近的保守视锥，
        // 保证"有产出"而不是"零覆盖"。包围盒通常在第 0 帧就算好了。
        const float3 fitMin = m_SceneBounds.IsValid() ? m_SceneBounds.min : (camera.position - float3(50.0f));
        const float3 fitMax = m_SceneBounds.IsValid() ? m_SceneBounds.max : (camera.position + float3(50.0f));
        const auto   rsmFit = FitRSMFrustumToBounds(fitMin, fitMax, ldir);
        if (rsmFit) rsmLightViewProj = rsmFit->viewProj;
        rsmHalfExtent = rsmFit ? rsmFit->halfExtent : 0.0f;
        m_RSM->SetLightViewProj(rsmLightViewProj, m_RSM->GetRSMPositionMap()->GetWidth(),
                                m_ShadowSystem->GetShadowSampler(),
                                rhi::kInvalidSet);
        // 通量计算要读方向光的颜色/强度：本帧的光源缓冲（上面已收集，见 §9.2-AA）
        m_RSM->SetLightBuffer(m_LightBuffers[m_CurrentFrameSlot].get());
        // ── RSM pass：遍历 Provider 注册（Wave 2 推广）──
        // Provider 自报「是否需要本帧的 pass」（层栈含 RSM ∧ 源有效），
        // 帧图只负责按注册顺序建 pass 并注入执行上下文。
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
    }

    // ============================================================
    // RSM 间接光：半分辨率 VPL 求和（任务 16 / B3）
    //   搬出 Lighting 的理由与读数见 GI/RSMIndirect.h 与 RSM_Indirect.frag.slang 的文件头：
    //   16 VPL × 2 张贴图 = 32 次采样，实测在 Lighting 里独占约 0.45 ms（0.882 → 0.433）。
    //   【光源 VP 必须与 RSM pass 用同一个】此前 Lighting 侧用的是
    //   `u_ShadowData[0].lightViewProj[0]`，而那是 **CSM 第 0 级**的 VP（拟合相机视锥），
    //   RSM 贴图却是用上面那个**固定的、覆盖场景的** VP 渲染的 —— 查表 UV 与写入 UV 不同源。
    //   现在把同一个 rsmLightViewProj 交给本 pass，两边天然一致（本轮修复项之一）。
    //   【门控与 rsmPassRegistered 同源】没有 RSM 产出时不能让 Lighting 采样上一帧的绑定，
    //   必须回绑黑色占位（§9.2-T）；没有方向光投影时 RSM 间接光恒为 0，故连 pass 都不注册。
    //   执行顺序：RSM pass 声明的是空输出（贴图由 GI_RSM 自己持有，不在帧图资源表里），
    //   因此对齐只能靠**同队列的注册顺序**——本 pass 必须注册在 RSM 之后、Lighting 之前。
    // ============================================================
    rhi::IRHITexture* rsmIndirectTex = nullptr;
    if (rsmPassRegistered && rsmDirLightValid && m_RSMIndirect.IsReady()) {
        m_RSMIndirect.SetInputs(m_GBuffer->GetDepth(), m_GBuffer->GetWorldPos(), m_GBuffer->GetNormal());
        // 光源类型恒为方向光、阴影强度恒有效：不满足这两条的情形已被上面的门控排除
        // （等价于旧着色器里 `shadowParams.w >= 0.5` / `shadowParams.z <= 0` 两条提前返回）
        // 【VPL 采样缩放】由上面拟合出的光锥半宽推出（GI/RSMFrustum.h，单测锁住尺度不变性）：
        // 每个采样点代表的世界面积随场景尺度平方增长，正好抵消 `1/d²` 的变化 —— 旧的硬编码
        // 常数隐含"半径 60 的场景"，在 3720 单位宽的 Sponza 上把整项压到 1e-8（§9.2-AA ①）。
        const float rsmVplScale = RSMVplScale(rsmHalfExtent, kRSMIndirectRadiusUV,
                                             kRSMIndirectVplCount);
        m_RSMIndirect.SetRSM(m_RSM->GetRSMPositionMap(), m_RSM->GetRSMFluxMap(),
                             m_RSM->GetRSMRadianceMap(),
                             rsmLightViewProj, rsmVplScale,
                             /*shadowType=*/0.0f, /*shadowStrength=*/1.0f,
                             /*lightCount=*/1u);
        auto rsmIndirectH = rg.ImportTexture("RSM_Indirect", m_RSMIndirect.GetOutput());
        const u32 riW = m_RSMIndirect.GetOutputWidth();
        const u32 riH = m_RSMIndirect.GetOutputHeight();
        rg.AddPass("RSM_Indirect",
            {{gbWorldPos, ResourceAccess::Read}, {gbB, ResourceAccess::Read}, {gbDepth, ResourceAccess::Read}},
            {{rsmIndirectH, ResourceAccess::Write}},
            [&, riW, riH](rhi::IRHICommandList* c) {
                m_RSMIndirect.PreBind(c);   // 必须先绑管线：BeginOffscreenPass 靠它推导 RenderPass
                rhi::ClearValue clr{};
                c->BeginOffscreenPass(m_RSMIndirect.GetOutput()->GetNativeHandle(), nullptr, riW, riH, &clr, false);
                m_RSMIndirect.Render(c);
                c->EndOffscreenPass();
            },
            RGPassQueue::Graphics);   // 同队列 ⇒ 排在 RSM 之后、Lighting 之前执行
        rsmIndirectTex = m_RSMIndirect.GetOutput();
    }
    // 喂 DDGI：探针改用 RSM 世界辐射度
    // 【必须与上面的 pass 注册条件一致】RSM 未渲染时把 position/radiance 图交给 DDGI，探针会采到
    // 空数据；且 useRSM 是由这两个成员推导的**闩锁**（只置位、永不清除，§9.2-R）⇒ 一旦漏判就
    // 永久走空 RSM 路径、静默丢掉全部 GI（§11.3.1）。因此这里给出**逐帧明确结论**：
    // 注册了才 SetRSM，没注册就 ClearRSM，绝不"什么都不做"。
    if (m_GIConfig.ShouldRunDDGI()) {
        if (rsmPassRegistered) {
            m_DDGI.SetRSM(m_RSM->GetRSMPositionMap(), m_RSM->GetRSMRadianceMap(), rsmLightViewProj);
        } else {
            m_DDGI.ClearRSM();
        }
    }

    // ============================================================
    // DDGI 探针射线的光追 march（任务 17 / B4 · M5.2-A）
    //   【为什么必须在这里】它是 DDGI 探针更新的**输入**：探针的 SH 投影要读本 pass 写出的
    //   射线辐射度，所以必须排在下面的 DDGI compute 之前（同队列 + 注册顺序即执行顺序）。
    //   【为什么要 AS + 材质纹理也提前】两者都在更靠后的 RT 段里；DDGI 走 march 时它们必须
    //   先就绪，故把 AS_Build 的注册从 RT 段提到这里，门控改成"开了任一 RT 源 **或** DDGI
    //   走 march"——否则"只开 DDGI"的配置里 AS 根本不会构建。
    //   【回退】设备不支持光追（m_RTEnabled 为假）时不注册，DDGI.comp 走原来的 RSM/IBL 路径。
    // ============================================================
    {
        const bool ddgiTraceWanted = m_RTEnabled && m_RTPass && m_GIConfig.ShouldRunDDGI();
        const bool anyRT = m_RTEnabled && m_RTPass && m_GIConfig.AnyRTSource();
        // 【步骤 26】Lumen 的**远场硬件光追**也是加速结构的消费者：它不属于 `AnyRTSource()`
        //（Lumen 不是 RT 效果源），也不是 DDGI。若不加这一项，只开 Lumen 时 AS_Build 根本不会注册
        // ⇒ TLAS 是空的 ⇒ 远场光线一条都命中不了（实测 RT 命中率 0.0%，而 SDF 命中率 93.8%）。
        const bool lumenFarWanted = m_RTEnabled && m_RTPass
                                  && m_GIConfig.diffuse.Has(GISourceId::Lumen);
        if (anyRT || ddgiTraceWanted || lumenFarWanted) {
            // 加速结构（TLAS）：每帧一次，被所有 RT 消费者共享（含本帧的 DDGI march）
            rg.AddPass("AS_Build", {}, {},
                [this, &world, &sg](rhi::IRHICommandList* c) {
                    m_GITimer.Begin(c, GITimer::kCommonItemIdx);
                    m_RTPass->BuildAS(c, world, sg);
                    m_GITimer.End(c, GITimer::kCommonItemIdx);
                });
            // 场景材质纹理（ClosestHit 材质查询）：首帧延迟构建一次（CPU 侧）
            if (!m_SceneMaterialBuilt) {
                if (m_RTPass->BuildSceneMaterialTexture(m_Device, world)) {
                    m_SceneMaterialBuilt = true;
                } else {
                    HE_CORE_WARN("DeferredPipeline: 场景材质纹理构建失败，RT 材质查询不可用");
                }
            }
        }

        if (ddgiTraceWanted && m_DDGI_Trace) {
            const u32 probes = m_DDGI.gridX * m_DDGI.gridY * m_DDGI.gridZ;
            if (m_DDGI_Trace->EnsureCapacity(probes, GI_DDGI::kSamplesPerProbe)) {
                m_DDGI_Trace->SetGrid(m_DDGI.gridOrigin, m_DDGI.gridX, m_DDGI.gridY, m_DDGI.gridZ,
                                      m_DDGI.cellSize);
                // 追踪距离与起始偏移：用"探针网格的实际尺度"而不是固定米数——本场景的
                // 世界单位远大于米（Sponza 包围盒 3720 单位），固定 30m 会只覆盖到探针脚下。
                const float maxDist   = std::max(m_DDGI.cellSize * 4.0f, 1.0f);
                const float stepRatio = 0.4f;   // 与 DDGI.comp 的 stepDist = cellSize*0.4 一致
                m_DDGI_Trace->SetSampling(GI_DDGI::kSamplesPerProbe, maxDist, stepRatio);
                if (auto* giIBL = dynamic_cast<GI_IBL*>(m_GI.get())) {
                    m_DDGI_Trace->SetIBL(giIBL->GetIrradianceMap(), giIBL->GetIBLSampler());
                }
                // 探针更新读它 ⇒ 必须先把"本帧有光追结果"这件事告诉 DDGI（u_Flags.w）
                m_DDGI.SetTracedRadiance(m_DDGI_Trace->GetRadianceBuffer(), GI_DDGI::kSamplesPerProbe);

                rg.AddPass("DDGI_Trace", {}, {},
                    [this, camPos = camera.position, lightCount = fpc.lightCount](rhi::IRHICommandList* c) {
                        RTExecuteContext tctx{};
                        tctx.cameraPos            = camPos;
                        tctx.frameIndex           = m_DiagFrameCounter;
                        tctx.lightBuffer          = m_LightBuffers[m_CurrentFrameSlot].get();
                        tctx.lightCount           = lightCount;
                        tctx.sceneMaterialTex     = m_RTPass->GetSceneMaterialTexture();
                        tctx.sceneTriangleNormals = m_RTPass->GetSceneTriangleNormals();
                        // 本 pass 不读 GBuffer：每条射线是"探针位置 + 球面方向"，与屏幕无关。
                        // 网格与采样数在参数 UBO 里，dispatch 的射线数在 EnsureCapacity 里定。
                        m_DDGI_Trace->Execute(c, m_RTPass->GetTLAS(), tctx);
                    },
                    RGPassQueue::Graphics);   // 与 DDGI 的 compute 同队列 ⇒ 靠注册顺序保证先后
            } else {
                // 缓冲建不出来：明确回退，不能让 DDGI 去读一个没绑的缓冲（§9.2-T）
                m_DDGI.SetTracedRadiance(nullptr, 0);
            }
        } else {
            m_DDGI.SetTracedRadiance(nullptr, 0);   // 无光追：DDGI 走 RSM/IBL 路径
        }

        // ── 步骤 31（L5）：把 DDGI 的输入改为 **Screen Probe 的结果** ──
        // 【一帧延迟】DDGI 段在 Lumen 段**之前**注册，所以这里读到的是上一帧的探针缓冲 —— 这既
        // 避开了循环依赖，也是"用上一帧屏幕信息"的标准做法（与其它 GI 源一致）。
        // `HE_DDGI_INPUT=traced` 可切回自追踪路径，用于 A/B。
        {
            static const bool s_useScreenProbe = []() {
                const char* v = std::getenv("HE_DDGI_INPUT");
                return !(v && std::string(v) == "traced");   // 默认：Screen Probe（计划要求的输入源）
            }();
            LumenProvider* lp = nullptr;
            for (auto& p : m_GIProviders) {
                if (!p->Handles(GISourceId::Lumen)) continue;
                if (auto* l = dynamic_cast<LumenProvider*>(p.get())) { lp = l; break; }
            }
            if (s_useScreenProbe && lp && lp->GetProbeBuffer() && lp->GetCellProbeBuffer()) {
                GI_DDGI::ScreenProbeInput in;
                in.probeBuffer  = lp->GetProbeBuffer();
                in.cellProbeMap = lp->GetCellProbeBuffer();
                in.viewProj     = camera.GetViewProjMatrix();
                in.cellsX       = lp->GetScreenCellsX();
                in.cellsY       = lp->GetScreenCellsY();
                in.width        = w;
                in.height       = h;
                // 距离门限 = 1 个 DDGI 格距：只接受"与本探针落在同一处表面附近"的屏幕探针
                in.maxMatchDistance = m_DDGI.cellSize;
                m_DDGI.SetScreenProbeInput(in);
            } else {
                m_DDGI.ClearScreenProbeInput();
            }
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
        const u32 giIdx = (u32)(&prov - m_GIProviders.data());   // 计时下标（任务 29）
        rg.AddPass(prov->GetName(),
            {{gbA, ResourceAccess::Read}, {gbB, ResourceAccess::Read}, {gbDepth, ResourceAccess::Read}},
            {},
            [&, p = prov.get(), cam = &camera, giIdx](rhi::IRHICommandList* c) {
                m_GITimer.Begin(c, giIdx);
                p->Render(c, GIProviderContext{ &world, &sg, cam, m_CurrentFrameSlot, m_GIConfig.furnaceMode });
                m_GITimer.End(c, giIdx);
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
        const u32 giIdx = (u32)(&prov - m_GIProviders.data());   // 计时下标（任务 29）
        rg.AddPass(prov->GetName(), {}, {{ssaoOut, ResourceAccess::Write}},
            [&, aoW, aoH, p = prov.get(), giIdx, aoCtx = GIProviderContext{ &world, &sg, &camera, m_CurrentFrameSlot, m_GIConfig.furnaceMode }](rhi::IRHICommandList* c) {
                p->PreBind(c);                                  // 绑定该源 pass 的管线状态
                p->SetInputs(m_GBuffer->GetDepth(), m_GBuffer->GetNormal(), m_GBuffer->GetAlbedo());
                rhi::ClearValue aoClear;
                aoClear.color[0]=aoClear.color[1]=aoClear.color[2]=aoClear.color[3]=1.0f;
                c->BeginOffscreenPass(p->GetAOOutput()->GetNativeHandle(), nullptr, aoW, aoH, &aoClear, false);
                m_GITimer.Begin(c, giIdx);
                p->Render(c, aoCtx);
                m_GITimer.End(c, giIdx);
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
        const u32 giIdx = (u32)(&prov - m_GIProviders.data());   // 计时下标（任务 29）
        rg.AddPass(prov->GetName(), {}, {{mainH, ResourceAccess::Write}},
            [&, p = prov.get(), pw, ph, giIdx](rhi::IRHICommandList* c) {
                p->PreBind(c);
                rhi::ClearValue clr{};
                c->BeginOffscreenPass(p->GetSpecularOutput()->GetNativeHandle(), nullptr, pw, ph, &clr, false);
                m_GITimer.Begin(c, giIdx);
                p->Render(c, GIProviderContext{ &world, &sg, &camera, m_CurrentFrameSlot, m_GIConfig.furnaceMode });
                m_GITimer.End(c, giIdx);
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
                [&, p = prov.get(), i, aw, ah, giIdx](rhi::IRHICommandList* c) {
                    p->PreBindAux(c, i);
                    rhi::ClearValue clr{};
                    c->BeginOffscreenPass(p->GetAuxPassOutput(i)->GetNativeHandle(), nullptr, aw, ah, &clr, false);
                    // 步骤 37：附属 pass（降噪/升采样）单独计时 —— 它们此前完全不在读数里
                    m_GITimer.Begin(c, GITimer::kAuxItemBase + giIdx);
                    p->RenderAux(c, i, GIProviderContext{});
                    m_GITimer.End(c, GITimer::kAuxItemBase + giIdx);
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
        const u32 giIdx = (u32)(&prov - m_GIProviders.data());   // 计时下标（任务 29）
        rg.AddPass(prov->GetName(), {}, {{mainH, ResourceAccess::Write}},
            [&, p = prov.get(), pw, ph, giIdx](rhi::IRHICommandList* c) {
                p->PreBind(c);
                rhi::ClearValue clr{};
                c->BeginOffscreenPass(p->GetDiffuseOutput()->GetNativeHandle(), nullptr, pw, ph, &clr, false);
                m_GITimer.Begin(c, giIdx);
                p->Render(c, GIProviderContext{ &world, &sg, &camera, m_CurrentFrameSlot, m_GIConfig.furnaceMode });
                m_GITimer.End(c, giIdx);
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
                [&, p = prov.get(), i, aw, ah, giIdx](rhi::IRHICommandList* c) {
                    p->PreBindAux(c, i);
                    rhi::ClearValue clr{};
                    c->BeginOffscreenPass(p->GetAuxPassOutput(i)->GetNativeHandle(), nullptr, aw, ah, &clr, false);
                    // 步骤 37：附属 pass（降噪/升采样）单独计时 —— 它们此前完全不在读数里
                    m_GITimer.Begin(c, GITimer::kAuxItemBase + giIdx);
                    p->RenderAux(c, i, GIProviderContext{});
                    m_GITimer.End(c, GITimer::kAuxItemBase + giIdx);
                    c->EndOffscreenPass();
                });
            lastOut = auxH;
        }
        ssgiDenoised = lastOut;
        ssgiFinalTex = prov->GetFinalDiffuseOutput();
    }

    // ============================================================
    // Lumen 段（虚拟化几何 GI）：**第 8 条按 Provider 的循环**
    //
    // 与上面 7 条不同，这里按 **Provider** 遍历而不是按 source id：Lumen 的
    // `ToPipelineCap` 同时返回漫反射与镜面两位（一份估计量喂两个通道，与 IBL 同形），
    // 若按 id 分派会被两条循环各跑一遍（双跑）。这正是《Lumen设计与实现》§11 的"逃生口"：
    // 在 PROVIDER-EXEC（任务 19）落地之前，新增源用自己的循环接入，pass 仍只注册一次。
    //
    // 【骨架阶段（步骤 6）】Provider::Render 不绘制任何东西，输出由 clear 决定：
    // 白炉模式清成 1.0（供白炉标度判据），其余清成 0.0（中性，不影响画面）。
    // ============================================================
    bool                lumenProduced = false;
    rhi::IRHITexture*   lumenTex      = nullptr;
    rhi::IRHISampler*   lumenSampler  = nullptr;
    render::ResourceHandle lumenHandle = kInvalidHandle;
    {
        // 通道取"实际要求了 Lumen 的那一个"：同时要求时以漫反射为准（面板同一份输出）
        const GIChannelStack& lumenStack = m_GIConfig.diffuse.Has(GISourceId::Lumen)
                                         ? m_GIConfig.diffuse : m_GIConfig.specular;
        for (auto& prov : m_GIProviders) {
            if (!prov->Handles(GISourceId::Lumen)) continue;
            prov->SyncToStack(lumenStack);
            if (!prov->NeedsPass(lumenStack)) continue;
            prov->SetInputs(m_GBuffer->GetDepth(), m_GBuffer->GetNormal(), m_GBuffer->GetAlbedo());
            if (auto* lp0 = dynamic_cast<LumenProvider*>(prov.get())) lp0->SetWorldPosInput(m_GBuffer->GetWorldPos());
            // 步骤 26：远场硬件光追的输入。TLAS / 场景材质纹理 / 三角形法线都来自共享的 RTPass
            //（TLAS 已在 DDGI 段之前构建；此处只传句柄，不重复构建）。
            if (auto* lp0 = dynamic_cast<LumenProvider*>(prov.get())) {
                PushConstantData ffpc{};
                CollectLights(ffpc, world, sg, camera);
                lp0->SetRTInputs(m_RTPass ? m_RTPass->GetTLAS() : nullptr,
                                 m_RTPass ? m_RTPass->GetSceneMaterialTexture() : nullptr,
                                 m_RTPass ? m_RTPass->GetSceneTriangleNormals() : nullptr,
                                 m_LightBuffers[m_CurrentFrameSlot].get(), ffpc.lightCount);
            }

            rhi::IRHITexture* out = prov->GetDiffuseOutput();
            if (!out) continue;

            // ── 步骤 8：Mesh SDF 构建（独立 compute pass）──
            // 必须**在 offscreen render pass 之外**：Vulkan 不允许在 render pass 内 dispatch
            // compute（实测把 dispatch 放进主 pass 会直接访问违例崩溃）。本 pass 不声明资源
            // 依赖（自持资源 + 自管 barrier），writes 为空故不会被 CullDeadPasses 裁掉。
            const u32 lumenGiIdx = (u32)(&prov - m_GIProviders.data());   // 计时下标（步骤 29 读耗时用）
            // 步骤 32：DDGI 的下标与探针数也在这里取好（lambda 没有默认捕获，不能在里面用 this）
            u32 ddgiGiIdx = 0xFFFFFFFFu, ddgiProbes = 0u;
            for (size_t pi = 0; pi < m_GIProviders.size(); ++pi) {
                if (m_GIProviders[pi]->Handles(GISourceId::DDGI)) { ddgiGiIdx = (u32)pi; break; }
            }
            ddgiProbes = m_DDGI.gridX * m_DDGI.gridY * m_DDGI.gridZ;
            rg.AddPass("Lumen_SDF_Build", {}, {},
                // GBuffer 的三张纹理按值捕获（lambda 没有默认捕获，用 this 会编译失败）
                [p = prov.get(), cam = &camera, furnaceMode = m_GIConfig.furnaceMode,
                 gbN = m_GBuffer->GetNormal(), gbAl = m_GBuffer->GetAlbedo(),
                 gbWP = m_GBuffer->GetWorldPos(), giIdx = lumenGiIdx, timer = &m_GITimer,
                 ddgiIdx = ddgiGiIdx, probeCount = ddgiProbes, self = this](rhi::IRHICommandList* c) {
                    if (auto* lp = dynamic_cast<LumenProvider*>(p)) {
                        // 【步骤 37】把 Lumen 计算 pass 的 **CPU 录制耗时**按步骤拆开。
                        // 实测这个 pass 的 CPU 录制要 16.4 ms（GPU 只有 3.4 ms），不拆开就只能猜；
                        // 只在 HE_CPU_PASSES=1 时累计与打印。
                        static const bool s_cpuSteps = (std::getenv("HE_CPU_PASSES") != nullptr);
                        static double s_cpuStep[12] = {0};
                        auto stepT0 = std::chrono::steady_clock::now();
                        auto mark = [&](u32 i) {
                            if (!s_cpuSteps) return;
                            const auto t = std::chrono::steady_clock::now();
                            s_cpuStep[i] += std::chrono::duration<double, std::milli>(t - stepT0).count();
                            stepT0 = t;
                        };

                        // 步骤 29：整个计算 pass 的 GPU 耗时（SDF 构建 → 页表 → 捕获 → 反馈 → 探针
                        // → 追踪 → 着色 → SH → 辐照度 → 远场光追都在这一个 pass 里）
                        timer->Begin(c, kLumenComputeTimerIdx);
                        timer->Begin(c, kLumenSdfTimerIdx);
                        lp->TransitionStorageImagesOnce(c);   // 步骤 37：首帧转换自持存储图像的布局
                        lp->StepSDF(c, *cam);   // 近层跟随相机（相机位置在第一次 Step 之前给出）
                        timer->End(c, kLumenSdfTimerIdx);
                        mark(0);   // StepSDF
                        timer->Begin(c, kLumenCacheTimerIdx);
                        lp->StepSurfaceCache(c);   // 步骤 14：页表 + 页状态机（GPU 镜像校验）
                        mark(1);   // StepSurfaceCache
                        lp->RunCardCapture(c, *cam);   // 步骤 15：Card 捕获（写 atlas）
                        mark(2);   // RunCardCapture
                        lp->RunFeedback(c, *cam);      // 步骤 16：Feedback（缺失页检测 + 请求写回页表）
                        timer->End(c, kLumenCacheTimerIdx);
                        mark(3);   // RunFeedback
                        timer->Begin(c, kLumenProbeTimerIdx);
                        lp->RunProbePlacement(c);      // 步骤 20：Screen Probe 布置与自适应合并
                        mark(4);   // RunProbePlacement
                        lp->RunProbeTrace(c);          // 步骤 21：探针半球追踪（半球采样 + SDF march）
                        mark(5);   // RunProbeTrace
                        // 步骤 26：远场硬件光追 + 与 SDF 逐光线对照 + **合并**（远场命中写回命中点）。
                        // 必须夹在"追踪"与"着色"之间：合并写回的命中点就是步骤 22 的输入，
                        // 放到着色之后再跑等于白跑（写回的值会在下一帧被追踪覆盖）。
                        // 步骤 29：远场光追单独计时（用 32 个源里靠后的一个保留下标，不影响 Provider 读数）
                        timer->Begin(c, kLumenFarFieldTimerIdx);
                        lp->RunFarFieldRT(c);
                        timer->End(c, kLumenFarFieldTimerIdx);
                        mark(6);   // RunFarFieldRT
                        lp->RunSurfaceCacheShading(c, *cam);   // 步骤 22：命中点着色（材质取自 atlas）
                        mark(7);   // RunSurfaceCacheShading
                        // 步骤 23：SH 投影（白炉下 l0 必须等于 √π —— 用同一面白炉开关驱动）
                        lp->RunScreenProbeSHProject(c, furnaceMode);
                        mark(8);   // SH 投影
                        // 步骤 35：探针滤波（3×3 单元 YCoCg AABB + 时域重投影 EMA）。
                        // 必须夹在 SH 投影与逐像素辐照度之间：后者读的是过滤后的探针缓冲，
                        // 而 DDGI 段（注册在本段之前）读的也是它 —— 按步骤 31 定的"一帧延迟"语义，
                        // DDGI 拿到的是上一帧的过滤结果，正是降噪后的输入。
                        lp->RunProbeFilter(c, *cam);
                        mark(9);   // 探针滤波
                        // 步骤 24：由探针 SH 采样出逐像素辐照度（供 Provider 输出 pass 贴图）
                        lp->RunProbeIrradiance(c, gbN, gbAl, gbWP);
                        timer->End(c, kLumenProbeTimerIdx);
                        mark(10);  // 逐像素辐照度
                        // 步骤 12（L1 退出判据）：SDF 构建完之后，同一 compute pass 里跑一次
                        // 逐像素 sphere tracing 可视化（相机主射线）。放在这里而不是 Lighting
                        // 之后，是因为它只依赖 SDF 本身，与 GBuffer / 合成无关。
                        timer->Begin(c, kLumenDebugTimerIdx);
                        lp->RunSDFDebug(c, *cam);
                        timer->End(c, kLumenDebugTimerIdx);
                        timer->End(c, kLumenComputeTimerIdx);
                        mark(11);  // SDF 调试视图
                        // 步骤 29：Lumen 的 GPU 耗时（滚动平均/峰值）。四段之和 ≈ 计算 pass 整体；
                        // 输出贴图 pass 是光照合成里那次全屏采样（giIdx 计时套的就是它）。
                        //
                        // 【步骤 37：把"启动期"和"稳态"分开】滚动平均是 EMA，跑得越久越接近稳态，
                        // 但 Lumen 的首帧要建 101 个 mesh 场（实测峰值 1996 ms），前几十帧的读数会把
                        // 平均值吊得很高 —— 步骤 29 报的 "计算 pass 27.110 ms" 就是这么来的，
                        // 它**不是**稳态每帧成本。做法：mesh 场建完的那一刻把**平均值**清零
                        // （峰值保留），此后 `AvgMs` 就是稳态 EMA、`PeakMs` 是全程峰值。
                        static u32 s_timingFrames = 0;
                        static bool s_avgReset = false;
                        if (!s_avgReset && lp->IsMeshBuildComplete()) {
                            const float startupPeak = timer->PeakMs(kLumenComputeTimerIdx);
                            timer->ResetAverages();
                            s_avgReset = true;
                            HE_CORE_INFO("Lumen 帧时读数：mesh 场已建完，滚动平均从零重新计（稳态口径）；"
                                         "启动期峰值 {:.3f} ms", (double)startupPeak);
                        }
                        if (++s_timingFrames % 120u == 0u) {
                            HE_CORE_INFO("Lumen GPU 耗时（步骤 29/37）: 计算 pass 平均 {:.3f} ms（稳态口径）"
                                         "/ 峰值 {:.3f} ms（含启动期）；输出贴图 pass 平均 {:.3f} ms",
                                         (double)timer->AvgMs(kLumenComputeTimerIdx),
                                         (double)timer->PeakMs(kLumenComputeTimerIdx),
                                         (double)timer->AvgMs(giIdx));
                            // 步骤 32：DDGI（Radiance Cache）的耗时也一并记录 —— 提高网格分辨率是拿它的
                            // 计算量换伪影幅度，必须两边都能看见。
                            HE_CORE_INFO("   DDGI（Radiance Cache）pass 平均 {:.3f} ms（{} 个探针）",
                                         (ddgiIdx == 0xFFFFFFFFu) ? 0.0 : (double)timer->AvgMs(ddgiIdx),
                                         probeCount);
                            HE_CORE_INFO("   拆分: SDF 构建 {:.3f} ms / 页表+捕获+反馈 {:.3f} ms / "
                                         "探针+追踪+着色+SH+辐照度 {:.3f} ms（其中远场光追 {:.3f} ms，每帧 {} 条射线）/ "
                                         "SDF 调试视图 {:.3f} ms",
                                         (double)timer->AvgMs(kLumenSdfTimerIdx),
                                         (double)timer->AvgMs(kLumenCacheTimerIdx),
                                         (double)timer->AvgMs(kLumenProbeTimerIdx),
                                         (double)timer->AvgMs(kLumenFarFieldTimerIdx), lp->GetFarFieldRays(),
                                         (double)timer->AvgMs(kLumenDebugTimerIdx));
                            // 步骤 37：稳态（mesh 场建完之后）的每帧成本 —— 这才是 L6 帧时该看的数
                            HE_CORE_INFO("   【稳态】SDF 构建 {:.3f} ms（场建完后它只是每帧的注入/更新）；"
                                         "启动期峰值 {:.3f} ms",
                                         (double)timer->AvgMs(kLumenSdfTimerIdx),
                                         (double)timer->PeakMs(kLumenComputeTimerIdx));
                            self->LogFrameBudget();   // 步骤 37：整帧预算（各 pass 合计 + 附属 pass 分解）
                            if (s_cpuSteps) {
                                static const char* kStepNames[12] = {
                                    "StepSDF", "StepSurfaceCache", "CardCapture", "Feedback",
                                    "ProbePlacement", "ProbeTrace", "FarFieldRT", "CacheShading",
                                    "SHProject", "ProbeFilter", "ProbeIrradiance", "SDFDebug"
                                };
                                double sum = 0.0;
                                for (u32 i = 0; i < 12u; ++i) sum += s_cpuStep[i];
                                HE_CORE_INFO("   Lumen 计算 pass 的 CPU 录制分解（每帧，合计 {:.3f} ms）: "
                                             "StepSDF {:.3f} / 页表 {:.3f} / 捕获 {:.3f} / 反馈 {:.3f} / "
                                             "布置 {:.3f} / 追踪 {:.3f} / 远场 {:.3f} / 着色 {:.3f} / "
                                             "SH {:.3f} / 滤波 {:.3f} / 辐照度 {:.3f} / 调试 {:.3f}",
                                             sum / 120.0,
                                             s_cpuStep[0] / 120.0, s_cpuStep[1] / 120.0, s_cpuStep[2] / 120.0,
                                             s_cpuStep[3] / 120.0, s_cpuStep[4] / 120.0, s_cpuStep[5] / 120.0,
                                             s_cpuStep[6] / 120.0, s_cpuStep[7] / 120.0, s_cpuStep[8] / 120.0,
                                             s_cpuStep[9] / 120.0, s_cpuStep[10] / 120.0, s_cpuStep[11] / 120.0);
                                (void)kStepNames;
                                for (u32 i = 0; i < 12u; ++i) s_cpuStep[i] = 0.0;
                            }
                        }
                    }
                });

            const u32 pw = out->GetWidth();
            const u32 ph = out->GetHeight();
            lumenHandle = rg.ImportTexture(prov->GetName(), out);
            const u32 giIdx = lumenGiIdx;   // 计时下标（与步骤 29 的耗时日志同源）
            const GIProviderContext lumenCtx{ &world, &sg, &camera, m_CurrentFrameSlot,
                                              m_GIConfig.furnaceMode };
            rg.AddPass(prov->GetName(),
                {{gbDepth, ResourceAccess::Read}, {gbA, ResourceAccess::Read}, {gbB, ResourceAccess::Read}},
                {{lumenHandle, ResourceAccess::Write}},
                [&, p = prov.get(), lumenCtx, pw, ph, giIdx](rhi::IRHICommandList* c) {
                    rhi::ClearValue clr{};
                    // 白炉：输出 1.0，使「源自身的标度」可被直接读出；否则输出 0（中性占位）
                    const float v = lumenCtx.furnace ? 1.0f : 0.0f;
                    clr.color[0] = clr.color[1] = clr.color[2] = v;
                    clr.color[3] = 1.0f;
                    p->PreBind(c);
                    c->BeginOffscreenPass(p->GetDiffuseOutput()->GetNativeHandle(), nullptr,
                                          pw, ph, &clr, false);
                    m_GITimer.Begin(c, giIdx);
                    p->Render(c, lumenCtx);
                    m_GITimer.End(c, giIdx);
                    c->EndOffscreenPass();
                });

            lumenProduced = true;
            lumenTex      = prov->GetFinalDiffuseOutput();
            if (auto* lumenProv = dynamic_cast<LumenProvider*>(prov.get())) {
                lumenSampler = lumenProv->GetOutputSampler();
            }
        }
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
        // RT 效果需要光源数据（与本帧帧首那次同源；Lighting 的那份按值捕获复用）
        PushConstantData rtfpc{};
        CollectLights(rtfpc, world, sg, camera);

        // 加速结构（TLAS）与场景材质纹理已在 **DDGI 段之前**注册/构建（见那里的说明：
        // DDGI 的光追 march 也要用它们）。此处不再重复注册，否则同一帧会构建两次 TLAS。

        const GIProviderContext rtCtx{ &world, &sg, &camera, m_CurrentFrameSlot,
                                       m_GIConfig.furnaceMode,
                                       m_LightBuffers[m_CurrentFrameSlot].get(), rtfpc.lightCount,
                                       m_RTPass->GetTLAS() };

        for (auto& prov : m_GIProviders) {
            auto* rtp = dynamic_cast<RTEffectProvider*>(prov.get());
            if (!rtp) continue;   // 只处理光追效果源
            rtp->SetRTShadowWanted(m_GIConfig.ShouldRunRTShadow());
            // DDGI 自己也是漫反射层栈的源时，GI 的 miss 分支必须停止回退 DDGI ——
            // 否则 DDGI 信息会以两个槽位的总权重进入归一化合成（§9.2-I）
            rtp->SetDDGIInStack(m_GIConfig.diffuse.Has(GISourceId::DDGI));
            rtp->SetVelocity(m_GBuffer->GetVelocity());
            rtp->SetInputs(m_GBuffer->GetDepth(), m_GBuffer->GetNormal(), m_GBuffer->GetAlbedo());

            // 该效果对应的层栈通道（RT 阴影为独立枚举，不属三通道）
            const GIChannelStack& stack =
                  rtp->IsShadowEffect()                            ? m_GIConfig.diffuse
                : (rtp->GetSourceId() == GISourceId::RTAO)         ? m_GIConfig.ao
                : (rtp->GetSourceId() == GISourceId::RTReflection) ? m_GIConfig.specular
                                                                   : m_GIConfig.diffuse;
            // 与屏幕空间两条循环同一约定：先 SyncToStack 再 NeedsPass —— 这样
            // 「本帧是否真的产出」有唯一落点（步骤 34 的信号登记读的就是它）
            rtp->SyncToStack(stack);
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
            const u32 giIdx = (u32)(&prov - m_GIProviders.data());   // 计时下标（RT 效果源）
            rg.AddPass(prov->GetName(),
                {{gbDepth, ResourceAccess::Read}, {gbB, ResourceAccess::Read}},
                {{mainH, ResourceAccess::UAV}},
                [&, p = prov.get(), rtCtx, giIdx](rhi::IRHICommandList* c) {
                    m_GITimer.Begin(c, giIdx);
                    p->Render(c, rtCtx);
                    m_GITimer.End(c, giIdx);
                });

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
                    [p = prov.get(), i, aw, ah, giIdx, timer = &m_GITimer](rhi::IRHICommandList* c) {
                        p->PreBindAux(c, i);
                        rhi::ClearValue clr{};
                        c->BeginOffscreenPass(p->GetAuxPassOutput(i)->GetNativeHandle(),
                            nullptr, aw, ah, &clr, false);
                        // 步骤 37：附属 pass 单独计时（时域/空间降噪 + 重建升采样）
                        timer->Begin(c, GITimer::kAuxItemBase + giIdx);
                        p->RenderAux(c, i, GIProviderContext{});
                        timer->End(c, GITimer::kAuxItemBase + giIdx);
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
    } else {
        // 本帧没有任何 RT 源在层栈里：把「本帧是否产出」显式清零。
        // 否则步骤 34 的信号登记会残留上一帧的真值 —— RT 关闭后日志仍然报"有 4 条信号"，
        // 而 RG pass 列表里一个 RT pass 都没有（多信号共存的读数就变成假的）。
        for (auto& prov : m_GIProviders) {
            auto* rtp = dynamic_cast<RTEffectProvider*>(prov.get());
            if (!rtp) continue;
            rtp->SetRTShadowWanted(false);
            rtp->SyncToStack(GIChannelStack{});   // 空层栈 ⇒ Has() 恒假 ⇒ 不登记
        }
    }
    // ============================================================
    // 【步骤 34（11.3）】统一降噪信号登记：本帧所有 GI 源在此自报"待降噪信号"
    //
    // 放在这里是因为**所有主 pass / 附属 pass 都已在上面注册完毕**、各家 `SyncToStack`
    // 也已把这个源与层栈对齐 —— 此时 `IsValid()` 的答案才是"本帧真的会产出"，
    // 而不是"配置里写着要"。登记结果用于三件事：
    //   · 多信号共存的一次性视图（`LogSummary`：名字/分辨率/是否需升采样/引导参数）；
    //   · 历史纹理的统一分配账（`DenoiseHistoryPool::LogSummary` 逐条报出占用）；
    //   · 半分辨率信号必须"降噪后再升采样"这一条（`needsUpscale`）。
    // 每帧先 Clear：信号集合是**当帧事实**，不该残留上一帧已关闭的源。
    // ============================================================
    {
        m_DenoiseSignals.Clear();
        rhi::IRHITexture* dnDepth    = m_GBuffer->GetDepth();
        rhi::IRHITexture* dnNormal   = m_GBuffer->GetNormal();
        rhi::IRHITexture* dnVelocity = m_GBuffer->GetVelocity();
        for (auto& prov : m_GIProviders) {
            prov->DescribeSignals(m_DenoiseSignals, dnDepth, dnNormal, dnVelocity);
        }
        m_DenoiseSignals.LogSummary("GI");

        // 历史纹理池的占用账：只在**条数变化**时打印（初始化/尺寸变化各一次），
        // 否则每帧刷屏。池里的纹理是 Acquire 时才建的，故必须等到有人真的取过历史
        // 之后来看，才不会漏报 —— 这正是"信号登记"这一刻。
        static u32 s_LastPoolCount = 0xFFFFFFFFu;
        const u32 poolCount = m_DenoiseHistoryPool.Count();
        if (poolCount != s_LastPoolCount) {
            s_LastPoolCount = poolCount;
            m_DenoiseHistoryPool.LogSummary("HistoryPool");
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
    //
    // 【任务 27 / §9.2-X：这里**不再按消费者门控**】原先的条件是「漫反射栈要它 ∨ 镜面栈要它
    // ∨ DDGI 要它」—— 漏掉了第四个消费者：**BRDF LUT 是 PBR 逐像素无条件采样的**
    // （`DeferredLighting.frag.slang` 里 `u_BRDF_LUT.Sample(...)` 在直接光路径上，与任何层栈
    // 都无关）。于是「IBL 不在任何层栈里」时烘焙不注册 ⇒ 那张 LUT **从未被写入** ⇒ 直接光的
    // BRDF 读到未初始化显存 ⇒ 画面上出现 **463 量级**的亮点（实测：三通道全空时
    // `mean 0.2022 / max 463.32`，位置固定在 (416,996)；一旦有任何镜面源触发烘焙就回到
    // `0.0694 / 42.20`）。这与 §9.2-Q 是同一族缺陷的**第三个实例**：把"谁需要这张图"写成
    // 一份手工清单，就一定会漏掉下一个消费者。
    // 现在恒注册：pass 内部仍按 `IsDirty()` 早退，不脏时它什么也不做（耗时读数恒为 0）。
    for (auto& prov : m_GIProviders) {
        if (!prov->Handles(GISourceId::IBL)) continue;
        prov->SyncToStack(m_GIConfig.diffuse);
        const u32 giIdx = (u32)(&prov - m_GIProviders.data());   // 计时下标（任务 29）
        rg.AddPass("IBL_Bake", {}, {},
            [&, p = prov.get(), giIdx](rhi::IRHICommandList* c) {
                m_GITimer.Begin(c, giIdx);
                p->Render(c, GIProviderContext{ &world, &sg, &camera, m_CurrentFrameSlot, m_GIConfig.furnaceMode });
                m_GITimer.End(c, giIdx);
            });
    }

    // Lighting 读取依赖：仅在对应通道注册了 pass 时才加入（避免无效句柄）
    std::vector<render::PassResource> lightingReads = {
        {gbA, ResourceAccess::Read}, {gbB, ResourceAccess::Read}, {gbC, ResourceAccess::Read},
        {gbWorldPos, ResourceAccess::Read},
        {gbDisneyA, ResourceAccess::Read}, {gbDisneyB, ResourceAccess::Read},
        // 光照图键（任务 31）：Lighting 用它查烘焙光照图，故必须声明读依赖
        {gbLightmapKey, ResourceAccess::Read},
    };
    // 屏幕空间源本帧是否真的产出了内容（= 其 pass 是否注册）。这是**唯一**判据：
    // 它同时决定「声明读取依赖」与「绑给 Lighting 的纹理」，避免两处判断不一致。
    const bool ssgiProduced = (ssgiDenoised != kInvalidHandle);
    const bool ssrProduced  = (ssrDenoised  != kInvalidHandle);
    if (ssgiProduced) {
        lightingReads.push_back({ssgiDenoised, ResourceAccess::Read});
    }
    if (ssrProduced) {
        lightingReads.push_back({ssrDenoised, ResourceAccess::Read});
    }
    // Lumen 输出：本帧产出才声明读依赖（与 SSGI/SSR 同一判据：pass 注册了才算产出）
    if (lumenProduced && lumenHandle != kInvalidHandle) {
        lightingReads.push_back({lumenHandle, ResourceAccess::Read});
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
        // 【必须按值捕获这些局部量】帧图只负责**注册** pass，真正的执行发生在 BuildFrameGraph
        // 返回之后（DeferredPipeline.cpp：BuildFrameGraph → Compile → Execute）。那时本函数的
        // 栈帧已经失效，按引用捕获读到的是垃圾——表现为「本帧没产出的纹理被绑上」（§9.2-T）乃至
        // 指向已销毁纹理的野指针（实测每条运行约 190 条此类报错）。w/h 早已按值捕获，
        // 这里把光照输入的解析结果一并按值捕获。
        [&, w, h,
         ssgiProduced, ssgiFinalTex, ssrProduced, ssrFinalTex, rsmPassRegistered, rsmIndirectTex,
         rtGITex, rtShadowTex, rtAOTex, rtReflectionTex,
         lumenProduced, lumenTex, lumenSampler, fpc](rhi::IRHICommandList* c) {
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

            // 光源数据已在帧图开头收集（RSM 的通量计算也要读，见 §9.2-AA），此处按值捕获复用
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
            in.gbLightmapKey = m_GBuffer->GetLightmapKey();   // 光照图键（任务 31）
            // ── 阴影贴图：本帧**没被写入**的图必须传 nullptr，回落到 LightingPass 预绑的
            // 1×1 占位纹理。传真实纹理会让描述符指向未初始化显存（§9.2-T）：这类采样是
            // 静默的（不报错、画面只是偏暗），读数还随显存布局变化。占位为白色（采样深度 1.0
            // = 无遮挡），正好是该通道的中性值。
            auto shadowMap = [this](u32 index) -> rhi::IRHITexture* {
                if (!m_ShadowSystem || !m_ShadowSystem->WasShadowMapWritten(index)) return nullptr;
                return m_ShadowSystem->GetShadowMap(index);
            };
            in.csmShadow0 = shadowMap(0);
            in.csmShadow1 = shadowMap(1);
            in.csmShadow2 = shadowMap(2);
            in.spotShadow = shadowMap(4);
            in.lightBuffer  = m_LightBuffers[m_CurrentFrameSlot].get();
            in.shadowBuffer = m_ShadowBuffers[m_CurrentFrameSlot].get();
            in.ssaoTex    = m_SSAO.GetAOTexture();
            // 屏幕空间源的最终输出由 Provider 给出（已封装「有降噪取降噪、halfRes 取原始」
            // 的选择），帧图不再重复判断 halfRes——避免两处逻辑不一致。
            // 【门控】没注册 pass 就不能绑它的输出纹理：Provider 持有纹理 ≠ 本帧写过它。
            // ssgiProduced / ssrProduced 是在注册期算好、按值捕获进来的（与 lightingReads
            // 用的是同一个判据），不是在这里重新判断。
            in.ssgiTex     = ssgiProduced
                             ? (ssgiFinalTex ? ssgiFinalTex : m_SSGI.GetIndirectDiffuseTexture())
                             : nullptr;
            in.ssgiSampler = ssgiProduced ? m_SSGI.GetOutputSampler() : nullptr;
            // SSR 同理：未注册 pass 就不绑它的输出纹理
            in.ssrTex      = ssrProduced
                             ? (ssrFinalTex ? ssrFinalTex : m_SSR.GetIndirectSpecularTexture())
                             : nullptr;
            in.ssrSampler  = ssrProduced ? m_SSR.GetOutputSampler() : nullptr;
            // Lumen（binding 32）：本帧没产出就传 nullptr —— LightingPass 会回绑黑色占位
            // （= 无间接光）。传一张没写过的纹理会让描述符"看起来合法"，故障静默（§9.2-T）。
            in.lumenTex     = lumenProduced ? lumenTex : nullptr;
            in.lumenSampler = lumenProduced ? lumenSampler : nullptr;
            in.ddgiProbeBuffer = m_DDGI.GetProbeBuffer();
            in.ddgiGridUniform = m_DDGI.GetGridUniform();
            // RSM 间接光（有 RSM 渲染时喂给 Lighting——shader 内 rsmIndirect 分支）
            // 【与上面的 pass 注册同源】rsmPassRegistered 为假时本帧没有 RSM 内容，
            // 必须传 nullptr 回落到占位纹理；否则描述符指向从未写入的图（§9.2-T）。
            in.rsmPositionMap = rsmPassRegistered ? m_RSM->GetRSMPositionMap() : nullptr;
            in.rsmNormalMap   = rsmPassRegistered ? m_RSM->GetRSMFluxMap()     : nullptr;
            // RSM 间接光 E（半分辨率，任务 16）。与上面两个 RSM 输入用**同一份**门控结论：
            // 本帧没产出就必须传 nullptr 让 LightingPass 回绑黑色占位（§9.2-T）。
            in.rsmIndirectTex = rsmPassRegistered ? rsmIndirectTex : nullptr;
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
                // 逐项写入 UBO 槽位，shader 按 id 分派采样；新增算法无需改动此处。
                // 置信度掩码（confidence）由 GIChannelBlendData::Add 统一推导，此处不填。
                const float edgeFade = m_GIConfig.edgeFade;   // 屏幕覆盖置信度的边缘带宽（§3.2）
                auto fillSlots = [edgeFade](GIChannelBlendData& b, const GIChannelStack& st) {
                    b.count = 0;
                    b.mode  = (u32)st.mode;
                    b.edgeFade = edgeFade;
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
            in.frameSlot = m_CurrentFrameSlot;   // 每飞行帧一份描述符集/UBO（§9.2-J）
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
    // 门控由**消费者声明**（IGIProvider::NeedsRadianceHistory）：写漏一个消费者 ⇒ 它采样到
    // 一张从未写入的纹理、输出恒为 0，而且表面上一切正常（§9.2-Q 的同类缺陷）。
    const bool radianceNeeded = [&] {
        for (auto& prov : m_GIProviders) if (prov->NeedsRadianceHistory()) return true;
        return false;
    }();
    rg.AddPass("CaptureRadiance",
        {{hdrC, ResourceAccess::Read}},  // 读 HDR 作为拷贝源
        {},                               // 无 RenderGraph 管理的输出
        [&, radianceNeeded](rhi::IRHICommandList* c) {
            if (radianceNeeded) {
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

// ============================================================
// 步骤 37：整帧预算的读数（L6 帧时判据）
//
// 为什么需要它：`GITimer` 只给"每个源"的耗时，而 L6 的判据是**整帧**（1080p / 60fps）。
// 两者之间隔着两大块此前完全没有读数：图形侧的各 pass（Shadow / GBuffer / Lighting / 后处理），
// 以及各源的**附属 pass**（降噪 / 重建升采样 —— 步骤 36 又给四种光追效果各加了 4 个全屏 pass）。
// 这里把 `ProfilerManager` 的逐 pass 时间加起来当整帧 GPU 时间，并把附属 pass 单列，
// 于是"离 60fps 还差多少、差在谁身上"可以直接读出来，而不是靠估。
// ============================================================
void DeferredPipeline::LogFrameBudget() {
    const auto& pdata = m_Profiler.GetLastFrameData();
    if (pdata.empty()) return;

    float total = 0.0f;
    std::vector<const ProfilerManager::PassProfile*> sorted;
    sorted.reserve(pdata.size());
    for (const auto& p : pdata) {
        if (p.gpuMs < 0.0f) continue;   // 未使用的 pass
        total += p.gpuMs;
        sorted.push_back(&p);
    }
    if (sorted.empty()) return;
    std::sort(sorted.begin(), sorted.end(),
              [](const auto* a, const auto* b) { return a->gpuMs > b->gpuMs; });

    HE_CORE_INFO("帧预算（步骤 37，1080p）: 各 pass 合计 {:.3f} ms ⇒ 上限 {:.1f} fps；共 {} 个 pass",
                 (double)total, total > 0.0f ? 1000.0 / (double)total : 0.0, (u32)sorted.size());
    const u32 kTop = std::min<u32>(8u, (u32)sorted.size());
    std::string top;
    char buf[96];
    for (u32 i = 0; i < kTop; ++i) {
        std::snprintf(buf, sizeof(buf), "%s %.2f", sorted[i]->name.c_str(), (double)sorted[i]->gpuMs);
        if (!top.empty()) top += " / ";
        top += buf;
    }
    HE_CORE_INFO("   最重的 {} 个: {}", kTop, top);

    // 各源的"附属 pass"（降噪 / 升采样）—— 步骤 37 起才进入读数
    for (size_t i = 0; i < m_GIProviders.size(); ++i) {
        const float aux = m_GITimer.AvgMs(GITimer::kAuxItemBase + (u32)i);
        if (aux > 0.02f) {
            HE_CORE_INFO("   附属 pass（{}）: {:.3f} ms", m_GIProviders[i]->GetName(), (double)aux);
        }
    }
}

} // namespace he::render
