#pragma once

#include "Pipeline/IRenderPipeline.h"
#include "Pipeline/GBufferRenderer.h"
#include "Pipeline/LightingPass.h"
#include "PostProcess/PostProcessChain.h"
#include "Pipeline/RTPass.h"
#include "RT/RTShadowPass.h"
#include "RT/RTAOPass.h"
#include "RT/RTReflectionPass.h"
#include "RT/RTGIPass.h"
#include "Pipeline/GPUCulling.h"
#include "Pipeline/GPUScene.h"
#include "Pipeline/MeshBatcher.h"
#include "Pipeline/ClusteredShading.h"
#include "Pipeline/ParticleRenderer.h"
#include "GI/GI_DDGI.h"
#include "RHI/RHI.h"
#include "RenderGraph.h"
#include "Profiler/ProfilerManager.h"
#include "Profiler/ProfilerPanel.h"
#include <memory>
#include <vector>

namespace he::render {

// ============================================================
// HybridRTPipeline — 混合 Ray Tracing 管线（GBuffer + RT 效果）
//
// 与 DeferredPipeline 平行，共享 GBufferRenderer / LightingPass /
// PostProcessChain 三大组件。用硬件 RT 替代屏幕空间效果：
//   RT Shadow    → 替代 CSM
//   RT Reflection → 替代 SSR
//   RT AO        → 替代 SSAO
//   RT GI        → 替代 SSGI
//   DDGI         → 保留（远距离低频 GI）
//
// 渲染流程（BuildFrameGraph）：
//   [GPU_Cull] → [AS_Build] → GB_Clear → [RT_Shadow] → DDGI_Update
//   → [RT_AO] → [RT_Reflection + 降噪] → [RT_GI + 降噪]
//   → Lighting（读取 GBuffer + RT 效果纹理 + DDGI）
//   → DDGI_CaptureHDR → AutoExposure → [Bloom] → ToneMap → FXAA → BackBuffer
//
// 通过 r.Pipeline.Mode CVar / 02.Cube renderMode 切换 (0=Forward, 1=Deferred, 2=HybridRT)
// ============================================================
class HybridRTPipeline : public IRenderPipeline {
    HE_DECLARE_NON_COPYABLE(HybridRTPipeline);

public:
    HybridRTPipeline()  = default;
    ~HybridRTPipeline() override = default;

    bool Initialize(rhi::IRHIDevice* device) override;
    void Shutdown() override;
    void NextFrame() override;
    void OnResize(u32 width, u32 height) override;
    const char* GetName() const override { return "HybridRTPipeline"; }

    void Render(rhi::IRHICommandList* cmd, he::World& world,
                he::SceneGraph& sg, const CameraData& camera,
                float deltaTime = 0.016f) override;

    // ── 访问器（供 02.Cube 的 ImGui 调用）──
    void SetSwapChain(rhi::IRHISwapChain* sc) { m_SwapChain = sc; }
    GBufferRenderer*      GetGBuffer()          { return m_GBuffer.get(); }
    LightingPass*         GetLighting()         { return &m_Lighting; }
    PostProcessChain*     GetPostProcess()      { return &m_PostProcess; }
    RTPass*               GetRTPass()           { return m_RTPass.get(); }
    RTShadowPass*         GetRTShadow()         { return m_RTShadow.get(); }
    RTAOPass*             GetRTAO()             { return m_RTAO.get(); }
    RTReflectionPass*     GetRTReflection()     { return m_RTReflection.get(); }
    RTGIPass*             GetRTGI()             { return m_RTGI.get(); }
    GI_DDGI*              GetDDGI()             { return &m_DDGI; }

    // RT 效果开关（供 ImGui / CVar 控制）
    void SetRTShadowEnabled(bool e) { m_RTShadowEnabled = e; }
    bool IsRTShadowEnabled() const  { return m_RTShadowEnabled; }
    void SetRTAOEnabled(bool e) { m_RTAOEnabled = e; }
    bool IsRTAOEnabled() const  { return m_RTAOEnabled; }
    void SetRTReflectionEnabled(bool e) { m_RTReflectionEnabled = e; }
    bool IsRTReflectionEnabled() const  { return m_RTReflectionEnabled; }
    void SetRTGIEnabled(bool e) { m_RTGIEnabled = e; }
    bool IsRTGIEnabled() const  { return m_RTGIEnabled; }
    ClusteredShading&     GetClusteredShading() { return m_ClusteredShading; }
    GPUCulling&           GetGPUCulling()       { return m_GPUCulling; }
    ParticleRenderer&     GetParticleRenderer() { return m_ParticleRenderer; }
    ProfilerManager&      GetProfiler()         { return m_Profiler; }
    ProfilerPanel&        GetProfilerPanel()    { return m_ProfilerPanel; }
    BloomPass&            GetBloom()            { return m_PostProcess.GetBloom(); }
    DOFPass&              GetDOF()              { return m_PostProcess.GetDOF(); }
    MotionBlurPass&       GetMotionBlur()       { return m_PostProcess.GetMotionBlur(); }
    AutoExposurePass&     GetAutoExposure()     { return m_PostProcess.GetAutoExposure(); }
    ColorGradingPass&     GetColorGrading()     { return m_PostProcess.GetColorGrading(); }
    ToneMapPass*          GetToneMap()          { return m_PostProcess.GetToneMap(); }
    void EnableFXAA(bool e) { m_PostProcess.EnableFXAA(m_Device, m_Width, m_Height, e); }
    bool IsFXAAEnabled() const { return m_PostProcess.IsFXAAEnabled(); }

    // GBuffer 模式
    void SetGBufferMode(GBufferRenderer::Mode m);
    GBufferRenderer::Mode GetGBufferMode() const;

    void AddParticleComponent(u32 id) { m_ParticleComponentIDs.push_back(id); }

    rhi::IRHIBuffer* GetCurrentObjectBuffer() { return m_ObjectBuffers[m_CurrentFrameSlot].get(); }

private:
    void BuildFrameGraph(RenderGraph& rg, he::World& world,
                         he::SceneGraph& sg, const CameraData& camera);
    void CollectLights(he::World& world, he::SceneGraph& sg,
                       const CameraData& camera, u32& outLightCount);

    rhi::IRHIDevice*    m_Device    = nullptr;
    rhi::IRHISwapChain* m_SwapChain = nullptr;

    // ── 共享组件 ──
    std::unique_ptr<GBufferRenderer> m_GBuffer;
    LightingPass    m_Lighting;
    PostProcessChain m_PostProcess;

    // ── RT 基础设施 ──
    std::unique_ptr<RTPass> m_RTPass;          // AS 构建 + TLAS + 场景资源（set1/set2）
    std::unique_ptr<RTShadowPass> m_RTShadow;  // RT 阴影效果 Pass
    std::unique_ptr<RTAOPass> m_RTAO;          // RT AO 效果 Pass
    std::unique_ptr<RTReflectionPass> m_RTReflection;  // RT 反射效果 Pass
    std::unique_ptr<RTGIPass> m_RTGI;          // RT GI 效果 Pass
    bool m_RTEnabled = false;
    bool m_RTShadowEnabled = true;      // RT 阴影开关（ImGui/CVar 控制）
    bool m_RTAOEnabled = true;          // RT AO 开关
    bool m_RTReflectionEnabled = true;  // RT 反射开关
    bool m_RTGIEnabled = true;          // RT GI 开关
    bool m_SceneMaterialBuilt = false;  // 场景材质纹理是否已构建（延迟到首帧）

    // ── 专有 GI ──
    GI_DDGI m_DDGI;

    // ── GPU Driven 基础设施 ──
    GPUCulling m_GPUCulling;
    GPUScene   m_GPUScene;
    MeshBatcher m_MeshBatcher;
    bool       m_BatchBuilt = false;
    ClusteredShading m_ClusteredShading;
    ParticleRenderer m_ParticleRenderer;
    std::vector<u32> m_ParticleComponentIDs;
    std::unique_ptr<SceneRenderer> m_SceneRenderer;

    // ── 聚集着色缓冲 ──
    std::unique_ptr<rhi::IRHIBuffer> m_LightGridBuffer;
    std::unique_ptr<rhi::IRHIBuffer> m_LightIndexListBuffer;
    std::vector<GPULight> m_CachedLights;

    // ── 三缓冲 SSBO ──
    std::unique_ptr<rhi::IRHIBuffer> m_LightBuffers[MAX_FRAMES_IN_FLIGHT];
    std::unique_ptr<rhi::IRHIBuffer> m_ObjectBuffers[MAX_FRAMES_IN_FLIGHT];
    std::unique_ptr<rhi::IRHIBuffer> m_ShadowBuffers[MAX_FRAMES_IN_FLIGHT];
    u32 m_CurrentFrameSlot = 0;
    std::vector<u32> m_GPUVisibleIndices;

    // ── Profiler ──
    ProfilerManager m_Profiler;
    ProfilerPanel   m_ProfilerPanel;

    // 相机矩阵缓存（当前帧 + 上一帧，velocity 计算用）
    float4x4 m_PrevViewProj = float4x4(1.0f);
    float4x4 m_CurrViewProj = float4x4(1.0f);
    u32 m_FrameIndex = 0;  // 帧索引（RT 时域抖动用）

    u32 m_Width = rhi::kDefaultBackBufferWidth, m_Height = rhi::kDefaultBackBufferHeight;
    bool m_Ready = false;
};

} // namespace he::render
