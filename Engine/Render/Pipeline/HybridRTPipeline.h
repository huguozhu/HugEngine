#pragma once

#include "Pipeline/IRenderPipeline.h"
#include "Pipeline/GBufferRenderer.h"
#include "Pipeline/LightingPass.h"
#include "PostProcess/PostProcessChain.h"
#include "Pipeline/RTPass.h"
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
//   RT Shadow  → 替代 CSM
//   RT Reflection → 替代 SSR
//   RT AO      → 替代 SSAO
//   RT GI      → 替代 SSGI
//   DDGI       → 保留（远距离低频 GI）
//
// 通过 r.Pipeline.Mode CVar 一键切换 (0=Deferred, 1=HybridRT)
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

    // ── 访问器 ──
    void SetSwapChain(rhi::IRHISwapChain* sc) { m_SwapChain = sc; }
    GBufferRenderer*      GetGBuffer()       { return m_GBuffer.get(); }
    LightingPass*         GetLighting()      { return &m_Lighting; }
    PostProcessChain*     GetPostProcess()   { return &m_PostProcess; }
    RTPass*               GetRTPass()        { return m_RTPass.get(); }
    GI_DDGI*              GetDDGI()          { return &m_DDGI; }
    ClusteredShading&     GetClusteredShading() { return m_ClusteredShading; }
    GPUCulling&           GetGPUCulling()       { return m_GPUCulling; }
    ParticleRenderer&     GetParticleRenderer()  { return m_ParticleRenderer; }
    ProfilerManager&      GetProfiler()      { return m_Profiler; }
    ProfilerPanel&        GetProfilerPanel() { return m_ProfilerPanel; }
    BloomPass&            GetBloom()         { return m_PostProcess.GetBloom(); }
    DOFPass&              GetDOF()           { return m_PostProcess.GetDOF(); }
    MotionBlurPass&       GetMotionBlur()    { return m_PostProcess.GetMotionBlur(); }
    AutoExposurePass&     GetAutoExposure()  { return m_PostProcess.GetAutoExposure(); }
    ColorGradingPass&     GetColorGrading()  { return m_PostProcess.GetColorGrading(); }
    ToneMapPass*          GetToneMap()       { return m_PostProcess.GetToneMap(); }
    void EnableFXAA(bool e) { m_PostProcess.EnableFXAA(m_Device, m_Width, m_Height, e); }
    bool IsFXAAEnabled() const { return m_PostProcess.IsFXAAEnabled(); }

    // GBuffer 模式
    void SetGBufferMode(GBufferRenderer::Mode m);
    GBufferRenderer::Mode GetGBufferMode() const;

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
    std::unique_ptr<RTPass> m_RTPass;
    bool m_RTEnabled = false;

    // ── HybridRTPipeline 专有 GI ──
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
    std::unique_ptr<rhi::IRHIBuffer> m_LightBuffers[3];
    std::unique_ptr<rhi::IRHIBuffer> m_ObjectBuffers[3];
    std::unique_ptr<rhi::IRHIBuffer> m_ShadowBuffers[3];
    u32 m_CurrentFrameSlot = 0;
    std::vector<u32> m_GPUVisibleIndices;

    // ── Profiler ──
    ProfilerManager m_Profiler;
    ProfilerPanel   m_ProfilerPanel;

    float4x4 m_PrevViewProj = float4x4(1.0f);
    float4x4 m_CurrViewProj = float4x4(1.0f);

    u32 m_Width = 0, m_Height = 0;
    bool m_Ready = false;
};

} // namespace he::render
