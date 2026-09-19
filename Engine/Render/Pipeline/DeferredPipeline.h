#pragma once

#include "Pipeline/IRenderPipeline.h"
#include "Pipeline/Material.h"
#include "GI/GlobalIllumination.h"
#include "GI/GI_RSM.h"
#include "GI/RSMIndirect.h"   // RSM 间接光的半分辨率求值 pass（任务 16）
#include "GI/DDGITracePass.h" // DDGI 探针射线的光追 march（任务 17）
#include "RHI/RHI.h"
#include "RenderGraph.h"

namespace he::render { class GI_IBL; class GI_RSM; }
namespace he::render { class ToneMapPass; class SkyboxPass; class SceneRenderer; }

#include "Shadow/IShadowSystem.h"
#include "SceneRenderer.h"
#include "Pipeline/ClusteredShading.h"
#include "Pipeline/GPUCulling.h"
#include "Pipeline/GPUScene.h"
#include "Pipeline/MeshBatcher.h"

#include "Pipeline/GBufferRenderer.h"
#include "Pipeline/DecalPass.h"   // 任务 24：GBuffer 投影贴花
#include "Pipeline/InstanceCuller.h"   // 任务 25：逐实例 GPU 视锥剔除
#include "Pipeline/LightingPass.h"
#include "Pipeline/ParticleRenderer.h"
#include "GI/GI_SSGI.h"
#include "PostProcess/SSAO.h"
#include "GI/GI_SSR.h"
#include "GI/GI_DDGI.h"
#include "GI/GIRadianceHistory.h"   // 前帧 HDR 辐射度（GI 源共享）
#include "GI/GITiming.h"            // 各源 GPU 耗时读数（任务 29 / §9.2-Z）
#include "GI/GI_IBL.h"
#include "GI/GITypes.h"   // GIConfig（通道层栈 + 档位）
#include "GI/IGIProvider.h"   // GI 源统一抽象（P4）
#include "GI/AOProvider.h"   // 屏幕空间 AO Provider（Wave 2 试点）
#include "GI/IBLProvider.h"  // IBL 环境源 Provider
#include "GI/RSMProvider.h"  // RSM 间接光 Provider
#include "GI/SSGIProvider.h" // 屏幕空间 GI Provider（含降噪附属 pass）
#include "GI/SSRProvider.h"  // 屏幕空间反射 Provider
#include "GI/DDGIProvider.h" // 动态漫反射探针 Provider（compute、无纹理输出）
#include "GI/LumenProvider.h" // Lumen（虚拟化几何 GI）Provider
#include "Lumen/LumenScene.h" // Lumen 持久资源宿主（atlas / SDF clipmap / 探针）
#include "GI/RTProvider.h"   // 光追效果 Provider（四种效果共用实现）
#include "PostProcess/Denoiser.h"
// RT 效果（P3 统一后 Deferred 亦可按层栈启用光追源）
#include "RT/RTShadowPass.h"
#include "RT/RTAOPass.h"
#include "RT/RTReflectionPass.h"
#include "RT/RTGIPass.h"
#include "PostProcess/RTDenoiser.h"
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

    bool Initialize(rhi::IRHIDevice* device, u32 width = 0, u32 height = 0) override;
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
    /// 前帧 HDR 辐射度（供需要真实入射辐射度的 GI 源消费）
    GIRadianceHistory&   GetRadianceHistory()     { return m_RadianceHistory; }
    GI_SSGI*             GetSSGI()               { return &m_SSGI; }
    GI_SSR*              GetSSR()                { return &m_SSR; }
    // GI 配置（M2 数据驱动：档位/通道/强度单一数据源）
    GIConfig*            GetGIConfig() override { return &m_GIConfig; }
    u32                  GetGIPipelineCaps() const override { return PipelineCaps::Deferred; }
    ClusteredShading&    GetClusteredShading()   { return m_ClusteredShading; }
    GPUCulling&          GetGPUCulling()         { return m_GPUCulling; }
    SceneRenderer&        GetSceneRenderer()       { return *m_SceneRenderer; }
    ParticleRenderer&     GetParticleRenderer()   { return m_ParticleRenderer; }
    void AddParticleComponent(u32 id)             { m_ParticleComponentIDs.push_back(id); }
    void SetSwapChain(rhi::IRHISwapChain* sc) override  { m_SwapChain = sc; }
    BloomPass&      GetBloom()      { return m_PostProcess.GetBloom(); }
    DOFPass&        GetDOF()        { return m_PostProcess.GetDOF(); }
    MotionBlurPass& GetMotionBlur() { return m_PostProcess.GetMotionBlur(); }
    SSAO&           GetSSAO()       { return m_SSAO; }
    ProfilerManager&    GetProfiler()      { return m_Profiler; }
    /// Lighting 通道（暴露 HDR 目标等，供结果校验类功能读取，如白炉测试探针）
    LightingPass&       GetLighting()      { return m_Lighting; }
    /// GBuffer 通道（暴露 albedo/normal 等 MRT，供结果校验类功能读取，如离线频谱采样）
    GBufferRenderer*    GetGBuffer()       { return m_GBuffer.get(); }
    /// GBuffer 投影贴花 Pass（任务 24；暴露统计供调试/判据读取）
    DecalPass&          GetDecalPass()     { return m_DecalPass; }
    /// 已注册的 GI Provider（帧图按注册表遍历构建 pass，而非手写门控）
    std::vector<std::unique_ptr<IGIProvider>>& GetGIProviders() { return m_GIProviders; }
    /// RSM 子系统与它的半分辨率求值 pass（供离线采样设施逐级查看 RSM 链路：
    /// 位置 / 法线 / VPL 辐射度 / 间接光输出。任务 30 的缺陷正是"pass 在跑、输出恒空"，
    /// 没有这几级就只能猜 —— 见文档 §11.3 的采样目标清单）。
    GI_RSM*      GetRSM()         { return m_RSM.get(); }
    RSMIndirect& GetRSMIndirect() { return m_RSMIndirect; }

    // RT 基础设施访问（供 PathTracingPipeline 共享同一份 TLAS，避免重复内存）
    RTPass*             GetRTPass()        { return m_RTPass.get(); }
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

    // GBuffer 投影贴花（任务 24）：贴花体积盒 → 读 GBuffer 世界坐标裁剪 → 混合写回 albedo/法线
    DecalPass m_DecalPass;
    /// 贴花卡片是否从 GBuffer 绘制中排除（Deferred 恒为 true：由投影 Pass 接管）
    bool m_ExcludeDecalCards = false;

    // 逐实例 GPU 视锥剔除（任务 25）：实例化网格的可见列表 + 间接命令
    InstanceCuller m_InstanceCuller;

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
    /// Lumen 的持久资源宿主（Surface Cache atlas / Global SDF clipmap / 探针缓冲 / 屏幕输出）。
    /// 生命周期由 `LumenProvider` 转调（Provider 的 `OnResize/Shutdown` → 本对象），
    /// 即步骤 1 补上的 Provider 生命周期遍历那条路径。
    LumenScene m_LumenScene;
    /// 前帧 HDR 辐射度（GI 源共享；DDGI 探针与 SSGI 的入射辐射度都取自它）
    GIRadianceHistory m_RadianceHistory;
    GIConfig m_GIConfig;   // GI 配置（M2 档位/通道/强度 → P3 源层栈单一数据源）
    std::vector<std::unique_ptr<IGIProvider>> m_GIProviders;   // 已注册的 GI 源（P4）
    /// 各源 GPU 耗时读数（任务 29 / §9.2-Z）：帧图在 Provider 的主 pass 前后打时间戳，
    /// 在**该飞行帧槽位下一次被复用时**读回并写进源自己的 GIDebugData。
    GITimer m_GITimer;

    /// 诊断日志的统一节流计数（每帧 Render 自增一次）。
    /// **不要用 `m_FrameCounter` 代替**：它只在启用异步计算时才自增，普通路径恒为 0。
    u32 m_DiagFrameCounter = 0;

    // ── 场景包围盒（DDGI 网格自动拟合 + RSM 光源视锥，任务 14 / 任务 30）──
    /// 包围盒重算倒计时：遍历带变换的网格包围盒不是零成本，而场景几何很少变。
    /// **不能用 `m_FrameCounter` 代替**：它只在启用异步计算时才自增（普通路径恒为 0）。
    static constexpr u32 kSceneBoundsRefreshFrames = 30;
    u32 m_SceneBoundsCountdown = 0;
    /// 最近一次算出的场景包围盒（`IsValid()` 为假表示还没有几何 / 场景为空）。
    /// 两个消费者共用同一份：DDGI 探针网格拟合、RSM 光源正交视锥拟合（§9.2-AA ④
    /// 之前 RSM 用的是硬编码的 sceneCenter=(0,3,0) / sceneRadius=60）。
    he::AABB m_SceneBounds;

    // ── RT 基础设施（设备支持光追时创建；是否参与由层栈的 RT 源决定）──
    // P3：光追是「GI 源」而非「管线类型」，故 Deferred 亦可直接启用 RTGI/RT 反射/RTAO/RT 阴影
    std::unique_ptr<RTPass>           m_RTPass;             // AS 构建 + TLAS + 场景资源
    std::unique_ptr<RTShadowPass>     m_RTShadow;
    std::unique_ptr<RTAOPass>         m_RTAO;
    std::unique_ptr<RTReflectionPass> m_RTReflection;
    std::unique_ptr<RTGIPass>         m_RTGI;
    // RT 降噪（时域累积 + 反射/GI 的空间滤波）
    std::unique_ptr<RTDenoiser> m_ShadowDenoiser;
    std::unique_ptr<RTDenoiser> m_AODenoiser;
    std::unique_ptr<RTDenoiser> m_ReflectionDenoiser;
    std::unique_ptr<RTDenoiser> m_GIDenoiser;
    // ── 统一降噪框架（§10 的 11.3，步骤 34）──
    DenoiseHistoryPool     m_DenoiseHistoryPool;   // 统一的历史纹理分配
    DenoiseSignalRegistry  m_DenoiseSignals;       // 当帧信号登记处（多信号共存的唯一视角）
    Denoiser m_ReflectionSpatial;
    Denoiser m_GISpatial;
    bool m_RTEnabled = false;   // 设备支持光追且 RTPass 初始化成功
    bool m_SceneMaterialBuilt = false;   // 场景材质纹理是否已构建（延迟到首帧）
    Denoiser m_DenoiseSSGI;
    Denoiser m_DenoiseSSR;
    SSAO    m_SSAO;
    /// RSM 间接光（16 点 Poisson VPL 求和）的半分辨率求值 pass（任务 16 / B3）。
    /// 它不是 GI 源（没有通道输出、不参与归一化），只是 Lighting 采样 GISOURCE_RSM 的
    /// **求值前置**，故与 SSAO 同类由管线持有，不注册进 IGIProvider 表。
    RSMIndirect m_RSMIndirect;
    /// DDGI 探针射线的硬件光追 march（任务 17 / B4）：设备支持光追时创建，否则为 nullptr
    /// ⇒ DDGI 的探针更新回退到 RSM/IBL 路径（`supportsRayTracing` 自动选择）。
    std::unique_ptr<DDGITracePass> m_DDGI_Trace;
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
