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

// DGC 前向声明（仅在 Vulkan 后端下实际使用）
namespace he::rhi { class VulkanDGC; }
#include "Pipeline/GBufferRenderer.h"
#include "Pipeline/ParticleRenderer.h"
#include "GI/GI_SSGI.h"
#include "PostProcess/SSAO.h"
#include "GI/GI_SSR.h"
#include "GI/GI_DDGI.h"
#include "PostProcess/Denoiser.h"
#include "PostProcess/BloomPass.h"
#include "PostProcess/DOFPass.h"
#include "PostProcess/MotionBlurPass.h"
#include "PostProcess/AutoExposurePass.h"
#include "PostProcess/ColorGradingPass.h"
#include "PostProcess/ToneMapPass.h"
#include "Profiler/ProfilerManager.h"
#include "PostProcess/SkyboxPass.h"
#include "Scene/World.h"
#include "Scene/SceneGraph.h"
#include "AntiAliasing/AntiAliasing.h"
#include "AntiAliasing/AA_FXAA.h"
#include "AntiAliasing/AA_SMAA.h"
#include "AntiAliasing/AA_MSAA.h"

#include <memory>
#include <vector>

namespace he::render {

// ============================================================================
// DeferredPipeline — 延迟渲染管线（GBuffer + Lighting Pass）
//
// 复用 ShadowSystem / GI_IBL / GI_RSM / ToneMapPass / SkyboxPass
// GBuffer 3×MRT (albedo+metallic / normal+roughness / emissive+ao) + D32
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
                he::SceneGraph& sg, const CameraData& camera) override;

    // AsyncCompute: 在 Graphics Submit 之后调用，提交 Compute 工作
    // 内部使用 Timeline Semaphore 确保跨队列同步顺序
    void FlushComputeWork();

    IShadowSystem*       GetShadowSystem() override { return m_ShadowSystem.get(); }
    IGlobalIllumination* GetGI()           override { return m_GI.get(); }
    ToneMapPass*         GetToneMap()            { return m_ToneMap.get(); }
    GI_DDGI*             GetDDGI()               { return &m_DDGI; }
    GI_SSGI*             GetSSGI()               { return &m_SSGI; }
    GI_SSR*              GetSSR()                { return &m_SSR; }
    ClusteredShading&    GetClusteredShading()   { return m_ClusteredShading; }
    GPUCulling&          GetGPUCulling()         { return m_GPUCulling; }
    void SetSwapChain(rhi::IRHISwapChain* sc)  { m_SwapChain = sc; }
    BloomPass&      GetBloom()      { return m_Bloom; }
    DOFPass&        GetDOF()        { return m_DOF; }
    MotionBlurPass& GetMotionBlur() { return m_MotionBlur; }
    SSAO&           GetSSAO()       { return m_SSAO; }
    ProfilerManager&    GetProfiler()      { return m_Profiler; }
    AutoExposurePass&   GetAutoExposure()  { return m_AutoExposure; }
    ColorGradingPass&   GetColorGrading()  { return m_ColorGrading; }
    // GBuffer 渲染模式
    enum class GBufferMode : u8 { CPU, GPU };
    void         SetGBufferMode(GBufferMode m);
    GBufferMode  GetGBufferMode() const         { return m_GBufferMode; }

    void EnableFXAA(bool enable);
    bool IsFXAAEnabled() const                 { return m_FXAAEnabled && m_FXAA != nullptr; }

    void EnableSMAA(bool enable);                                                   // SMAA 懒初始化
    bool IsSMAAEnabled() const                 { return m_SMAAEnabled && m_SMAA != nullptr && m_SMAA->IsReady(); }
    AA_SMAA* GetSMAA()                         { return m_SMAA.get(); }

    void EnableMSAA(bool enable);                                                   // MSAA 懒初始化
    bool IsMSAAEnabled() const                 { return m_MSAAEnabled && m_MSAA != nullptr && m_MSAA->IsReady(); }
    AA_MSAA* GetMSAA()                         { return m_MSAA.get(); }

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
    std::unique_ptr<rhi::IRHITexture> m_GBufferA, m_GBufferB, m_GBufferC;
    // GBuffer 第 4 张 MRT：velocity（RG16_FLOAT，UV 空间运动矢量）
    std::unique_ptr<rhi::IRHITexture> m_GBufferD;
    // GBuffer 第 5 张 MRT：worldPos.xyz（RGBA16_FLOAT，Lighting pass 直接读取，无需 invViewProj）
    std::unique_ptr<rhi::IRHITexture> m_GBufferE;
    std::unique_ptr<rhi::IRHITexture> m_GBufferDepth;
    std::unique_ptr<rhi::IRHIPipelineState> m_GBufferPSO;
    rhi::DescriptorSetLayoutHandle m_GBufferLayout = rhi::kInvalidLayout;   // set=0: per-frame + bindless
    rhi::DescriptorSetHandle       m_GBufferSet    = rhi::kInvalidSet;      // set=0 共享（含 bindless）

    // Lighting PSO + 描述符
    std::unique_ptr<rhi::IRHIPipelineState> m_LightingPSO;
    rhi::DescriptorSetLayoutHandle m_LightingLayout = rhi::kInvalidLayout;
    rhi::DescriptorSetHandle       m_LightingSet    = rhi::kInvalidSet;
    std::unique_ptr<rhi::IRHITexture> m_BindlessPlaceholder;
    std::unique_ptr<rhi::IRHISampler> m_BindlessSampler;

    // HDR (Lighting 输出 + ToneMap 输入)
    std::unique_ptr<rhi::IRHITexture> m_HDRTarget, m_HDRDepth;
    std::unique_ptr<rhi::IRHISampler> m_HDRSampler;
    std::unique_ptr<rhi::IRHISampler> m_PointSampler;  // 点采样器（深度纹理 Nearest，避免 Linear 插值导致 worldPos 错误）

    // LDR 中间纹理（ToneMap 输出 → FXAA 输入，BGRA8_UNORM）
    // FXAA 禁用时 ToneMap 直接写 BackBuffer，此纹理闲置
    std::unique_ptr<rhi::IRHITexture> m_LDRTarget;
    std::unique_ptr<rhi::IRHISampler> m_LDRSampler;
    std::unique_ptr<rhi::IRHITexture> m_LDRDummyDepth;  // ToneMap PSO 带 depth，Offscreen 需 2 附件

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
    std::unique_ptr<ToneMapPass>         m_ToneMap;
    std::unique_ptr<SkyboxPass>          m_Skybox;
    std::unique_ptr<SceneRenderer>       m_SceneRenderer;

    // 时域抗锯齿
    std::unique_ptr<IAntiAliasing> m_AntiAliasing;

    // FXAA（LDR 空间后处理抗锯齿，可单独使用或与 TAA 叠加）
    std::unique_ptr<AA_FXAA> m_FXAA;
    bool m_FXAAEnabled = false;

    // SMAA（LDR 空间形态学抗锯齿，与 FXAA 互斥二选一）
    std::unique_ptr<AA_SMAA> m_SMAA;
    bool m_SMAAEnabled = false;

    // MSAA（硬件多重采样，覆盖 RT/PSO 的 sampleCount，无需独立 Pass）
    std::unique_ptr<AA_MSAA> m_MSAA;
    bool m_MSAAEnabled = false;

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
    rhi::VulkanDGC* m_VulkanDGC  = nullptr;   // DGC 封装（仅在 Vulkan + DGC 支持时创建，手动管理生命周期）
    bool            m_DGCEnabled = false;     // 运行时 CVar 控制

    // GPU 粒子系统
    ParticleRenderer m_ParticleRenderer;
    std::vector<u32> m_ParticleComponentIDs;   // 注册的粒子组件 ID 列表

    // GBuffer 渲染（CPU/GPU 双模式）
    GBufferMode m_GBufferMode = GBufferMode::CPU;
    GBufferContext m_GBufferCtx;
    std::unique_ptr<IGBufferRenderer> m_GBufferRenderer;

    // SSGI + SSR + DDGI + SSAO
    GI_SSGI m_SSGI;
    GI_SSR  m_SSR;
    GI_DDGI m_DDGI;
    Denoiser m_DenoiseSSGI;
    Denoiser m_DenoiseSSR;
    SSAO    m_SSAO;
    BloomPass m_Bloom;          // Bloom（懒初始化）
    DOFPass  m_DOF;            // 景深（懒初始化）
    MotionBlurPass m_MotionBlur; // 运动模糊（懒初始化）
    ProfilerManager m_Profiler;  // GPU 时间戳 Profiler
    AutoExposurePass m_AutoExposure; // 自动曝光
    ColorGradingPass m_ColorGrading; // LDR 色彩分级
    std::vector<u32> m_GPUVisibleIndices;


    // 相机矩阵缓存（当前帧 + 上一帧，用于 velocity 计算和 TAA）
    float4x4 m_PrevViewProj = float4x4(1.0f);
    float4x4 m_CurrViewProj = float4x4(1.0f);

    u32 m_Width = 1920, m_Height = 1080;
    bool m_Ready = false;
};

} // namespace he::render
