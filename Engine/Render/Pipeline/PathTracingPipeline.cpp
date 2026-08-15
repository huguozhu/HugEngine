// ============================================================
// PathTracingPipeline.cpp — 全路径追踪管线实现（Level 2: PT 参考模式）
//
// 帧图：AS_Build → PT_Render（NEE+MIS+俄罗斯轮盘赌）
//   → [ReSTIR DI（可选）] → PT_Denoise（时域累积）→ ToneMap → FXAA
//
// 崩溃防御要点（沿用 HybridRTPipeline 的教训）：
//   1. 每个 Compute dispatch 末尾恢复 graphics pipeline（ReSTIR 后 SetPipeline）。
//   2. ToneMap PreBind + LDR 虚拟深度与 LDR 目标同尺寸。
//   3. FXAA → BackBuffer 前通过 ToneMap PreBind 保证 RP 兼容。
// ============================================================
#include "Pipeline/PathTracingPipeline.h"
#include "Pipeline/PTQualityCVars.h"
#include "Pipeline/PhysicalLight.h"
#include "RT/ReSTIRPass.h"
#include "Scene/World.h"
#include "Scene/SceneGraph.h"
#include "Scene/LightComponent.h"
#include "Scene/MeshComponent.h"
#include "Scene/PhysicalSkyComponent.h"
#include "Core/Log.h"
#include "Core/Assert.h"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <cstring>
#include <algorithm>
#include <cmath>   // std::acos（相机旋转量）

namespace he::render {

// PT flags（与 PT_Full.rgen.slang 的 PT_FLAG_* 一致）
static constexpr u32 kPTFlag_ReSTIR   = 1u << 0;
static constexpr u32 kPTFlag_MIS      = 1u << 1;
static constexpr u32 kPTFlag_Roulette = 1u << 2;
static constexpr u32 kPTFlag_NEE      = 1u << 3;

// 相机运动 → 时域降噪混合权重缩放：约 0.33m 平移或 0.33rad(~19°) 旋转 → 混合抬到 1.0
static constexpr float kMotionBlendScale = 3.0f;

bool PathTracingPipeline::Initialize(rhi::IRHIDevice* device) {
    m_Device = device;
    m_Width  = rhi::kDefaultBackBufferWidth;
    m_Height = rhi::kDefaultBackBufferHeight;

    // ── 后处理（ToneMap → LDR → FXAA）──
    m_PostProcess.Initialize(device, m_Width, m_Height);
    m_PostProcess.EnableFXAA(device, m_Width, m_Height, true);  // 终端 AA 保证 RP 状态正确

    // ToneMap HDR 输入采样器（线性，与 LightingPass 的 HDR sampler 同款）
    {
        rhi::SamplerDesc sd;
        sd.minFilter = sd.magFilter = rhi::FilterMode::Linear;
        sd.addressU  = sd.addressV  = rhi::AddressMode::ClampToEdge;
        m_LinearSampler = device->CreateSampler(sd);
        if (!m_LinearSampler) HE_CORE_WARN("PathTracingPipeline: 线性采样器创建失败");
    }

    // ── GPU 粒子系统（粒子 Billboard 复合到 PT HDR，降噪之前）──
    // 粒子深度附件：D32 深度缓冲，每帧清成远平面 1.0，保证粒子始终通过深度测试
    // （PT 场景深度由 RayGen 输出，未光栅化到深度附件，粒子不与场景做深度遮挡）
    {
        rhi::TextureDesc dd;
        dd.format = rhi::Format::D32_FLOAT;
        dd.width  = m_Width;
        dd.height = m_Height;
        dd.usage  = rhi::TextureUsage::DepthStencil | rhi::TextureUsage::ShaderResource;
        m_ParticleDepth = device->CreateTexture(dd);

        rhi::SamplerDesc sd;
        sd.minFilter = sd.magFilter = rhi::FilterMode::Nearest;   // 深度 Load 点采样
        sd.addressU  = sd.addressV  = rhi::AddressMode::ClampToEdge;
        m_ParticleDepthSampler = device->CreateSampler(sd);

        m_ParticleRenderer.Initialize(device);
        m_ParticleRenderer.SetSceneDepth(m_ParticleDepth.get(), m_ParticleDepthSampler.get());
    }

    // ── 三缓冲光源 SSBO ──
    for (u32 i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        m_LightBuffers[i] = device->CreateBuffer(
            {sizeof(GPULight) * MAX_LIGHTS, rhi::BufferUsage::Storage});
    }

    // ── Ray Tracing 子系统 ──
    m_RTEnabled = device->GetCaps().supportsRayTracing;
    if (m_RTEnabled) {
        // RTPass：AS-only 模式（构建 BLAS/TLAS + 场景资源）
        m_RTPass = std::make_unique<RTPass>();
        if (!m_RTPass->Initialize(device, {}, {})) {
            HE_CORE_WARN("PathTracingPipeline: RTPass 初始化失败，禁用 PT");
            m_RTPass.reset();
            m_RTEnabled = false;
        }
    }
    if (m_RTEnabled) {
        // 全路径追踪 Pass（全分辨率，4 输出 UAV）
        m_PT = std::make_unique<PTPass>();
        if (!m_PT->Initialize(device, m_Width, m_Height)) {
            HE_CORE_ERROR("PathTracingPipeline: PTPass 初始化失败（设备 maxPayloadSize 可能 < 48B），禁用 PT");
            m_PT.reset();
            m_RTEnabled = false;
        }
    }
    if (m_RTEnabled) {
        // ReSTIR DI Pass（始终创建资源；开关由 cvPTReSTIR 控制是否 dispatch）
        m_ReSTIR = std::make_unique<ReSTIRPass>();
        if (!m_ReSTIR->Initialize(device, m_Width, m_Height)) {
            HE_CORE_WARN("PathTracingPipeline: ReSTIRPass 初始化失败，ReSTIR DI 禁用");
            m_ReSTIR.reset();
        }

        // STBN 时空蓝噪声（PT RayGen + ReSTIR 三 Pass 共用，无采样器 Load 采样）
        m_STBN = std::make_unique<STBNTexture>();
        if (!m_STBN->Initialize(device)) {
            // shader 随机已彻底走 STBN（无 Hash 回退），失败后继续 dispatch 会让
            // g_BlueNoise.Load() 读未绑定描述符（UB）→ 直接禁用 RT。
            m_STBN.reset();
            m_RTEnabled = false;
            HE_CORE_ERROR("STBNTexture 初始化失败，禁用 PT（无 Hash 回退）");
        }

        // 时域降噪（RGBA16F，ptDepth 为米制 viewZ → depthThreshold=1.0）
        m_PTDenoiser = std::make_unique<RTDenoiser>();
        RTDenoiser::Config cfg;
        cfg.format          = rhi::Format::RGBA16_FLOAT;
        cfg.width           = m_Width;
        cfg.height          = m_Height;
        cfg.temporalBlend   = cvPTDenoiseBlend.Get();
        cfg.depthThreshold  = 1.0f;    // 线性视图深度（米制），非 NDC 深度
        cfg.normalThreshold = 0.85f;
        cfg.debugName       = "PTDenoiser";
        if (!m_PTDenoiser->Initialize(device, cfg)) {
            HE_CORE_WARN("PathTracingPipeline: RTDenoiser 初始化失败，时域降噪禁用");
            m_PTDenoiser.reset();
        }

        // A-Trous 空间滤波（SVGF 风格多迭代边缘感知；配置初值从 CVar 读取）
        m_PTAtrous = std::make_unique<PTAtrousPass>();
        PTAtrousPass::Config acfg;
        acfg.width  = m_Width;
        acfg.height = m_Height;
        acfg.iterations      = (u32)cvPTAtrousIterations.Get();
        acfg.sigmaDepth      = cvPTAtrousSigmaDepth.Get();
        acfg.normalPower     = cvPTAtrousSigmaNormal.Get();
        acfg.sigmaColor      = cvPTAtrousSigmaColor.Get();
        acfg.clampThreshold  = cvPTAtrousClamp.Get();
        if (!m_PTAtrous->Initialize(device, acfg)) {
            HE_CORE_WARN("PathTracingPipeline: PTAtrousPass 初始化失败，空间滤波禁用");
            m_PTAtrous.reset();
        }
    }

    m_Ready = true;
    HE_CORE_INFO("PathTracingPipeline: 初始化完成 ({}x{}, RT={})",
                 m_Width, m_Height, m_RTEnabled ? "on" : "off");
    return true;
}

void PathTracingPipeline::Shutdown() {
    m_PTDenoiser.reset();
    m_PTAtrous.reset();
    m_STBN.reset();
    m_ReSTIR.reset();
    if (m_PT) { m_PT->Shutdown(); m_PT.reset(); }
    if (m_RTPass) { m_RTPass->Shutdown(); m_RTPass.reset(); }
    m_ParticleRenderer.Shutdown(m_Device);
    m_ParticleDepthSampler.reset();
    m_ParticleDepth.reset();
    m_ParticleComponentIDs.clear();
    m_PostProcess.Shutdown();
    m_LinearSampler.reset();
    for (auto& b : m_LightBuffers) b.reset();
    m_Device = nullptr; m_Ready = false;
    HE_CORE_INFO("PathTracingPipeline: shutdown");
}

void PathTracingPipeline::NextFrame() {
    m_CurrentFrameSlot = (m_CurrentFrameSlot + 1) % MAX_FRAMES_IN_FLIGHT;
}

void PathTracingPipeline::OnResize(u32 w, u32 h) {
    if (w == m_Width && h == m_Height) return;
    m_Width = w; m_Height = h;
    m_PostProcess.OnResize(m_Device, w, h);
    if (m_Device && m_RTEnabled) {
        // PT/ReSTIR Pass 重建输出纹理与蓄水池
        if (m_PT) { m_PT->Shutdown(); m_PT->Initialize(m_Device, w, h); }
        if (m_ReSTIR) { m_ReSTIR->Shutdown(); m_ReSTIR->Initialize(m_Device, w, h); }
        if (m_PTDenoiser) m_PTDenoiser->OnResize(w, h);
        if (m_PTAtrous) m_PTAtrous->OnResize(w, h);
    }
    // 粒子深度附件跟随分辨率重建
    if (m_Device) {
        m_ParticleDepth.reset();
        rhi::TextureDesc dd;
        dd.format = rhi::Format::D32_FLOAT;
        dd.width  = w;
        dd.height = h;
        dd.usage  = rhi::TextureUsage::DepthStencil | rhi::TextureUsage::ShaderResource;
        m_ParticleDepth = m_Device->CreateTexture(dd);
        m_ParticleRenderer.SetSceneDepth(m_ParticleDepth.get(), m_ParticleDepthSampler.get());
    }
}

// ── PT 质量开关（CVar 薄封装，供 ImGui / 02.Cube 调用；实际状态存于 r.PT.* CVar）──
void PathTracingPipeline::SetPTDenoise(bool e) { cvPTDenoise.Set(e); }
bool PathTracingPipeline::IsPTDenoise() const  { return cvPTDenoise.Get(); }
void PathTracingPipeline::SetPTReSTIR(bool e)  { cvPTReSTIR.Set(e); }
bool PathTracingPipeline::IsPTReSTIR() const   { return cvPTReSTIR.Get(); }
void PathTracingPipeline::SetPTMIS(bool e)     { cvPTMIS.Set(e); }
bool PathTracingPipeline::IsPTMIS() const      { return cvPTMIS.Get(); }
void PathTracingPipeline::SetPTRoulette(bool e){ cvPTRoulette.Set(e); }
bool PathTracingPipeline::IsPTRoulette() const { return cvPTRoulette.Get(); }
void PathTracingPipeline::SetPTSampleCount(i32 v) { cvPTSampleCount.Set(v); }
i32  PathTracingPipeline::GetPTSampleCount() const { return cvPTSampleCount.Get(); }
void PathTracingPipeline::SetPTMaxBounces(i32 v) { cvPTMaxBounces.Set(v); }
i32  PathTracingPipeline::GetPTMaxBounces() const { return cvPTMaxBounces.Get(); }

// ============================================================
// Render — 主渲染入口
// ============================================================
void PathTracingPipeline::Render(rhi::IRHICommandList* cmd, he::World& world,
                                 he::SceneGraph& sg, const CameraData& camera,
                                 float deltaTime) {
    he::SyncPhysicalSkyToSun(world);  // 物理天空太阳→方向光同步（阴影/光照收集前）
    if (!m_SwapChain || !m_Device) {
        HE_CORE_ERROR("PathTracingPipeline::Render: SwapChain 或 Device 未设置");
        return;
    }

    // 时域降噪混合因子（CVar 热更新）
    if (m_PTDenoiser)
        m_PTDenoiser->SetTemporalBlend(std::clamp(cvPTDenoiseBlend.Get(), 0.0f, 1.0f));

    // ── 粒子模拟 (Compute，在 RenderGraph 之前) ──
    float4x4 viewProj = camera.GetViewProjMatrix();
    for (u32 pid : m_ParticleComponentIDs) {
        m_ParticleRenderer.DispatchCompute(cmd, pid, deltaTime, viewProj);
    }

    m_FrameIndex++;  // 帧索引递增（PT 抖动 / ReSTIR 种子）

    RenderGraph rg;
    rg.SetSwapChain(m_SwapChain);

    BuildFrameGraph(rg, world, sg, camera);

    rg.Compile();
    rg.Execute(cmd, m_Device);

    // 帧末交换 ReSTIR 历史槽位（读取槽 ↔ 写入槽）
    if (m_ReSTIR) m_ReSTIR->EndFrame();
}

// ============================================================
// CollectLights — 收集场景光源数据到当前帧槽位 SSBO
//（复制 HybridRTPipeline 实现：色温 + 物理光源模式）
// ============================================================
void PathTracingPipeline::CollectLights(he::World& world, he::SceneGraph& sg,
                                        const CameraData& camera, u32& outLightCount) {
    (void)camera;
    outLightCount = 0;
    u32 lightCount = 0;

    auto cl = [&](he::Entity e, he::LightComponent& lc) {
        if (lightCount >= MAX_LIGHTS || !lc.enabled) return;
        GPULight gl{};
        gl.shadowIndex = -1;  // PT 无传统阴影系统（阴影包含在路径中）
        gl.shadowRadius = lc.shadowRadius;

        // 色温 → RGB 颜色（叠加到 color 滤镜色上）
        float3 lightColor = lc.color;
        if (lc.colorTemperature > 0.0f) {
            lightColor *= render::KelvinToRGB(lc.colorTemperature);
        }
        gl.colorIntensity = float4(lightColor, lc.intensity);

        // 物理光源模式（luminousIntensity>0 或 illuminance>0 时启用）
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
void PathTracingPipeline::BuildFrameGraph(RenderGraph& rg, he::World& world,
                                          he::SceneGraph& sg, const CameraData& camera) {
    if (m_SwapChain) rg.SetSwapChain(m_SwapChain);
    u32 w = m_Width, h = m_Height;
    auto backBuf = rg.ImportBackBuffer();

    // ── 帧首：更新相机矩阵（velocity / 时域降噪用）──
    m_CurrViewProj = camera.GetViewProjMatrix();
    static bool firstFrame = true;
    if (firstFrame) { m_PrevViewProj = m_CurrViewProj; firstFrame = false; }

    // 相机运动自适应降噪：静止时低混合高质量累积；运动时抬升混合缩短历史拖影。
    // （快速旋转时 PT 无累积、图像变噪，为路径追踪固有取舍，停下后自动恢复）
    if (m_PTDenoiser) {
        if (m_CamInited) {
            float posDelta = glm::length(camera.position - m_PrevCamPos);
            float fwdDot   = std::clamp(glm::dot(camera.forward, m_PrevCamFwd), -1.0f, 1.0f);
            float rotDelta = std::acos(fwdDot);   // 相机旋转量（弧度）
            float motionBlend = std::clamp((posDelta + rotDelta) * kMotionBlendScale, 0.0f, 1.0f);
            m_PTDenoiser->SetMotionBlend(motionBlend);
        }
        m_PrevCamPos = camera.position;
        m_PrevCamFwd = camera.forward;
        m_CamInited  = true;
    }

    // ── AS Build — BLAS/TLAS 构建 ──
    if (m_RTEnabled && m_RTPass && m_RTPass->IsValid()) {
        rg.AddPass("AS_Build", {}, {},
            [this, &world, &sg](rhi::IRHICommandList* c) {
                m_RTPass->BuildAS(c, world, sg);
            });

        // 场景材质纹理（4×N，PT ClosestHit 用）：首帧延迟构建一次
        if (!m_SceneMaterialBuilt) {
            if (m_RTPass->BuildSceneMaterialTexture(m_Device, world)) {
                m_SceneMaterialBuilt = true;
            } else {
                HE_CORE_WARN("PathTracingPipeline: 场景材质纹理构建失败，PT 材质查询不可用");
            }
        }
    }

    // ── 收集光源（PT 与 ReSTIR 共用当前帧槽位数据）──
    u32 lightCount = 0;
    CollectLights(world, sg, camera, lightCount);

    // ── ReSTIR 蓄水池可用判定：首帧无历史；光源数变化 → 历史失效 ──
    bool reservoirReady = false;
    if (m_ReSTIR && m_ReSTIR->IsValid() && m_FrameIndex > 1 && lightCount == m_PrevLightCount) {
        reservoirReady = true;
    }
    m_PrevLightCount = lightCount;

    // ── PT Render — 全路径追踪（输出 HDR + GBuffer）──
    bool useReSTIR = cvPTReSTIR.Get() && reservoirReady;
    u32 ptFlags = 0;
    if (useReSTIR)       ptFlags |= kPTFlag_ReSTIR;
    if (cvPTMIS.Get())   ptFlags |= kPTFlag_MIS;
    if (cvPTRoulette.Get()) ptFlags |= kPTFlag_Roulette;
    ptFlags |= kPTFlag_NEE;   // NEE 恒开（ReSTIR 关闭时的直接光照基础）

    rhi::IRHITexture* ptHDR    = m_PT ? m_PT->GetHDR() : nullptr;
    rhi::IRHITexture* ptDepth  = m_PT ? m_PT->GetDepth() : nullptr;
    rhi::IRHITexture* ptNormal = m_PT ? m_PT->GetNormal() : nullptr;
    rhi::IRHITexture* ptVel    = m_PT ? m_PT->GetVelocity() : nullptr;
    rhi::IRHITexture* ptAlbedo = m_PT ? m_PT->GetAlbedoMetallic() : nullptr;

    ResourceHandle ptHDRHandle = kInvalidHandle, ptDepthHandle = kInvalidHandle;
    ResourceHandle ptNormalHandle = kInvalidHandle, ptVelHandle = kInvalidHandle;
    ResourceHandle ptAlbedoHandle = kInvalidHandle;

    if (m_RTEnabled && m_PT && m_PT->IsValid() && m_RTPass) {
        ptHDRHandle    = rg.ImportTexture("PT_HDR", ptHDR);
        ptDepthHandle  = rg.ImportTexture("PT_Depth", ptDepth);
        ptNormalHandle = rg.ImportTexture("PT_Normal", ptNormal);
        ptVelHandle    = rg.ImportTexture("PT_Velocity", ptVel);
        ptAlbedoHandle = rg.ImportTexture("PT_AlbedoMetallic", ptAlbedo);

        rg.AddPass("PT_Render",
            {},
            {{ptHDRHandle, ResourceAccess::UAV}, {ptDepthHandle, ResourceAccess::UAV},
             {ptNormalHandle, ResourceAccess::UAV}, {ptVelHandle, ResourceAccess::UAV},
             {ptAlbedoHandle, ResourceAccess::UAV}},
            [this, &camera, lightCount, ptFlags, useReSTIR](rhi::IRHICommandList* c) {
                PTRenderContext ctx;
                ctx.invViewProj  = glm::inverse(camera.GetViewProjMatrix());
                ctx.prevViewProj = m_PrevViewProj;
                ctx.cameraPos    = camera.position;
                ctx.frameIndex   = m_FrameIndex;
                ctx.maxBounces   = std::clamp((u32)cvPTMaxBounces.Get(), 1u, 8u);
                ctx.sampleCount  = std::clamp((u32)cvPTSampleCount.Get(), 1u, 8u);
                ctx.skyIntensity = std::max(cvPTSkyIntensity.Get(), 0.0f);
                ctx.flags        = ptFlags;
                ctx.lightBuffer  = m_LightBuffers[m_CurrentFrameSlot].get();
                ctx.lightCount   = lightCount;
                ctx.finalReservoir = useReSTIR && m_ReSTIR ? m_ReSTIR->GetFinalReservoir() : nullptr;
                ctx.sceneMaterialTex = m_RTPass->GetSceneMaterialTexture();
                ctx.sceneTriangleNormals = m_RTPass->GetSceneTriangleNormals();
                ctx.blueNoise = m_STBN ? m_STBN->GetTexture() : nullptr;
                m_PT->Execute(c, m_RTPass->GetTLAS(), ctx);
            });
    }

    // ── Particle Render（粒子 Billboard 写入 PT HDR，PT_Render 之后、降噪之前）──
    // 粒子作为 HDR 内容的一部分流向时域降噪 + ToneMap（与 Deferred 的粒子路径一致）。
    // 深度附件每帧清成远平面：粒子始终通过深度测试（PT 场景深度未光栅化到该附件）。
    if (!m_ParticleComponentIDs.empty() && m_PT && m_PT->IsValid()) {
        for (u32 pid : m_ParticleComponentIDs) {
            rg.AddPass("ParticleRender",
                {{ptHDRHandle, ResourceAccess::Read}},
                {{ptHDRHandle, ResourceAccess::Write}},
                [this, pid, &camera, w, h](rhi::IRHICommandList* c) {
                    // 先把粒子深度附件清成远平面（内容确定 + 保证粒子通过深度测试）
                    c->ClearDepthStencil(m_ParticleDepth.get(), 1.0f);
                    // 先设置粒子 PSO（BeginOffscreenPass 需要预绑定 PSO 来创建 RenderPass）
                    c->SetPipeline(m_ParticleRenderer.GetRenderPSO());
                    c->BeginOffscreenPass(
                        m_PT->GetHDR()->GetNativeHandle(),
                        m_ParticleDepth->GetNativeHandle(),
                        w, h, nullptr, false);  // LoadOp::Load 保留 PT 结果
                    c->SetViewport({0, (float)h, (float)w, -(float)h, 0, 1});
                    c->SetScissor({0, 0, w, h});
                    m_ParticleRenderer.Render(c, pid, camera.GetViewProjMatrix(), camera);
                    c->EndOffscreenPass();
                });
        }
    }

    // ── ReSTIR DI（Init → Temporal → Spatial 顺序 dispatch，单 RG Pass）──
    // 读 PT 输出（RAW 依赖 → RG 强制排在 PT_Render 之后）；SSBO 对 RG 不可见，
    // 同一命令缓冲内按提交序顺序执行（无重排风险，无需 token 链）。
    if (cvPTReSTIR.Get() && m_ReSTIR && m_ReSTIR->IsValid()
        && ptDepthHandle != kInvalidHandle && ptAlbedoHandle != kInvalidHandle) {
        rg.AddPass("ReSTIR_DI",
            {{ptDepthHandle, ResourceAccess::Read},
             {ptNormalHandle, ResourceAccess::Read},
             {ptVelHandle, ResourceAccess::Read},
             {ptAlbedoHandle, ResourceAccess::Read}},
            {},
            [this, &camera, lightCount, reservoirReady, ptDepth, ptNormal, ptVel](
                rhi::IRHICommandList* c) {
                ReSTIRDispatchContext ctx;
                ctx.invViewProj  = glm::inverse(camera.GetViewProjMatrix());
                ctx.cameraPos    = camera.position;
                ctx.frameIndex   = m_FrameIndex;
                ctx.lightBuffer  = m_LightBuffers[m_CurrentFrameSlot].get();
                ctx.lightCount   = lightCount;
                ctx.historyValid = reservoirReady;
                ctx.ptDepth      = ptDepth;
                ctx.ptNormal     = ptNormal;
                ctx.ptVelocity   = ptVel;
                ctx.ptAlbedo     = m_PT ? m_PT->GetAlbedoMetallic() : nullptr;
                ctx.blueNoise    = m_STBN ? m_STBN->GetTexture() : nullptr;
                ctx.candidateCount = std::clamp((u32)cvPTRestirCandidates.Get(), 1u, 64u);
                ctx.spatialRadius  = std::clamp((u32)cvPTRestirRadius.Get(), 1u, 8u);
                ctx.spatialSamples = std::clamp((u32)cvPTRestirSamples.Get(), 1u, 16u);
                ctx.maxDistance    = 1.0f;
                m_ReSTIR->Execute(c, ctx);
                // Compute dispatch 后恢复 graphics pipeline，
                // 确保后续 pass 的 SetPipeline / BeginOffscreenPass 状态正确
                if (m_PostProcess.GetToneMap())
                    c->SetPipeline(m_PostProcess.GetToneMap()->GetPSO());
            });
    }

    // ── PT Denoise — 时域累积（velocity 重投影 + 去遮挡检测）──
    rhi::IRHITexture* denoisedTex = nullptr;
    ResourceHandle denoisedHandle = kInvalidHandle;
    bool useDenoise = cvPTDenoise.Get() && m_PTDenoiser && m_PTDenoiser->IsReady()
                      && ptHDRHandle != kInvalidHandle;
    if (useDenoise) {
        denoisedTex = m_PTDenoiser->GetOutput();
        denoisedHandle = rg.ImportTexture("PT_Denoised", denoisedTex);
        rg.AddPass("PT_Denoise",
            {{ptHDRHandle, ResourceAccess::Read},
             {ptDepthHandle, ResourceAccess::Read},
             {ptNormalHandle, ResourceAccess::Read},
             {ptVelHandle, ResourceAccess::Read}},
            {{denoisedHandle, ResourceAccess::Write}},
            [this, ptHDR, ptDepth, ptNormal, ptVel](rhi::IRHICommandList* c) {
                m_PTDenoiser->SetInputs(ptHDR, ptDepth, ptNormal, ptVel);
                m_PTDenoiser->Render(c);
            });
    }

    // ── PT A-Trous — 空间滤波（时域累积之后；深度/法线边缘感知 + 方差钳制）──
    rhi::IRHITexture* atrousTex = nullptr;
    ResourceHandle atrousHandle = kInvalidHandle;
    bool useAtrous = cvPTAtrous.Get() && m_PTAtrous && m_PTAtrous->IsReady()
                     && denoisedHandle != kInvalidHandle && ptDepthHandle != kInvalidHandle;
    if (useAtrous) {
        atrousTex = m_PTAtrous->GetOutput();
        atrousHandle = rg.ImportTexture("PT_AtrousOut", atrousTex);
        rg.AddPass("PT_Atrous",
            {{denoisedHandle, ResourceAccess::Read},
             {ptDepthHandle, ResourceAccess::Read},
             {ptNormalHandle, ResourceAccess::Read}},
            {{atrousHandle, ResourceAccess::Write}},
            [this, denoisedTex, ptDepth, ptNormal](rhi::IRHICommandList* c) {
                m_PTAtrous->SetInputs(denoisedTex, ptDepth, ptNormal);
                m_PTAtrous->SetParams((u32)cvPTAtrousIterations.Get(),
                                      cvPTAtrousSigmaDepth.Get(),
                                      cvPTAtrousSigmaNormal.Get(),
                                      cvPTAtrousSigmaColor.Get(),
                                      cvPTAtrousClamp.Get());
                m_PTAtrous->Render(c);
            });
    }

    // 交换链颜色格式同步（HDR=A2B10G10R10，SDR=BGRA8）
    rhi::Format swapFmt = m_SwapChain ? m_SwapChain->GetColorFormat() : rhi::Format::BGRA8_UNORM;
    m_PostProcess.GetToneMap()->SetOutputFormat(swapFmt);
    m_PostProcess.GetToneMap()->SetHDREnabled(swapFmt == rhi::Format::A2B10G10R10_UNORM_PACK32);
    bool isHDR = (swapFmt == rhi::Format::A2B10G10R10_UNORM_PACK32);   // HDR 下禁用 LDR 的 FXAA

    // ── ToneMap（输入：A-Trous 输出优先，其次降噪输出，最后原始 PT HDR）──
    rhi::IRHITexture* toneMapInput = atrousTex ? atrousTex : (denoisedTex ? denoisedTex : ptHDR);
    ResourceHandle toneMapInputHandle = atrousHandle != kInvalidHandle ? atrousHandle
                                      : (denoisedHandle != kInvalidHandle ? denoisedHandle : ptHDRHandle);

    auto ldrTarget = rg.ImportTexture("LDR", m_PostProcess.GetLDRTarget());
    bool useFXAA  = m_PostProcess.IsFXAAEnabled();
    bool needLDR  = useFXAA && !isHDR;   // HDR 下跳过 LDR 中间纹理（FXAA 关闭，ToneMap 直写交换链）

    if (toneMapInputHandle != kInvalidHandle && m_PostProcess.GetToneMap()) {
        rg.AddPass("ToneMap",
            {{toneMapInputHandle, ResourceAccess::Read}},
            {{needLDR ? ldrTarget : backBuf, ResourceAccess::Write}},
            [this, w, h, needLDR, toneMapInput, &camera, swapFmt](rhi::IRHICommandList* c) {
                m_PostProcess.GetToneMap()->SetInput(toneMapInput, m_LinearSampler.get());
                m_PostProcess.GetToneMap()->SetExposure(exp2(camera.exposureBias));
                // PreBind 调用 SetPipeline → 设置 m_CurrentRenderPass 为 BGRA8 兼容格式
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
    }

    // ── FXAA: LDR → BackBuffer（先 ToneMap PreBind 保证 RP 兼容，再进入 SwapChain RP）──
    if (useFXAA && !isHDR && m_PostProcess.GetFXAA()) {  // HDR 下禁用 LDR 的 FXAA（与 A2B10G10R10 后备缓冲不兼容）
        rg.AddPass("FXAA", {{ldrTarget, ResourceAccess::Read}}, {{backBuf, ResourceAccess::Write}},
            [this, swapFmt](rhi::IRHICommandList* c) {
                m_PostProcess.GetFXAA()->SetInput(
                    m_PostProcess.GetLDRTarget(), m_PostProcess.GetLDRSampler());
                // 关键：用 ToneMap 的 2 附件 RP 进入 BeginRenderPass，
                // 确保 SwapChain Framebuffer（BGRA8+D32）与 RP 兼容
                m_PostProcess.GetToneMap()->PreBind(c);
                c->BeginRenderPass(1, swapFmt);
                m_PostProcess.GetFXAA()->Render(c);
                c->EndRenderPass();
            });
    }

    m_PrevViewProj = m_CurrViewProj;
}

} // namespace he::render
