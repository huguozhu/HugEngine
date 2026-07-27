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

#include "Asset/BindlessTextureManager.h"
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

namespace he::render {

bool DeferredPipeline::Initialize(rhi::IRHIDevice* device) {
    m_Device = device;
    HE_ASSERT(m_Device, "DeferredPipeline: null device");

    // GBuffer 渲染器（纹理所有权 + PSO + 描述符集，共享组件）
    m_GBuffer = std::make_unique<GBufferRenderer>();
    m_GBuffer->Initialize(device, m_Width, m_Height);

    // 硬件 MSAA：覆盖纹理和 PSO 的 sampleCount
    if (m_MSAAEnabled) {
        m_MSAA = std::make_unique<AA_MSAA>();
        m_MSAA->Initialize(device, m_Width, m_Height);
        HE_CORE_INFO("DeferredPipeline: MSAA {}x enabled (HDR 目标多采样，GBuffer 保持 1x)", m_MSAA->GetCurrentSampleCount());
    }

    // HDR 目标 + Lighting PSO + 描述符集（委托给 LightingPass）
    m_Lighting.Initialize(device, m_Width, m_Height);

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
    // Bloom / FXAA / GaussianBlur：懒初始化，首次 SetEnabled(true) 时才分配 GPU 资源

    // AA_TAA（HDR 空间）
    m_AntiAliasing = std::make_unique<AA_TAA>();
    if (!m_AntiAliasing->Initialize(device, m_Width, m_Height)) {
        HE_CORE_WARN("DeferredPipeline: AA_TAA init failed, anti-aliasing disabled");
        m_AntiAliasing.reset();
    }

    // AA_FXAA（LDR 空间，懒初始化：EnableFXAA(true) 首次调用时分配 GPU 资源）


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
    m_AutoExposure.Initialize(device, m_Width, m_Height);

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
    device->StartPSOPrecompile();

    m_Ready = true;
    HE_CORE_INFO("DeferredPipeline initialized");
    return true;
}

void DeferredPipeline::Shutdown() {
    if (m_ShadowSystem) m_ShadowSystem->Shutdown();
    if (m_ToneMap) m_ToneMap->Shutdown();
    if (m_Skybox)  m_Skybox->Shutdown();
    if (m_GBuffer) m_GBuffer->Shutdown();
    m_Lighting.Shutdown();
    if (m_AntiAliasing) m_AntiAliasing->Shutdown();
    m_AntiAliasing.reset();
    if (m_FXAA) m_FXAA->Shutdown();
    m_FXAA.reset();
    if (m_SMAA) m_SMAA->Shutdown();
    m_SMAA.reset();
    if (m_MSAA) m_MSAA->Shutdown();
    m_MSAA.reset();
    m_LDRTarget.reset(); m_LDRSampler.reset(); m_LDRDummyDepth.reset();
    m_TransientTestPSO.reset();
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

void DeferredPipeline::SetGBufferMode(GBufferRenderer::Mode mode) {
    if (m_GBuffer) m_GBuffer->SetMode(mode);
}

GBufferRenderer::Mode DeferredPipeline::GetGBufferMode() const {
    return m_GBuffer ? m_GBuffer->GetMode() : GBufferRenderer::Mode::CPU;
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
}

void DeferredPipeline::OnResize(u32 w, u32 h) {
    if (w == m_Width && h == m_Height) return;
    m_Width = w; m_Height = h;
    // 重建 GBuffer 纹理（委托给 GBufferRenderer）
    if (m_GBuffer) m_GBuffer->OnResize(w, h);
    // 重建 HDR 目标（通过 LightingPass）
    m_Lighting.OnResize(m_Device, w, h);
    m_ParticleRenderer.SetSceneDepth(m_Lighting.GetHDRDepth(), m_Lighting.GetPointSampler());  // 软粒子深度纹理更新
    if (m_ToneMap) m_ToneMap->OnResize(w, h);
    if (m_Skybox)  { m_Skybox->Shutdown(); m_Skybox->Initialize(m_Device, w, h); }
    if (m_AntiAliasing) m_AntiAliasing->OnResize(w, h);
    if (m_FXAA) m_FXAA->OnResize(w, h);
    if (m_SMAA) m_SMAA->OnResize(w, h);    // SMAA 中间纹理随分辨率重建
    // GBufferContext 纹理指针已在 GBufferRenderer::OnResize() 中更新
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
                // 方向光物理模式：colorIntensity.w = 照度 (lux), positionRange.w = -1 (flag)
                gl.colorIntensity.w = lc.illuminance;
                gl.positionRange.w   = -1.0f;
            }
            break;
        }
        case he::LightType::Point: {
            auto* pl = static_cast<he::PointLight*>(&lc);
            gl.positionRange = float4(sg.GetWorldPosition(e), pl->range);
            gl.directionType.w = 1.0f;
            if (lc.luminousIntensity > 0.0f) {
                // 点光源物理模式：colorIntensity.w = 发光强度 (cd), 范围取负标记
                gl.colorIntensity.w = lc.luminousIntensity;
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
                // 聚光物理模式：colorIntensity.w = 发光强度 (cd)
                gl.colorIntensity.w = lc.luminousIntensity;
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
