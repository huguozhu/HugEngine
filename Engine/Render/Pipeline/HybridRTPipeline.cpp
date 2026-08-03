// ============================================================
// HybridRTPipeline.cpp — 混合 Ray Tracing 管线实现
// GBuffer（光栅化）+ RT 阴影/反射/AO/GI（硬件 RT）+ DDGI
//
// 崩溃修复要点（相对旧版 HybridRTPipeline 的关键差异）：
//   1. 每个 AsyncCompute/Compute 末尾恢复 graphics pipeline，
//      确保 m_CurrentRenderPass 在后续 BeginRenderPass 前有效。
//   2. ToneMap PreBind + LDR 虚拟深度与 LDR 目标同尺寸。
//   3. FXAA → BackBuffer 前通过 ToneMap PreBind 保证 RP 兼容。
// ============================================================
#include "Pipeline/HybridRTPipeline.h"
#include "Pipeline/RTQualityCVars.h"
#include "Pipeline/PhysicalLight.h"
#include "Scene/World.h"
#include "Scene/SceneGraph.h"
#include "Scene/LightComponent.h"
#include "Scene/MeshComponent.h"
#include "Core/Log.h"
#include "Core/Assert.h"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <cstring>

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
    // 启用 FXAA：后处理链终端 Pass 需要图形 PSO 保证 RP 状态正确
    m_PostProcess.EnableFXAA(device, m_Width, m_Height, true);

    // ── GPU Profiler ──
    m_Profiler.Initialize(device, rhi::kMaxProfilerPasses, MAX_FRAMES_IN_FLIGHT);
    m_ProfilerPanel.SetProfiler(&m_Profiler);

    // ── GPU Driven 基础设施 ──
    m_SceneRenderer = std::make_unique<SceneRenderer>();
    m_GPUCulling.Initialize(device);
    m_GPUScene.Initialize(device);
    m_DDGI.Initialize(device, m_Width, m_Height);
    m_ParticleRenderer.Initialize(device);
    m_ParticleRenderer.SetSceneDepth(m_Lighting.GetHDRDepth(), m_Lighting.GetPointSampler());

    // 注入子系统指针到 GBufferRenderer
    m_GBuffer->SetSceneRenderer(m_SceneRenderer.get());
    m_GBuffer->SetGPUCulling(&m_GPUCulling);
    m_GBuffer->SetGPUScene(&m_GPUScene);
    m_GBuffer->SetVisibleIndices(&m_GPUVisibleIndices);
    m_GBuffer->SetMeshBatcher(&m_MeshBatcher);

    // ── 三缓冲 ──
    for (u32 i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        m_LightBuffers[i]  = device->CreateBuffer({sizeof(GPULight) * MAX_LIGHTS, rhi::BufferUsage::Storage});
        m_ObjectBuffers[i] = device->CreateBuffer({sizeof(GPUObjectData) * MAX_OBJECTS, rhi::BufferUsage::Storage});
        m_ShadowBuffers[i] = device->CreateBuffer({sizeof(GPUShadowData) * MAX_SHADOWS, rhi::BufferUsage::Storage});
    }

    // ── Ray Tracing 子系统 ──
    m_RTEnabled = device->GetCaps().supportsRayTracing;
    if (m_RTEnabled) {
        // RTPass：AS-only 模式（构建 BLAS/TLAS + 场景资源），效果管线由 RTEffectPass 创建
        m_RTPass = std::make_unique<RTPass>();
        if (m_RTPass->Initialize(device, {}, {})) {
            HE_CORE_INFO("HybridRTPipeline: RTPass 初始化完成 (AS-only)");

            // RT 阴影效果 Pass（默认半分辨率）
            m_RTShadow = std::make_unique<RTShadowPass>();
            if (m_RTShadow->Initialize(device, m_Width, m_Height, true)) {
                HE_CORE_INFO("HybridRTPipeline: RTShadowPass 初始化完成");
            } else {
                HE_CORE_WARN("HybridRTPipeline: RTShadowPass 初始化失败，RT 阴影禁用");
                m_RTShadow.reset();
            }

            // RT AO 效果 Pass（默认半分辨率）
            m_RTAO = std::make_unique<RTAOPass>();
            if (m_RTAO->Initialize(device, m_Width, m_Height, true)) {
                HE_CORE_INFO("HybridRTPipeline: RTAOPass 初始化完成");
            } else {
                HE_CORE_WARN("HybridRTPipeline: RTAOPass 初始化失败，RT AO 禁用");
                m_RTAO.reset();
            }

            // RT 反射效果 Pass（默认半分辨率）
            m_RTReflection = std::make_unique<RTReflectionPass>();
            if (m_RTReflection->Initialize(device, m_Width, m_Height, true)) {
                HE_CORE_INFO("HybridRTPipeline: RTReflectionPass 初始化完成");
            } else {
                HE_CORE_WARN("HybridRTPipeline: RTReflectionPass 初始化失败，RT 反射禁用");
                m_RTReflection.reset();
            }

            // RT GI 效果 Pass（默认四分之一分辨率）
            m_RTGI = std::make_unique<RTGIPass>();
            if (m_RTGI->Initialize(device, m_Width, m_Height, true)) {
                HE_CORE_INFO("HybridRTPipeline: RTGIPass 初始化完成");
            } else {
                HE_CORE_WARN("HybridRTPipeline: RTGIPass 初始化失败，RT GI 禁用");
                m_RTGI.reset();
            }

            // ── RT 降噪器（时域累积；反射/GI 追加空间滤波）──
            // 阴影/AO：半分辨率，仅时域累积（遮蔽信号时域变化慢，用低混合比获得最大累积）
            if (m_RTShadow && m_RTShadow->IsValid()) {
                RTDenoiser::Config cfg;
                cfg.format          = rhi::Format::R16_FLOAT;
                cfg.width           = m_RTShadow->GetWidth();
                cfg.height          = m_RTShadow->GetHeight();
                cfg.temporalBlend   = 0.05f;
                cfg.depthThreshold  = 0.02f;
                cfg.normalThreshold = 0.85f;
                cfg.debugName       = "RTShadowDenoiser";
                m_ShadowDenoiser = std::make_unique<RTDenoiser>();
                if (m_ShadowDenoiser->Initialize(device, cfg))
                    HE_CORE_INFO("HybridRTPipeline: RTShadowDenoiser 初始化完成");
                else {
                    m_ShadowDenoiser.reset();
                    HE_CORE_WARN("HybridRTPipeline: RTShadowDenoiser 初始化失败，RT 阴影降噪禁用");
                }
            }
            if (m_RTAO && m_RTAO->IsValid()) {
                RTDenoiser::Config cfg;
                cfg.format          = rhi::Format::R8_UNORM;
                cfg.width           = m_RTAO->GetWidth();
                cfg.height          = m_RTAO->GetHeight();
                cfg.temporalBlend   = 0.05f;
                cfg.depthThreshold  = 0.02f;
                cfg.normalThreshold = 0.85f;
                cfg.debugName       = "RTAODenoiser";
                m_AODenoiser = std::make_unique<RTDenoiser>();
                if (m_AODenoiser->Initialize(device, cfg))
                    HE_CORE_INFO("HybridRTPipeline: RTAODenoiser 初始化完成");
                else {
                    m_AODenoiser.reset();
                    HE_CORE_WARN("HybridRTPipeline: RTAODenoiser 初始化失败，RT AO 降噪禁用");
                }
            }
            // 反射：半分辨率 RGBA16_FLOAT，时域累积 + 空间滤波
            if (m_RTReflection && m_RTReflection->IsValid()) {
                RTDenoiser::Config cfg;
                cfg.format          = rhi::Format::RGBA16_FLOAT;
                cfg.width           = m_RTReflection->GetWidth();
                cfg.height          = m_RTReflection->GetHeight();
                cfg.temporalBlend   = 0.10f;
                cfg.depthThreshold  = 0.05f;
                cfg.normalThreshold = 0.80f;
                cfg.debugName       = "RTReflectionDenoiser";
                m_ReflectionDenoiser = std::make_unique<RTDenoiser>();
                if (m_ReflectionDenoiser->Initialize(device, cfg))
                    HE_CORE_INFO("HybridRTPipeline: RTReflectionDenoiser 初始化完成");
                else {
                    m_ReflectionDenoiser.reset();
                    HE_CORE_WARN("HybridRTPipeline: RTReflectionDenoiser 初始化失败，RT 反射降噪禁用");
                }
                // 空间滤波：5×5 双边模糊（复用 DeferredPipeline 的 Denoiser）
                if (!m_ReflectionSpatial.Initialize(device,
                        m_RTReflection->GetWidth(), m_RTReflection->GetHeight()))
                    HE_CORE_WARN("HybridRTPipeline: RTReflectionSpatial 初始化失败");
            }
            // GI：四分之一分辨率 RGBA16_FLOAT，时域累积 + 空间滤波（低分辨率锯齿用滤波补足）
            if (m_RTGI && m_RTGI->IsValid()) {
                RTDenoiser::Config cfg;
                cfg.format          = rhi::Format::RGBA16_FLOAT;
                cfg.width           = m_RTGI->GetWidth();
                cfg.height          = m_RTGI->GetHeight();
                cfg.temporalBlend   = 0.15f;
                cfg.depthThreshold  = 0.05f;
                cfg.normalThreshold = 0.80f;
                cfg.debugName       = "RTGIDenoiser";
                m_GIDenoiser = std::make_unique<RTDenoiser>();
                if (m_GIDenoiser->Initialize(device, cfg))
                    HE_CORE_INFO("HybridRTPipeline: RTGIDenoiser 初始化完成");
                else {
                    m_GIDenoiser.reset();
                    HE_CORE_WARN("HybridRTPipeline: RTGIDenoiser 初始化失败，RT GI 降噪禁用");
                }
                if (!m_GISpatial.Initialize(device,
                        m_RTGI->GetWidth(), m_RTGI->GetHeight()))
                    HE_CORE_WARN("HybridRTPipeline: RTGISpatial 初始化失败");
            }
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
    if (m_RTShadow) { m_RTShadow->Shutdown(); m_RTShadow.reset(); }
    if (m_RTAO) { m_RTAO->Shutdown(); m_RTAO.reset(); }
    if (m_RTReflection) { m_RTReflection->Shutdown(); m_RTReflection.reset(); }
    if (m_RTGI) { m_RTGI->Shutdown(); m_RTGI.reset(); }
    if (m_RTPass) { m_RTPass->Shutdown(); m_RTPass.reset(); }
    // RT 降噪器
    if (m_ShadowDenoiser) { m_ShadowDenoiser->Shutdown(); m_ShadowDenoiser.reset(); }
    if (m_AODenoiser) { m_AODenoiser->Shutdown(); m_AODenoiser.reset(); }
    if (m_ReflectionDenoiser) { m_ReflectionDenoiser->Shutdown(); m_ReflectionDenoiser.reset(); }
    if (m_GIDenoiser) { m_GIDenoiser->Shutdown(); m_GIDenoiser.reset(); }
    m_ReflectionSpatial.Shutdown();
    m_GISpatial.Shutdown();
    m_PostProcess.Shutdown();
    m_Profiler.Shutdown();
    m_Lighting.Shutdown();
    if (m_GBuffer) m_GBuffer->Shutdown();
    for (auto& b : m_LightBuffers) b.reset();
    for (auto& b : m_ObjectBuffers) b.reset();
    for (auto& b : m_ShadowBuffers) b.reset();
    m_Device = nullptr; m_Ready = false;
    HE_CORE_INFO("HybridRTPipeline: shutdown");
}

void HybridRTPipeline::NextFrame() {
    m_CurrentFrameSlot = (m_CurrentFrameSlot + 1) % MAX_FRAMES_IN_FLIGHT;
}

void HybridRTPipeline::OnResize(u32 w, u32 h) {
    if (w == m_Width && h == m_Height) return;
    m_Width = w; m_Height = h;
    if (m_GBuffer) m_GBuffer->OnResize(w, h);
    m_Lighting.OnResize(m_Device, w, h);
    m_ParticleRenderer.SetSceneDepth(m_Lighting.GetHDRDepth(), m_Lighting.GetPointSampler());
    m_PostProcess.OnResize(m_Device, w, h);
    m_DDGI.OnResize(w, h);
    // RT 效果 Pass 重建输出纹理（通过 Shutdown + Initialize）
    if (m_RTShadow && m_Device) {
        bool half = m_RTShadow->IsHalfRes();
        m_RTShadow->Shutdown();
        m_RTShadow->Initialize(m_Device, w, h, half);
    }
    if (m_RTAO && m_Device) {
        bool half = m_RTAO->IsHalfRes();
        m_RTAO->Shutdown();
        m_RTAO->Initialize(m_Device, w, h, half);
    }
    if (m_RTReflection && m_Device) {
        bool half = m_RTReflection->IsHalfRes();
        m_RTReflection->Shutdown();
        m_RTReflection->Initialize(m_Device, w, h, half);
    }
    if (m_RTGI && m_Device) {
        bool qr = m_RTGI->IsQuarterRes();
        m_RTGI->Shutdown();
        m_RTGI->Initialize(m_Device, w, h, qr);
    }
    // RT 降噪器随 RT Pass 分辨率重建（RT Pass 已用新尺寸重新初始化）
    if (m_ShadowDenoiser && m_RTShadow)
        m_ShadowDenoiser->OnResize(m_RTShadow->GetWidth(), m_RTShadow->GetHeight());
    if (m_AODenoiser && m_RTAO)
        m_AODenoiser->OnResize(m_RTAO->GetWidth(), m_RTAO->GetHeight());
    if (m_ReflectionDenoiser && m_RTReflection) {
        m_ReflectionDenoiser->OnResize(m_RTReflection->GetWidth(), m_RTReflection->GetHeight());
        m_ReflectionSpatial.OnResize(m_RTReflection->GetWidth(), m_RTReflection->GetHeight());
    }
    if (m_GIDenoiser && m_RTGI) {
        m_GIDenoiser->OnResize(m_RTGI->GetWidth(), m_RTGI->GetHeight());
        m_GISpatial.OnResize(m_RTGI->GetWidth(), m_RTGI->GetHeight());
    }
    m_SceneMaterialBuilt = false;  // 材质纹理无需重建（场景不变），但置位以便懒重建
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

    if (!m_SwapChain || !m_Device) {
        HE_CORE_ERROR("HybridRTPipeline::Render: SwapChain 或 Device 未设置");
        return;
    }

    m_FrameIndex++;  // 帧索引递增（RT 时域抖动 / 降噪用）

    RenderGraph rg;
    rg.SetProfiler(&m_Profiler);
    rg.SetSwapChain(m_SwapChain);

    BuildFrameGraph(rg, world, sg, camera);

    rg.Compile();
    rg.Execute(cmd, m_Device);
}

// ============================================================
// CollectLights — 收集场景光源数据到当前帧槽位 SSBO
// ============================================================
void HybridRTPipeline::CollectLights(he::World& world, he::SceneGraph& sg,
                                      const CameraData& camera, u32& outLightCount) {
    (void)camera;
    outLightCount = 0;
    u32 lightCount = 0;

    auto cl = [&](he::Entity e, he::LightComponent& lc) {
        if (lightCount >= MAX_LIGHTS || !lc.enabled) return;
        GPULight gl{};
        gl.shadowIndex = -1;  // 混合 RT 管线无传统阴影系统，阴影由 RT 阴影 Pass 覆盖

        // 色温 → RGB 颜色（叠加到 color 滤镜色上）
        float3 lightColor = lc.color;
        if (lc.colorTemperature > 0.0f) {
            lightColor *= render::KelvinToRGB(lc.colorTemperature);
        }
        gl.colorIntensity = float4(lightColor, lc.intensity);

        // ── 物理光源模式（luminousIntensity>0 或 illuminance>0 时启用）──
        // positionRange.w < 0 标记物理模式（shader 用 abs() 获取实际范围值）
        switch (lc.type) {
        case LightType::Directional: {
            auto* dl = static_cast<DirectionalLight*>(&lc);
            gl.directionType = float4(dl->direction, 0.0f);
            if (lc.illuminance > 0.0f) {
                gl.colorIntensity.w = lc.illuminance;
                gl.positionRange.w   = -1.0f;
            }
            break;
        }
        case LightType::Point: {
            auto* pl = static_cast<PointLight*>(&lc);
            gl.positionRange = float4(sg.GetWorldPosition(e), pl->range);
            gl.directionType.w = 1.0f;
            if (lc.luminousIntensity > 0.0f) {
                gl.colorIntensity.w = lc.luminousIntensity;
                gl.positionRange.w   = -(pl->range);
            }
            break;
        }
        case LightType::Spot: {
            auto* sl = static_cast<SpotLight*>(&lc);
            float r = lc.luminousIntensity > 0.0f ? -(sl->range) : sl->range;
            gl.positionRange = float4(sg.GetWorldPosition(e), r);
            gl.directionType = float4(glm::normalize(sl->direction), 2.0f);
            gl.coneAngles = float2(sl->innerConeAngle, sl->outerConeAngle);
            if (lc.luminousIntensity > 0.0f) {
                gl.colorIntensity.w = lc.luminousIntensity;
            }
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
    if (m_SwapChain) rg.SetSwapChain(m_SwapChain);
    u32 w = m_Width, h = m_Height;
    // 从 GBufferRenderer 导入所有 GBuffer 纹理
    auto gb = m_GBuffer->ImportToRenderGraph(rg);
    auto gbA = gb.albedo, gbB = gb.normal, gbC = gb.emissive;
    auto gbDepth = gb.depth, gbVel = gb.velocity, gbWorldPos = gb.worldPos;
    auto hdrC   = rg.ImportTexture("HDR_C", m_Lighting.GetHDRTarget());
    auto backBuf = rg.ImportBackBuffer();

    // ── 帧首：更新相机矩阵（velocity / 时域降噪用）──
    m_CurrViewProj = camera.GetViewProjMatrix();
    static bool firstFrame = true;
    if (firstFrame) { m_PrevViewProj = m_CurrViewProj; firstFrame = false; }

    // ── Mesh 批处理 + GPUScene 上传 ──
    m_GPUScene.Collect(world, sg);
    if (m_GBuffer->GetMode() == GBufferRenderer::Mode::GPU) {
        if (!m_BatchBuilt) { m_MeshBatcher.Build(world); m_BatchBuilt = true; }
        m_MeshBatcher.FillGPUScene(m_GPUScene);
    }
    m_GPUScene.Upload(m_Device);

    // ── GPU 可见性（上帧 Readback 结果）──
    if (m_GPUCulling.enabled) {
        m_GPUCulling.Readback(m_Device, m_GPUVisibleIndices);
    } else {
        m_GPUVisibleIndices.clear();
    }

    // ── AS Build — BLAS/TLAS 构建（P1 起 RT 启用时插入）──
    if (m_RTEnabled && m_RTPass && m_RTPass->IsValid()) {
        rg.AddPass("AS_Build", {}, {},
            [this, &world, &sg](rhi::IRHICommandList* c) {
                m_RTPass->BuildAS(c, world, sg);
            });

        // 场景材质纹理（反射/GI ClosestHit 用）：首帧延迟构建一次
        //（CPU 侧资源创建，不放 Pass lambda 内；场景静态时无需每帧重建）
        if (!m_SceneMaterialBuilt) {
            if (m_RTPass->BuildSceneMaterialTexture(m_Device, world)) {
                m_SceneMaterialBuilt = true;
            } else {
                HE_CORE_WARN("HybridRTPipeline: 场景材质纹理构建失败，反射/GI 材质查询不可用");
            }
        }
    }

    // ── GBuffer（光栅化，与 DeferredPipeline 相同）──
    rg.AddPass("GB_Clear", {}, {{gbA, ResourceAccess::Write}, {gbB, ResourceAccess::Write},
        {gbC, ResourceAccess::Write}, {gbVel, ResourceAccess::Write},
        {gbWorldPos, ResourceAccess::Write}, {gbDepth, ResourceAccess::Write}},
        [&](rhi::IRHICommandList* c) {
            m_GBuffer->SetObjectBuffer(m_ObjectBuffers[m_CurrentFrameSlot].get());
            m_GBuffer->SetPrevViewProj(m_PrevViewProj);
            m_GBuffer->Render(c, world, sg, camera);
        });

    // ── 收集光源（RT 阴影与 Lighting 共用当前帧槽位数据）──
    u32 lightCount = 0;
    CollectLights(world, sg, camera, lightCount);

    // ── CVar 热更新：降噪时域混合因子（每帧应用，Render 时写入 push constant）──
    if (m_ShadowDenoiser)     m_ShadowDenoiser->SetTemporalBlend(cvRTDenoiseShadowBlend.Get());
    if (m_AODenoiser)         m_AODenoiser->SetTemporalBlend(cvRTDenoiseAOBlend.Get());
    if (m_ReflectionDenoiser) m_ReflectionDenoiser->SetTemporalBlend(cvRTDenoiseReflectionBlend.Get());
    if (m_GIDenoiser)         m_GIDenoiser->SetTemporalBlend(cvRTDenoiseGIBlend.Get());

    // ── RT Shadow — 对 GBuffer 有效像素发射阴影射线（半分辨率遮罩）──
    rhi::IRHITexture* rtShadowTex = nullptr;
    ResourceHandle rtShadowHandle = kInvalidHandle;
    if (m_RTEnabled && m_RTShadowEnabled && m_RTShadow && m_RTShadow->IsValid() && m_RTPass) {
        rtShadowTex = m_RTShadow->GetOutput();
        rtShadowHandle = rg.ImportTexture("RT_ShadowMask", rtShadowTex);
        rg.AddPass("RT_Shadow",
            {{gbDepth, ResourceAccess::Read}, {gbB, ResourceAccess::Read}},
            {{rtShadowHandle, ResourceAccess::UAV}},
            [this, &camera, lightCount](rhi::IRHICommandList* c) {
                RTExecuteContext ctx;
                ctx.invViewProj = glm::inverse(camera.GetViewProjMatrix());
                ctx.cameraPos   = camera.position;
                ctx.frameIndex  = m_FrameIndex;
                ctx.gbDepth     = m_GBuffer->GetDepth();
                ctx.gbNormal    = m_GBuffer->GetNormal();
                ctx.lightBuffer = m_LightBuffers[m_CurrentFrameSlot].get();
                ctx.lightCount  = lightCount;
                m_RTShadow->Execute(c, m_RTPass->GetTLAS(), ctx);
            });
    }

    // ── RT Shadow Denoise — 时域累积（velocity 重投影 + 去遮挡检测）──
    // 用降噪输出替代原始噪声遮罩供 Lighting 采样，消除单帧 RT 阴影噪声闪烁
    rhi::IRHITexture* rtShadowDenoisedTex = nullptr;
    ResourceHandle rtShadowDenoisedHandle = kInvalidHandle;
    if (m_ShadowDenoiser && m_ShadowDenoiser->IsReady() && rtShadowHandle != kInvalidHandle) {
        rtShadowDenoisedTex = m_ShadowDenoiser->GetOutput();
        rtShadowDenoisedHandle = rg.ImportTexture("RT_ShadowMask_Denoised", rtShadowDenoisedTex);
        rg.AddPass("RT_Shadow_Denoise",
            {{rtShadowHandle, ResourceAccess::Read},
             {gbDepth, ResourceAccess::Read}, {gbB, ResourceAccess::Read}, {gbVel, ResourceAccess::Read}},
            {{rtShadowDenoisedHandle, ResourceAccess::Write}},
            [this](rhi::IRHICommandList* c) {
                m_ShadowDenoiser->SetInputs(m_RTShadow->GetOutput(),
                    m_GBuffer->GetDepth(), m_GBuffer->GetNormal(), m_GBuffer->GetVelocity());
                m_ShadowDenoiser->Render(c);
            });
    }

    // ── RT AO — 对 GBuffer 有效像素发射遮蔽射线（半分辨率遮罩）──
    rhi::IRHITexture* rtAOTex = nullptr;
    ResourceHandle rtAOHandle = kInvalidHandle;
    if (m_RTEnabled && m_RTAOEnabled && m_RTAO && m_RTAO->IsValid() && m_RTPass) {
        rtAOTex = m_RTAO->GetOutput();
        rtAOHandle = rg.ImportTexture("RT_AO", rtAOTex);
        rg.AddPass("RT_AO",
            {{gbDepth, ResourceAccess::Read}, {gbB, ResourceAccess::Read}},
            {{rtAOHandle, ResourceAccess::UAV}},
            [this, &camera](rhi::IRHICommandList* c) {
                RTExecuteContext ctx;
                ctx.invViewProj = glm::inverse(camera.GetViewProjMatrix());
                ctx.cameraPos   = camera.position;
                ctx.frameIndex  = m_FrameIndex;
                ctx.gbDepth     = m_GBuffer->GetDepth();
                ctx.gbNormal    = m_GBuffer->GetNormal();
                m_RTAO->Execute(c, m_RTPass->GetTLAS(), ctx);
            });
    }

    // ── RT AO Denoise — 时域累积（AO 是低频信号，累积消除单帧噪声）──
    rhi::IRHITexture* rtAODenoisedTex = nullptr;
    ResourceHandle rtAODenoisedHandle = kInvalidHandle;
    if (m_AODenoiser && m_AODenoiser->IsReady() && rtAOHandle != kInvalidHandle) {
        rtAODenoisedTex = m_AODenoiser->GetOutput();
        rtAODenoisedHandle = rg.ImportTexture("RT_AO_Denoised", rtAODenoisedTex);
        rg.AddPass("RT_AO_Denoise",
            {{rtAOHandle, ResourceAccess::Read},
             {gbDepth, ResourceAccess::Read}, {gbB, ResourceAccess::Read}, {gbVel, ResourceAccess::Read}},
            {{rtAODenoisedHandle, ResourceAccess::Write}},
            [this](rhi::IRHICommandList* c) {
                m_AODenoiser->SetInputs(m_RTAO->GetOutput(),
                    m_GBuffer->GetDepth(), m_GBuffer->GetNormal(), m_GBuffer->GetVelocity());
                m_AODenoiser->Render(c);
            });
    }

    // ── RT Reflection — 对 GBuffer 像素发射反射光线（半分辨率）──
    rhi::IRHITexture* rtReflectionTex = nullptr;
    ResourceHandle rtReflectionHandle = kInvalidHandle;
    if (m_RTEnabled && m_RTReflectionEnabled && m_RTReflection && m_RTReflection->IsValid() && m_RTPass) {
        rtReflectionTex = m_RTReflection->GetOutput();
        rtReflectionHandle = rg.ImportTexture("RT_Reflection", rtReflectionTex);
        rg.AddPass("RT_Reflection",
            {{gbDepth, ResourceAccess::Read}, {gbB, ResourceAccess::Read}},
            {{rtReflectionHandle, ResourceAccess::UAV}},
            [this, &camera, lightCount](rhi::IRHICommandList* c) {
                RTExecuteContext ctx;
                ctx.invViewProj = glm::inverse(camera.GetViewProjMatrix());
                ctx.cameraPos   = camera.position;
                ctx.frameIndex  = m_FrameIndex;
                ctx.gbDepth     = m_GBuffer->GetDepth();
                ctx.gbNormal    = m_GBuffer->GetNormal();
                ctx.lightBuffer = m_LightBuffers[m_CurrentFrameSlot].get();
                ctx.lightCount  = lightCount;
                ctx.sceneMaterialTex = m_RTPass->GetSceneMaterialTexture();
                ctx.sceneTriangleNormals = m_RTPass->GetSceneTriangleNormals();
                m_RTReflection->Execute(c, m_RTPass->GetTLAS(), ctx);
            });
    }

    // ── RT Reflection Denoise — 时域累积 + 空间滤波（5×5 双边模糊）──
    rhi::IRHITexture* rtReflectionTemporalTex = nullptr;
    ResourceHandle rtReflectionTemporalHandle = kInvalidHandle;
    if (m_ReflectionDenoiser && m_ReflectionDenoiser->IsReady() && rtReflectionHandle != kInvalidHandle) {
        rtReflectionTemporalTex = m_ReflectionDenoiser->GetOutput();
        rtReflectionTemporalHandle = rg.ImportTexture("RT_Reflection_Temporal", rtReflectionTemporalTex);
        rg.AddPass("RT_Reflection_Temporal",
            {{rtReflectionHandle, ResourceAccess::Read},
             {gbDepth, ResourceAccess::Read}, {gbB, ResourceAccess::Read}, {gbVel, ResourceAccess::Read}},
            {{rtReflectionTemporalHandle, ResourceAccess::Write}},
            [this](rhi::IRHICommandList* c) {
                m_ReflectionDenoiser->SetInputs(m_RTReflection->GetOutput(),
                    m_GBuffer->GetDepth(), m_GBuffer->GetNormal(), m_GBuffer->GetVelocity());
                m_ReflectionDenoiser->Render(c);
            });
    }
    rhi::IRHITexture* rtReflectionDenoisedTex = rtReflectionTemporalTex;
    ResourceHandle rtReflectionDenoisedHandle = rtReflectionTemporalHandle;
    if (m_ReflectionSpatial.IsReady() && rtReflectionTemporalHandle != kInvalidHandle) {
        rtReflectionDenoisedTex = m_ReflectionSpatial.GetOutput();
        rtReflectionDenoisedHandle = rg.ImportTexture("RT_Reflection_Denoised", rtReflectionDenoisedTex);
        rg.AddPass("RT_Reflection_Spatial",
            {{rtReflectionTemporalHandle, ResourceAccess::Read},
             {gbDepth, ResourceAccess::Read}, {gbB, ResourceAccess::Read}},
            {{rtReflectionDenoisedHandle, ResourceAccess::Write}},
            [this, rtReflectionTemporalTex, rw = m_ReflectionDenoiser->GetWidth(),
             rh = m_ReflectionDenoiser->GetHeight()](rhi::IRHICommandList* c) {
                m_ReflectionSpatial.PreBind(c);
                // 输入为时域累积输出（构建时捕获，时域 Pass 已交换历史角色，不能用 GetOutput()）
                m_ReflectionSpatial.SetInputs(rtReflectionTemporalTex,
                    m_GBuffer->GetDepth(), m_GBuffer->GetNormal());
                rhi::ClearValue clr{};
                c->BeginOffscreenPass(m_ReflectionSpatial.GetOutput()->GetNativeHandle(),
                    nullptr, rw, rh, &clr, false);
                m_ReflectionSpatial.Render(c);
                c->EndOffscreenPass();
            });
    }

    // ── RT GI — 对 GBuffer 像素发射间接光射线（四分之一分辨率）──
    rhi::IRHITexture* rtGITex = nullptr;
    ResourceHandle rtGIHandle = kInvalidHandle;
    if (m_RTEnabled && m_RTGIEnabled && m_RTGI && m_RTGI->IsValid() && m_RTPass) {
        rtGITex = m_RTGI->GetOutput();
        rtGIHandle = rg.ImportTexture("RT_GI", rtGITex);
        rg.AddPass("RT_GI",
            {{gbDepth, ResourceAccess::Read}, {gbB, ResourceAccess::Read}},
            {{rtGIHandle, ResourceAccess::UAV}},
            [this, &camera, lightCount](rhi::IRHICommandList* c) {
                RTExecuteContext ctx;
                ctx.invViewProj = glm::inverse(camera.GetViewProjMatrix());
                ctx.cameraPos   = camera.position;
                ctx.frameIndex  = m_FrameIndex;
                ctx.gbDepth     = m_GBuffer->GetDepth();
                ctx.gbNormal    = m_GBuffer->GetNormal();
                ctx.lightBuffer = m_LightBuffers[m_CurrentFrameSlot].get();
                ctx.lightCount  = lightCount;
                ctx.sceneMaterialTex = m_RTPass->GetSceneMaterialTexture();
                ctx.sceneTriangleNormals = m_RTPass->GetSceneTriangleNormals();
                m_RTGI->Execute(c, m_RTPass->GetTLAS(), ctx);
            });
    }

    // ── RT GI Denoise — 时域累积 + 空间滤波（四分之一分辨率低频信号，滤波补足锯齿）──
    rhi::IRHITexture* rtGITemporalTex = nullptr;
    ResourceHandle rtGITemporalHandle = kInvalidHandle;
    if (m_GIDenoiser && m_GIDenoiser->IsReady() && rtGIHandle != kInvalidHandle) {
        rtGITemporalTex = m_GIDenoiser->GetOutput();
        rtGITemporalHandle = rg.ImportTexture("RT_GI_Temporal", rtGITemporalTex);
        rg.AddPass("RT_GI_Temporal",
            {{rtGIHandle, ResourceAccess::Read},
             {gbDepth, ResourceAccess::Read}, {gbB, ResourceAccess::Read}, {gbVel, ResourceAccess::Read}},
            {{rtGITemporalHandle, ResourceAccess::Write}},
            [this](rhi::IRHICommandList* c) {
                m_GIDenoiser->SetInputs(m_RTGI->GetOutput(),
                    m_GBuffer->GetDepth(), m_GBuffer->GetNormal(), m_GBuffer->GetVelocity());
                m_GIDenoiser->Render(c);
            });
    }
    rhi::IRHITexture* rtGIDenoisedTex = rtGITemporalTex;
    ResourceHandle rtGIDenoisedHandle = rtGITemporalHandle;
    if (m_GISpatial.IsReady() && rtGITemporalHandle != kInvalidHandle) {
        rtGIDenoisedTex = m_GISpatial.GetOutput();
        rtGIDenoisedHandle = rg.ImportTexture("RT_GI_Denoised", rtGIDenoisedTex);
        rg.AddPass("RT_GI_Spatial",
            {{rtGITemporalHandle, ResourceAccess::Read},
             {gbDepth, ResourceAccess::Read}, {gbB, ResourceAccess::Read}},
            {{rtGIDenoisedHandle, ResourceAccess::Write}},
            [this, rtGITemporalTex, rw = m_GIDenoiser->GetWidth(),
             rh = m_GIDenoiser->GetHeight()](rhi::IRHICommandList* c) {
                m_GISpatial.PreBind(c);
                // 输入为时域累积输出（构建时捕获，时域 Pass 已交换历史角色，不能用 GetOutput()）
                m_GISpatial.SetInputs(rtGITemporalTex,
                    m_GBuffer->GetDepth(), m_GBuffer->GetNormal());
                rhi::ClearValue clr{};
                c->BeginOffscreenPass(m_GISpatial.GetOutput()->GetNativeHandle(),
                    nullptr, rw, rh, &clr, false);
                m_GISpatial.Render(c);
                c->EndOffscreenPass();
            });
    }

    // ── DDGI 更新（探针 GI，远距离低频光照）──
    rg.AddPass("DDGI_Update",
        {{gbA, ResourceAccess::Read}, {gbB, ResourceAccess::Read}, {gbDepth, ResourceAccess::Read}}, {},
        [&](rhi::IRHICommandList* c) {
            if (m_DDGI.IsEnabled()) {
                m_DDGI.SetGBufferInputs(m_GBuffer->GetDepth(), m_GBuffer->GetNormal(), m_GBuffer->GetAlbedo());
                SubsystemContext ctx;
                ctx.camera = &camera;
                m_DDGI.Update(ctx);
                m_DDGI.Render(c);
                // Compute dispatch 后恢复 graphics pipeline，
                // 确保后续 pass 的 SetPipeline / BeginOffscreenPass 状态正确
                c->SetPipeline(m_Lighting.GetPSO());
            }
        },
        RGPassQueue::Compute);

    // ── Lighting（延迟光照，读取 GBuffer + RT 效果纹理 + DDGI）──
    // Lighting 必须显式声明对 RT 效果输出纹理的读取依赖：否则 RenderGraph 的
    // LIFO 拓扑排序会把 Lighting 排到各 RT Pass 之前，导致采样到上一帧（或
    // 未初始化）的数据。声明读依赖后 RG 自动插入 UAV→ShaderResource 屏障
    //（源阶段 RayTracingShader → 目标阶段 FragmentShader）。
    std::vector<PassResource> lightingReads = {
        {gbA, ResourceAccess::Read}, {gbB, ResourceAccess::Read},
        {gbC, ResourceAccess::Read}, {gbWorldPos, ResourceAccess::Read},
        {gbDepth, ResourceAccess::Read},  // 深度转换由 RG 统一管理（RT 效果 Pass 先读，Lighting 后读）
    };
    // Lighting 采样降噪后输出（若降噪器就绪），否则回退到原始 RT 输出
    if (rtShadowDenoisedHandle != kInvalidHandle)
        lightingReads.push_back({rtShadowDenoisedHandle, ResourceAccess::Read});
    else if (rtShadowHandle != kInvalidHandle)
        lightingReads.push_back({rtShadowHandle, ResourceAccess::Read});
    if (rtAODenoisedHandle != kInvalidHandle)
        lightingReads.push_back({rtAODenoisedHandle, ResourceAccess::Read});
    else if (rtAOHandle != kInvalidHandle)
        lightingReads.push_back({rtAOHandle, ResourceAccess::Read});
    if (rtReflectionDenoisedHandle != kInvalidHandle)
        lightingReads.push_back({rtReflectionDenoisedHandle, ResourceAccess::Read});
    else if (rtReflectionHandle != kInvalidHandle)
        lightingReads.push_back({rtReflectionHandle, ResourceAccess::Read});
    if (rtGIDenoisedHandle != kInvalidHandle)
        lightingReads.push_back({rtGIDenoisedHandle, ResourceAccess::Read});
    else if (rtGIHandle != kInvalidHandle)
        lightingReads.push_back({rtGIHandle, ResourceAccess::Read});

    // 降噪纹理指针（优先降噪输出；反射/GI 有空间滤波时用空间输出，否则用时域输出）
    rhi::IRHITexture* lightingShadowTex = rtShadowDenoisedTex ? rtShadowDenoisedTex : rtShadowTex;
    rhi::IRHITexture* lightingAOTex     = rtAODenoisedTex     ? rtAODenoisedTex     : rtAOTex;
    rhi::IRHITexture* lightingReflTex   = rtReflectionDenoisedTex ? rtReflectionDenoisedTex : rtReflectionTex;
    rhi::IRHITexture* lightingGITex     = rtGIDenoisedTex     ? rtGIDenoisedTex     : rtGITex;

    rg.AddPass("Lighting",
        std::move(lightingReads),
        {{hdrC, ResourceAccess::Write}},
        [this, &camera, w, h, lightCount, lightingShadowTex, lightingReflTex, lightingAOTex, lightingGITex](rhi::IRHICommandList* c) {
            // 深度 DepthStencilWrite→DepthStencilRead 转换由 RenderGraph 依据
            // gbDepth 的读取依赖（RT 效果 Pass + Lighting）自动生成，此处不再手动
            // 转换——否则 RT Pass 已把深度转成 Read 后再次 Write→Read 会 oldLayout 不匹配。

            // 委托 LightingPass 执行（RT 纹理传入 null 时回退到占位/屏幕空间路径）
            m_Lighting.Render(c,
                m_GBuffer->GetAlbedo(), m_GBuffer->GetNormal(),
                m_GBuffer->GetEmissive(),
                m_GBuffer->GetDepth(), m_GBuffer->GetWorldPos(),
                nullptr, nullptr, nullptr, nullptr,  // 无 CSM/Spot 阴影贴图（RT 阴影替代）
                m_LightBuffers[m_CurrentFrameSlot].get(),
                m_ShadowBuffers[m_CurrentFrameSlot].get(),
                nullptr,  // 无 SSAO（RT AO 替代）
                nullptr, nullptr,  // 无 SSGI（RT GI 替代）
                nullptr, nullptr,  // 无 SSR（RT 反射替代）
                m_DDGI.GetProbeBuffer(),
                nullptr,  // 无 Clustered
                m_LightGridBuffer.get(), m_LightIndexListBuffer.get(),
                &m_CachedLights,
                lightingShadowTex, lightingReflTex, lightingAOTex, lightingGITex,  // RT 纹理（降噪后）
                float4(camera.position, 0), 1.0f, lightCount, w, h);
        });

    // ── DDGI 前帧 HDR 捕获 ──
    rg.AddPass("DDGI_CaptureHDR", {{hdrC, ResourceAccess::Read}}, {},
        [&](rhi::IRHICommandList* c) {
            if (m_DDGI.IsEnabled()) {
                m_DDGI.CaptureHDR(c, m_Lighting.GetHDRTarget());
            }
        });

    // ── AutoExposure（Compute reduction → SSBO，Bloom 之前）──
    rg.AddPass("AutoExposure", {{hdrC, ResourceAccess::Read}}, {},
        [&](rhi::IRHICommandList* c) {
            m_PostProcess.GetAutoExposure().SetInput(
                m_Lighting.GetHDRTarget(), m_Lighting.GetHDRSampler());
            m_PostProcess.GetAutoExposure().Render(c);
            // 恢复 graphics pipeline state（compute dispatch 后 m_CurrentRenderPass 为空）
            c->SetPipeline(m_Lighting.GetPSO());
        },
        RGPassQueue::Compute);

    // ── 后处理链：Bloom → ToneMap → FXAA（与 DeferredPipeline 一致的 RP 状态管理）──
    bool bloomActive = m_PostProcess.GetBloom().IsEnabled()
                       && m_PostProcess.GetBloom().GetOutput() != nullptr;
    bool useFXAA  = IsFXAAEnabled();
    bool needLDR  = useFXAA;  // HybridRT 终端 AA 为 FXAA，需要 LDR 中间纹理

    // Bloom：显式声明对 hdrC 的读取依赖 → RG 自动插入 RenderTarget→ShaderResource
    // 屏障，pass 内不再手动 PipelineBarrier（避免与 RG 屏障重复转换）。
    ResourceHandle bloomOut = kInvalidHandle;
    if (bloomActive) {
        bloomOut = rg.ImportTexture("Bloom_Out", m_PostProcess.GetBloom().GetOutput());
        rg.AddPass("Bloom", {{hdrC, ResourceAccess::Read}},
            {{bloomOut, ResourceAccess::Write}},
            [&](rhi::IRHICommandList* c) {
                m_PostProcess.GetBloom().SetInput(
                    m_Lighting.GetHDRTarget(), m_Lighting.GetHDRSampler());
                m_PostProcess.GetBloom().Render(c);
            });
    }

    // ToneMap：声明输入依赖（Bloom 输出或 HDR），保证 RG 将其排在 Lighting/Bloom
    // 之后——否则 LIFO 拓扑排序会把 ToneMap 排到帧首，采样上一帧 HDR（整帧滞后一帧）。
    auto ldrTarget = rg.ImportTexture("LDR", m_PostProcess.GetLDRTarget());
    std::vector<PassResource> toneMapReads;
    if (bloomActive && bloomOut != kInvalidHandle)
        toneMapReads.push_back({bloomOut, ResourceAccess::Read});
    else
        toneMapReads.push_back({hdrC, ResourceAccess::Read});
    rg.AddPass("ToneMap", std::move(toneMapReads),
        {{needLDR ? ldrTarget : backBuf, ResourceAccess::Write}},
        [&, w, h, needLDR, bloomActive](rhi::IRHICommandList* c) {
            if (bloomActive) {
                m_PostProcess.GetToneMap()->SetInput(
                    m_PostProcess.GetBloom().GetOutput(),
                    m_PostProcess.GetBloom().GetOutputSampler());
            } else {
                m_PostProcess.GetToneMap()->SetInput(
                    m_Lighting.GetHDRTarget(), m_Lighting.GetHDRSampler());
            }
            m_PostProcess.GetToneMap()->SetExposure(
                m_PostProcess.GetAutoExposure().GetExposure()
                * exp2(camera.exposureBias));
            // PreBind 调用 SetPipeline → 设置 m_CurrentRenderPass 为 BGRA8 兼容格式
            m_PostProcess.GetToneMap()->PreBind(c);
            if (needLDR) {
                rhi::ClearValue clr{};
                c->BeginOffscreenPass(m_PostProcess.GetLDRTarget()->GetNativeHandle(),
                    m_PostProcess.GetLDRDummyDepth()->GetNativeHandle(), w, h, &clr, false);
                m_PostProcess.GetToneMap()->Render(c);
                c->EndOffscreenPass();
            } else {
                c->BeginRenderPass(1, rhi::Format::BGRA8_UNORM);
                m_PostProcess.GetToneMap()->Render(c);
                c->EndRenderPass();
            }
        });

    // FXAA: LDR → BackBuffer（先 ToneMap PreBind 保证 RP 兼容，再进入 SwapChain RP）
    // 已声明对 ldrTarget 的读取依赖，RG 自动插入 RenderTarget→ShaderResource 屏障，
    // pass 内不再重复 PipelineBarrier。
    if (useFXAA) {
        rg.AddPass("FXAA", {{ldrTarget, ResourceAccess::Read}}, {{backBuf, ResourceAccess::Write}},
            [&](rhi::IRHICommandList* c) {
                m_PostProcess.GetFXAA()->SetInput(
                    m_PostProcess.GetLDRTarget(), m_PostProcess.GetLDRSampler());
                // 关键：用 ToneMap 的 2 附件 RP 进入 BeginRenderPass，
                // 确保 SwapChain Framebuffer（BGRA8+D32）与 RP 兼容
                m_PostProcess.GetToneMap()->PreBind(c);
                c->BeginRenderPass(1, rhi::Format::BGRA8_UNORM);
                m_PostProcess.GetFXAA()->Render(c);
                c->EndRenderPass();
            });
    }

    m_PrevViewProj = m_CurrViewProj;
}

} // namespace he::render
