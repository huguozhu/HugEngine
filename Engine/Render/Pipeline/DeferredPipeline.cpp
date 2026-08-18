#include "Pipeline/DeferredPipeline.h"
#include "GI/GI_IBL.h"
#include "GI/GI_RSM.h"
#include "Shadow/ShadowSystem.h"
#include "PostProcess/ToneMapPass.h"
#include "PostProcess/SkyboxPass.h"
#include "SceneRenderer.h"
#include "AntiAliasing/AA_TAA.h"
#include "AntiAliasing/AA_FXAA.h"
#include "Pipeline/PhysicalLight.h"

#include "Scene/CubeComponent.h"
#include "Scene/SphereComponent.h"
#include "Scene/LightComponent.h"
#include "Core/Log.h"
#include "Core/Assert.h"
#include <cstring>
#include <cmath>
#include <unordered_set>
#include "Fullscreen.vert.spv.h"
#include "FullscreenCopy.frag.spv.h"

// CVar: DGC 运行时开关（0=关闭，1=开启，默认关闭以保留传统 ExecuteIndirect 回退）
// 在控制台输入 "r.DGC.Enable 1" 可动态启用
static int32_t cvDGC_Enable = 0;
static const char* kCVar_DGC_Enable_Name = "r.DGC.Enable";

// CVar: 瞬态资源路径验证开关（0=关闭，1=开启，默认关闭）
// 在控制台输入 "r.TransientTest 1" 可启用，验证 Transient Allocator 端到端路径
static int32_t cvTransientTest = 0;

// CVar: GPL 变体演示开关（0=关闭，1=开启，默认关闭）
// 设为 1 重新编译启用：初始化时生成 N 个仅 blend 状态不同的变体 PSO，
// 经限流器逐帧 fast-link 创建，验证 GPL 四段库缓存与限流协同工作。
static int32_t cvGPLVariantTest = 0;
static int32_t cvGPLVariantCount = 16;  // 变体数量 N

// 变体演示用的全屏三角形 VS + 全屏复制 FS（SPIR-V 头内联字节码）
// 位于全局命名空间，故使用完整限定名 he::rhi::ShaderBytecode
static he::rhi::ShaderBytecode g_VariantVS;
static he::rhi::ShaderBytecode g_VariantFS;

namespace he::render {

bool DeferredPipeline::Initialize(rhi::IRHIDevice* device) {
    m_Device = device;
    HE_ASSERT(m_Device, "DeferredPipeline: null device");

    // GBuffer 渲染器（纹理所有权 + PSO + 描述符集，共享组件）
    m_GBuffer = std::make_unique<GBufferRenderer>();
    m_GBuffer->Initialize(device, m_Width, m_Height);

    // 硬件 MSAA：覆盖纹理和 PSO 的 sampleCount（委托给 PostProcessChain）
    if (m_PostProcess.IsMSAAEnabled()) {
        HE_CORE_INFO("DeferredPipeline: MSAA enabled (HDR 目标多采样，GBuffer 保持 1x)");
    }

    // HDR 目标 + Lighting PSO + 描述符集（委托给 LightingPass）
    m_Lighting.Initialize(device, m_Width, m_Height);

    // LDR 中间纹理已在 PostProcessChain::Initialize() 中创建

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
    m_PostProcess.Initialize(device, m_Width, m_Height);  // ToneMap/Skybox/TAA/LDR 纹理
    // 延迟管线：SkyboxPass 用 LoadOp=Load 叠加到 Lighting 结果（背景天空盒，depth=Equal 只画无几何处）
    m_PostProcess.GetSkybox()->SetColorLoadOp(rhi::LoadOp::Load);
    m_SceneRenderer = std::make_unique<SceneRenderer>();
    m_GPUCulling.Initialize(device);
    if (m_GPUCulling.usePTG) {
        m_GPUCulling.InitializePTG(device);
    }
    m_GPUScene.Initialize(device);

    // 将子系统指针注入 GBufferRenderer（在它们全部初始化之后）
    m_GBuffer->SetSceneRenderer(m_SceneRenderer.get());
    m_GBuffer->SetGPUCulling(&m_GPUCulling);
    m_GBuffer->SetGPUScene(&m_GPUScene);
    m_GBuffer->SetVisibleIndices(&m_GPUVisibleIndices);
    m_GBuffer->SetMeshBatcher(&m_MeshBatcher);

    m_SSGI.Initialize(device, m_Width, m_Height);
    m_SSR.Initialize(device, m_Width, m_Height);
    m_DDGI.Initialize(device, m_Width, m_Height);
    m_DenoiseSSGI.Initialize(device, m_Width, m_Height);
    m_DenoiseSSR.Initialize(device, m_Width, m_Height);
    m_SSAO.Initialize(device, m_Width, m_Height);
    m_SSAO.enabled = false;  // 默认关闭
    // Bloom / FXAA / TAA / AutoExposure 已在 PostProcessChain::Initialize() 中创建

    // GBuffer PSO + 描述符集 + DGC 初始化已在 GBufferRenderer::Initialize() 中完成

    // 创建阴影 PSO（使用 GBuffer 的 descriptor set layout，
    // 阴影 VS 仅使用 binding=2 GPUObjectData[]，与 GBuffer layout 兼容）
    m_ShadowSystem->CreateShadowPSO(m_GBuffer->GetLayout());

    // ── DGC 初始化（通过 RHI 统一接口，后端透明）──
    if (device->GetCaps().supportsDGC) {
        bool dgcOK = device->InitializeDGC(m_GBuffer->GetPSO(),
            GPUCulling::kMaxObjects, GPUCulling::kMaxObjects);
        if (dgcOK) {
            HE_CORE_INFO("DeferredPipeline: DGC 初始化成功，可通过 r.DGC.Enable 1 启用");
        } else {
            HE_CORE_WARN("DeferredPipeline: DGC 初始化失败，回退到 ExecuteIndirect 路径");
        }
    } else {
        HE_CORE_INFO("DeferredPipeline: 硬件不支持 DGC，使用传统 ExecuteIndirect 路径");
    }

    // GBufferContext + IGBufferRenderer 已在 GBufferRenderer::Initialize() 中设置

    // GPU Profiler
    m_Profiler.Initialize(device, rhi::kMaxProfilerPasses, MAX_FRAMES_IN_FLIGHT);
    m_ProfilerPanel.SetProfiler(&m_Profiler);  // 绑定 ImGui 面板到 Profiler 数据源
    // Lighting PSO + 描述符集已在 LightingPass::Initialize() 中创建

    // 瞬态资源路径验证 PSO（全屏三角形 + 纹理拷贝，用于验证 Transient Allocator 端到端路径）
    {
        rhi::ShaderBytecode tVS, tFS;
        tVS.stage = rhi::ShaderStage::Vertex; tVS.spirv = k_Fullscreen_vert_spv; tVS.entryPoint = "main";
        tFS.stage = rhi::ShaderStage::Pixel;  tFS.spirv = k_FullscreenCopy_frag_spv; tFS.entryPoint = "main";
        rhi::PipelineStateDesc tDesc;
        tDesc.vertexShader = &tVS; tDesc.pixelShader = &tFS;
        tDesc.topology = rhi::PrimitiveTopology::TriangleList;
        tDesc.depthTest = false; tDesc.depthWrite = false;
        tDesc.colorAttachmentCount = 1;
        tDesc.colorFormats[0] = rhi::Format::RGBA16_FLOAT;  // 匹配瞬态纹理格式
        tDesc.debugName = "TransientTest";
        m_TransientTestPSO = device->CreatePipelineState(tDesc);
    }

    // GPU 粒子系统
    m_ParticleRenderer.Initialize(device);
    m_ParticleRenderer.SetSceneDepth(m_Lighting.GetHDRDepth(), m_Lighting.GetPointSampler());  // 软粒子深度纹理

    // 启动 PSO 后台预热（所有子系统已调用 PrecompileQueuePSO 注册 PSO 变体）
    // 注意：本机（Intel Arc B370 + 该版驱动）预编译 worker 线程会在
    // igc-default64.dll 内随机 SIGSEGV（约 50% 概率，见 gdb 栈：线程 27 编译器崩溃），
    // 导致 02.Cube 启动随机崩溃。预编译是纯优化（PSO 缓存已按 hash 去重，
    // 禁用后 PSO 仅在首次使用时惰性编译），故本机禁用。稳定驱动上可恢复。
    // device->StartPSOPrecompile();

    // ── GPL 变体演示（cvGPLVariantTest=1 且设备支持 GPL 时启用）──
    if (cvGPLVariantTest && device->GetCaps().supportsGraphicsPipelineLibrary) {
        g_VariantVS.stage = rhi::ShaderStage::Vertex;
        g_VariantVS.spirv = k_Fullscreen_vert_spv;
        g_VariantFS.stage = rhi::ShaderStage::Pixel;
        g_VariantFS.spirv = k_FullscreenCopy_frag_spv;

        rhi::PipelineStateDesc base;
        base.bindPoint        = rhi::PipelineBindPoint::Graphics;
        base.vertexShader     = &g_VariantVS;
        base.pixelShader      = &g_VariantFS;
        base.vertexLayout.stride = 0;              // 全屏三角形（SV_VertexID），无顶点输入
        base.colorAttachmentCount = 1;
        base.colorFormats[0]  = rhi::Format::RGBA16_FLOAT;
        base.depthFormat      = rhi::Format::Unknown;  // 无深度
        base.depthTest        = false;
        base.depthWrite       = false;
        base.sampleCount      = 1;

        for (int32_t i = 0; i < cvGPLVariantCount; ++i) {
            // 变体维度：仅 blend 状态不同（改变 fragment-output 段，其余 3 段共享）
            base.colorBlend[0].blendEnable         = true;
            base.colorBlend[0].srcColorBlendFactor =
                static_cast<rhi::BlendFactor>(i % 10);
            base.colorBlend[0].dstColorBlendFactor =
                static_cast<rhi::BlendFactor>((i / 10) % 10);
            device->EnqueuePSOCreate(base);
        }
        HE_CORE_INFO("DeferredPipeline: GPL 变体演示 — 已入队 {} 个变体 PSO",
                     cvGPLVariantCount);
    }

    m_Ready = true;
    HE_CORE_INFO("DeferredPipeline initialized");
    return true;
}

void DeferredPipeline::Shutdown() {
    if (m_ShadowSystem) m_ShadowSystem->Shutdown();
    m_PostProcess.Shutdown();
    if (m_GBuffer) m_GBuffer->Shutdown();
    m_Lighting.Shutdown();
    m_TransientTestPSO.reset();
    m_GPLVariantPSOs.clear();
    for (auto& b : m_LightBuffers) b.reset();
    for (auto& b : m_ObjectBuffers) b.reset();
    for (auto& b : m_ShadowBuffers) b.reset();
    for (auto& b : m_ShadowObjBuffers) b.reset();
    if (m_GPUCulling.usePTG) {
        m_GPUCulling.ShutdownPTG(m_Device);
    }
    m_GPUCulling.Shutdown(m_Device);
    m_GPUScene.Shutdown();
    // DGC 清理
    m_Device->ShutdownDGC();  // DGC 由 RHI 管理生命周期

    m_Profiler.Shutdown();
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
    m_Device = nullptr; m_Ready = false;
    HE_CORE_INFO("DeferredPipeline shutdown");
}

// CreateTextureDescriptorSet 已移除 — 使用全局 bindless 纹理数组替代

void DeferredPipeline::SetGBufferMode(GBufferRenderer::Mode mode) {
    if (m_GBuffer) m_GBuffer->SetMode(mode);
}

GBufferRenderer::Mode DeferredPipeline::GetGBufferMode() const {
    return m_GBuffer ? m_GBuffer->GetMode() : GBufferRenderer::Mode::CPU;
}

void DeferredPipeline::EnableFXAA(bool enable) {
    m_PostProcess.EnableFXAA(m_Device, m_Width, m_Height, enable);
}

void DeferredPipeline::EnableSMAA(bool enable) {
    m_PostProcess.EnableSMAA(m_Device, m_Width, m_Height, enable);
}

void DeferredPipeline::EnableMSAA(bool enable) {
    m_PostProcess.EnableMSAA(m_Device, m_Width, m_Height, enable);
    if (m_Ready && enable) {
        HE_CORE_WARN("DeferredPipeline: MSAA toggled after init — 需重启应用生效");
    }
}

void DeferredPipeline::NextFrame() {
    m_CurrentFrameSlot = (m_CurrentFrameSlot + 1) % MAX_FRAMES_IN_FLIGHT;

    // PSO 预热：主线程检查后台编译进度，完成后合并缓存
    static bool precompileMerged = false;
    if (!precompileMerged) {
        float progress = m_Device->GetPSOPrecompileProgress();
        if (progress >= 1.0f) {
            // Worker 线程完成 → 合并缓存到主 VkPipelineCache
            // vkMergePipelineCaches 由 VulkanDevice 内部调用
            precompileMerged = true;
            HE_CORE_INFO("DeferredPipeline: PSO 预热完成，worker 缓存已合并");
        } else if (static_cast<int>(progress * 100.0f) % 25 == 0) {
            // 每 25% 进度输出一次日志
            static int lastReported = -1;
            int pct = static_cast<int>(progress * 100.0f);
            if (pct / 25 != lastReported / 25) {
                lastReported = pct;
                HE_CORE_INFO("DeferredPipeline: PSO 预热进度 {:.0f}%", progress * 100.0f);
            }
        }
    }

    // PSO 限流器：每帧最多创建 3 个排队 PSO（变体演示 / 未来材质变体系统）
    static constexpr u32 kMaxPSOCreatesPerFrame = 3;
    if (m_Device->GetPendingPSOCreateCount() > 0) {
        auto created = m_Device->ProcessPSOCreateQueue(kMaxPSOCreatesPerFrame);
        for (auto& pso : created) m_GPLVariantPSOs.push_back(std::move(pso));
        HE_CORE_INFO("DeferredPipeline: 限流创建 {} 个 PSO（待处理 {}）",
                     static_cast<u32>(created.size()),
                     m_Device->GetPendingPSOCreateCount());
    }
}

void DeferredPipeline::OnResize(u32 w, u32 h) {
    if (w == m_Width && h == m_Height) return;
    m_Width = w; m_Height = h;
    // 重建 GBuffer 纹理（委托给 GBufferRenderer）
    if (m_GBuffer) m_GBuffer->OnResize(w, h);
    // 重建 HDR 目标（通过 LightingPass）
    m_Lighting.OnResize(m_Device, w, h);
    m_ParticleRenderer.SetSceneDepth(m_Lighting.GetHDRDepth(), m_Lighting.GetPointSampler());  // 软粒子深度纹理更新
    m_PostProcess.OnResize(m_Device, w, h);
    // GBufferContext 纹理指针已在 GBufferRenderer::OnResize() 中更新
    m_SSAO.OnResize(w, h);
    m_SSGI.OnResize(w, h);
    m_SSR.OnResize(w, h);
    m_DDGI.OnResize(w, h);
    m_DenoiseSSGI.OnResize(w, h);
    m_DenoiseSSR.OnResize(w, h);
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

        // 色温 → RGB 颜色（叠加到 color 滤镜色上）
        float3 lightColor = lc.color;
        if (lc.colorTemperature > 0.0f) {
            lightColor *= render::KelvinToRGB(lc.colorTemperature);
        }

        gl.colorIntensity = float4(lightColor, lc.intensity);
        gl.shadowIndex = m_ShadowSystem ? m_ShadowSystem->GetShadowIndex(e) : -1;

        // ── 物理光源模式（luminousIntensity>0 或 illuminance>0 时启用）──
        // positionRange.w < 0 标记物理模式（shader 用 abs() 获取实际范围值）
        switch (lc.type) {
        case he::LightType::Directional: {
            auto* dl = static_cast<he::DirectionalLight*>(&lc);
            gl.directionType = float4(dl->direction, 0.0f);
            if (lc.illuminance > 0.0f) {
                // 方向光物理模式：colorIntensity.w = 照度 (lux) × 换算系数, positionRange.w = -1 (flag)
                gl.colorIntensity.w = lc.illuminance * kPhysicalLightExposure;
                gl.positionRange.w   = -1.0f;
            }
            break;
        }
        case he::LightType::Point: {
            auto* pl = static_cast<he::PointLight*>(&lc);
            gl.positionRange = float4(sg.GetWorldPosition(e), pl->range);
            gl.directionType.w = 1.0f;
            if (lc.luminousIntensity > 0.0f) {
                // 点光源物理模式：colorIntensity.w = 发光强度 (cd) × 换算系数, 范围取负标记
                gl.colorIntensity.w = lc.luminousIntensity * kPhysicalLightExposure;
                gl.positionRange.w   = -(pl->range);
            }
            break;
        }
        case he::LightType::Spot: {
            auto* sl = static_cast<he::SpotLight*>(&lc);
            float r = lc.luminousIntensity > 0.0f ? -(sl->range) : sl->range;
            gl.positionRange = float4(sg.GetWorldPosition(e), r);
            gl.directionType = float4(glm::normalize(sl->direction), 2.0f); // Spot
            gl.coneAngles = float2(sl->innerConeAngle, sl->outerConeAngle);
            if (lc.luminousIntensity > 0.0f) {
                // 聚光物理模式：colorIntensity.w = 发光强度 (cd) × 换算系数
                gl.colorIntensity.w = lc.luminousIntensity * kPhysicalLightExposure;
            }
            break;
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
