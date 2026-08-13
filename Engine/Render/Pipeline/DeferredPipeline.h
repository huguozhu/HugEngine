#pragma once

#include "Pipeline/IRenderPipeline.h"
#include "Pipeline/Material.h"
#include "GI/GlobalIllumination.h"
#include "GI/GI_RSM.h"
#include "RHI/RHI.h"
#include "RenderGraph.h"
#include "Asset/BindlessTextureManager.h"

namespace he::render { class GI_IBL; class GI_RSM; }
namespace he::render { class ToneMapPass; class SkyboxPass; class SceneRenderer; }

#include "Shadow/IShadowSystem.h"
#include "SceneRenderer.h"
#include "Pipeline/ClusteredShading.h"
#include "Pipeline/GPUCulling.h"
#include "Pipeline/GPUScene.h"
#include "Pipeline/MeshBatcher.h"

#include "Pipeline/GBufferRenderer.h"
#include "Pipeline/LightingPass.h"
#include "Pipeline/ParticleRenderer.h"
#include "GI/GI_SSGI.h"
#include "PostProcess/SSAO.h"
#include "GI/GI_SSR.h"
#include "GI/GI_DDGI.h"
#include "PostProcess/Denoiser.h"
#include "Profiler/ProfilerManager.h"
#include "Profiler/ProfilerPanel.h"
#include "Scene/World.h"
#include "Scene/SceneGraph.h"
#include "PostProcess/PostProcessChain.h"

#include <memory>
#include <vector>

namespace he::render {

// ============================================================================
// DeferredPipeline — 延迟渲染管线（GBuffer + Lighting Pass）
//
// 复用 ShadowSystem / GI_IBL / GI_RSM / ToneMapPass / SkyboxPass
// GBuffer 5×MRT（albedo+metallic / normal+roughness / emissive+AO / velocity / worldPos）
// Lighting Pass 全屏三角形 PBR + IBL + RSM + Shadow
// ============================================================================
class DeferredPipeline : public IRenderPipeline {
    HE_DECLARE_NON_COPYABLE(DeferredPipeline);

public:
    DeferredPipeline()  = default;
    ~DeferredPipeline() override = default;

    bool Initialize(rhi::IRHIDevice* device) override;
    void Shutdown() override;
    void NextFrame() override;
    void OnResize(u32 width, u32 height) override;
    const char* GetName() const override { return "DeferredPipeline"; }

    void Render(rhi::IRHICommandList* cmd, he::World& world,
                he::SceneGraph& sg, const CameraData& camera,
                float deltaTime = 0.016f) override;

    // AsyncCompute: 在 Graphics Submit 之后调用，提交 Compute 工作
    // 内部使用 Timeline Semaphore 确保跨队列同步顺序
    void FlushComputeWork();

    IShadowSystem*       GetShadowSystem() override { return m_ShadowSystem.get(); }
    IGlobalIllumination* GetGI()           override { return m_GI.get(); }
    ToneMapPass*         GetToneMap()            { return m_PostProcess.GetToneMap(); }
    GI_DDGI*             GetDDGI()               { return &m_DDGI; }
    GI_SSGI*             GetSSGI()               { return &m_SSGI; }
    GI_SSR*              GetSSR()                { return &m_SSR; }
    ClusteredShading&    GetClusteredShading()   { return m_ClusteredShading; }
    GPUCulling&          GetGPUCulling()         { return m_GPUCulling; }
    SceneRenderer&        GetSceneRenderer()       { return *m_SceneRenderer; }
    ParticleRenderer&     GetParticleRenderer()   { return m_ParticleRenderer; }
    void AddParticleComponent(u32 id)             { m_ParticleComponentIDs.push_back(id); }
    void SetSwapChain(rhi::IRHISwapChain* sc)  { m_SwapChain = sc; }
    BloomPass&      GetBloom()      { return m_PostProcess.GetBloom(); }
    DOFPass&        GetDOF()        { return m_PostProcess.GetDOF(); }
    MotionBlurPass& GetMotionBlur() { return m_PostProcess.GetMotionBlur(); }
    SSAO&           GetSSAO()       { return m_SSAO; }
    ProfilerManager&    GetProfiler()      { return m_Profiler; }
    ProfilerPanel&      GetProfilerPanel() { return m_ProfilerPanel; }
    AutoExposurePass&   GetAutoExposure()  { return m_PostProcess.GetAutoExposure(); }
    ColorGradingPass&   GetColorGrading()  { return m_PostProcess.GetColorGrading(); }
    SkyboxPass*         GetSkybox()        { return m_PostProcess.GetSkybox(); }
    // GBuffer 渲染模式（委托给 GBufferRenderer）
    void         SetGBufferMode(GBufferRenderer::Mode m);
    GBufferRenderer::Mode GetGBufferMode() const;

    void EnableFXAA(bool enable);
    bool IsFXAAEnabled() const                 { return m_PostProcess.IsFXAAEnabled(); }

    void EnableSMAA(bool enable);
    bool IsSMAAEnabled() const                 { return m_PostProcess.IsSMAAEnabled(); }
    AA_SMAA* GetSMAA()                         { return m_PostProcess.GetSMAA(); }

    void EnableMSAA(bool enable);
    bool IsMSAAEnabled() const                 { return m_PostProcess.IsMSAAEnabled(); }
    AA_MSAA* GetMSAA()                         { return m_PostProcess.GetMSAA(); }

    rhi::IRHIBuffer* GetCurrentObjectBuffer()  { return m_ObjectBuffers[m_CurrentFrameSlot].get(); }
    rhi::IRHIBuffer* GetCurrentShadowBuffer()  { return m_ShadowBuffers[m_CurrentFrameSlot].get(); }
    rhi::IRHIBuffer* GetCurrentShadowObjBuffer(){ return m_ShadowObjBuffers[m_CurrentFrameSlot].get(); }

private:
    void BuildFrameGraph(RenderGraph& rg, he::World& world,
                         he::SceneGraph& sg, const CameraData& camera);
    void CollectLights(PushConstantData& pc, he::World& world,
                       he::SceneGraph& sg, const CameraData& camera);
    void UpdateIBLBindings(GI_IBL* gi);
    void UpdateRSMBindings();

    rhi::IRHIDevice* m_Device = nullptr;
    rhi::IRHISwapChain* m_SwapChain = nullptr;

    // AsyncCompute: 专用 Compute 队列命令列表（延迟创建）
    std::unique_ptr<rhi::IRHICommandList> m_ComputeCmdList;
    rhi::RHIFenceHandle m_CrossQueueFence = rhi::kInvalidFence;  // 跨队列同步信号量
    u64  m_FrameCounter = 0;              // 帧计数器（Fence 信号值）
    bool m_ComputePendingSubmit = false;  // 是否有待提交的 Compute 工作
    // GBuffer 渲染（纹理所有权 + PSO + 描述符集，共享组件）
    std::unique_ptr<GBufferRenderer> m_GBuffer;

    // 光照 Pass（HDR 目标 + PSO + 描述符集，共享组件）
    LightingPass m_Lighting;

    // 后处理链（Bloom/DOF/MotionBlur/TAA/ToneMap/ColorGrading/AA + LDR 纹理，共享组件）
    PostProcessChain m_PostProcess;

    // 三缓冲
    std::unique_ptr<rhi::IRHIBuffer> m_LightBuffers[MAX_FRAMES_IN_FLIGHT];
    std::unique_ptr<rhi::IRHIBuffer> m_ObjectBuffers[MAX_FRAMES_IN_FLIGHT];
    std::unique_ptr<rhi::IRHIBuffer> m_ShadowBuffers[MAX_FRAMES_IN_FLIGHT];
    std::unique_ptr<rhi::IRHIBuffer> m_ShadowObjBuffers[MAX_FRAMES_IN_FLIGHT];
    u32 m_CurrentFrameSlot = 0;

    // 子系统
    std::unique_ptr<IShadowSystem>       m_ShadowSystem;
    std::unique_ptr<IGlobalIllumination> m_GI;
    std::unique_ptr<GI_RSM>              m_RSM;
    std::unique_ptr<SceneRenderer>       m_SceneRenderer;

    // Clustered Shading
    ClusteredShading m_ClusteredShading;
    std::unique_ptr<rhi::IRHIBuffer> m_LightGridBuffer;       // binding 7
    std::unique_ptr<rhi::IRHIBuffer> m_LightIndexListBuffer;  // binding 8
    std::vector<GPULight> m_CachedLights;  // CPU 端缓存，避免 culling 时重复 Map

    // GPU Culling
    GPUCulling m_GPUCulling;
    GPUScene   m_GPUScene;
    MeshBatcher m_MeshBatcher;
    bool       m_BatchBuilt = false;

    // Device Generated Commands（DGC）状态
    bool            m_DGCEnabled = false;     // 运行时 CVar 控制（DGC 由 m_Device 内部管理）

    // GPU 粒子系统
    ParticleRenderer m_ParticleRenderer;
    std::vector<u32> m_ParticleComponentIDs;   // 注册的粒子组件 ID 列表

    // SSGI + SSR + DDGI + SSAO（DeferredPipeline 专有效果）
    GI_SSGI m_SSGI;
    GI_SSR  m_SSR;
    GI_DDGI m_DDGI;
    Denoiser m_DenoiseSSGI;
    Denoiser m_DenoiseSSR;
    SSAO    m_SSAO;
    ProfilerManager m_Profiler;  // GPU 时间戳 Profiler
    ProfilerPanel   m_ProfilerPanel; // ImGui 可视化面板
    std::unique_ptr<rhi::IRHIPipelineState> m_TransientTestPSO;  // 瞬态资源路径验证 PSO
    // GPL 变体演示：持有限流器创建的变体 PSO，避免中途销毁
    std::vector<std::unique_ptr<rhi::IRHIPipelineState>> m_GPLVariantPSOs;
    std::vector<u32> m_GPUVisibleIndices;


    // 相机矩阵缓存（当前帧 + 上一帧，用于 velocity 计算和 TAA）
    float4x4 m_PrevViewProj = float4x4(1.0f);
    float4x4 m_CurrViewProj = float4x4(1.0f);

    u32 m_Width = rhi::kDefaultBackBufferWidth, m_Height = rhi::kDefaultBackBufferHeight;
    bool m_Ready = false;
};

} // namespace he::render
