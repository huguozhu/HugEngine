#pragma once

#include "Pipeline/IRenderPipeline.h"
#include "Pipeline/Material.h"
#include "Pipeline/ClusteredShading.h"
#include "GI/GlobalIllumination.h"
#include "GI/GITypes.h"   // GIConfig（通道层栈 + 档位）
#include "RHI/RHI.h"
#include "RenderGraph.h"
#include "Pipeline/GPUCulling.h"
#include "Pipeline/GPUScene.h"
#include "Pipeline/MeshBatcher.h"
#include "Pipeline/InstanceCuller.h"   // 任务 25：逐实例 GPU 视锥剔除
#include "AntiAliasing/AntiAliasing.h"
#include "Profiler/ProfilerManager.h"

namespace he::render { class GI_IBL; }
namespace he::render { class GI_RSM; }
namespace he::render { class ToneMapPass; }
namespace he::render { class SkyboxPass; }
namespace he::render { class SceneRenderer; }

#include "Shadow/IShadowSystem.h"

#include "Scene/World.h"
#include "Scene/SceneGraph.h"
#include "Scene/MeshComponent.h"
#include "Scene/LightComponent.h"
#include "Scene/Transform.h"
#include "Core/Types.h"
#include "Math/Geometry.h"   // he::AABB（RSM 固定光锥的场景包围盒，任务 34）

#include <memory>
#include <vector>

namespace he::render {

class ForwardPipeline : public IRenderPipeline {
    HE_DECLARE_NON_COPYABLE(ForwardPipeline);

public:
    ForwardPipeline();
    ~ForwardPipeline() override;

    // IRenderPipeline
    bool Initialize(rhi::IRHIDevice* device, u32 width = 0, u32 height = 0) override;
    void Shutdown() override;
    void NextFrame() override;
    void OnResize(u32 width, u32 height) override;
    const char* GetName() const override { return "ForwardPipeline"; }
    void Render(rhi::IRHICommandList* cmd, he::World& world,
                he::SceneGraph& sg, const CameraData& camera,
                float deltaTime = 0.016f) override;

    // 子系统访问
    IShadowSystem*       GetShadowSystem() override { return m_ShadowSystem.get(); }
    IGlobalIllumination* GetGI()           override { return m_GI.get(); }
    // 该管线的 GI 通道配置（Forward：Raster 阴影 + SSAO + SSR + IBL/RSM）
    GIConfig*            GetGIConfig() override { return &m_GIConfig; }
    u32                  GetGIPipelineCaps() const override { return PipelineCaps::Forward; }
    int ReloadShader(StringView shaderName, const std::vector<u32>& newSpirv) override;
    ToneMapPass*         GetToneMap()            { return m_ToneMap.get(); }
    SkyboxPass*          GetSkybox()             { return m_Skybox.get(); }

    // ForwardPipeline 特有方法（命令式，保留兼容）
    void BeginFrame(rhi::IRHICommandList* cmd, u32 width, u32 height);
    void RenderScene(rhi::IRHICommandList* cmd, he::World& world,
                     he::SceneGraph& sg, const CameraData& camera);
    /// GPU 视锥剔除：收集场景对象 → 上传 GPUScene SSBO → 读回上帧可见性 → Dispatch Compute。
    /// **必须在任何 render pass 之外调用**：vkCmdDispatch 不允许出现在 render pass 内部
    /// （VUID-vkCmdDispatch-None-10672），且它会采样 HDR 深度 —— 那正是本帧 Scene pass 的
    /// 深度附件，在 pass 内采样构成非法反馈。RG 路径由独立的 "GPU_Cull" compute pass 调用，
    /// 非 RG 路径在 BeginHDRPass 之前调用。
    void RunGPUCulling(rhi::IRHICommandList* cmd, he::World& world,
                       he::SceneGraph& sg, const CameraData& camera);
    void EndFrame(rhi::IRHICommandList* cmd);

    // RenderGraph 模式（声明式 Pass 编排，自动 Barrier）
    void BuildFrameGraph(RenderGraph& rg, he::World& world, he::SceneGraph& sg,
                         const CameraData& camera);
    bool UseRenderGraph() const { return m_UseRenderGraph; }
    void SetUseRenderGraph(bool use) { m_UseRenderGraph = use; }
    void SetSwapChain(rhi::IRHISwapChain* sc) override { m_SwapChain = sc; }
    rhi::IRHIPipelineState* GetPipelineState() const { return m_PBR_PSO.get(); }

    void SetMultiThreadedRecording(bool e) { m_MultiThreadRecord = e; }
    bool IsMultiThreadedRecording() const { return m_MultiThreadRecord; }
    void SetUseExecuteIndirect(bool e) { m_UseExecuteIndirect = e; }
    bool GetUseExecuteIndirect() const { return m_UseExecuteIndirect; }

    // HDR 离屏渲染
    void BeginHDRPass(rhi::IRHICommandList* cmd, u32 w, u32 h);
    void EndHDRPass(rhi::IRHICommandList* cmd);
    void ResizeHDRTarget(u32 w, u32 h);

    void SetGI(std::unique_ptr<IGlobalIllumination> gi) { m_GI = std::move(gi); }
    IAntiAliasing* GetAntiAliasing() { return m_AntiAliasing.get(); }
    void SetAntiAliasing(std::unique_ptr<IAntiAliasing> aa) { m_AntiAliasing = std::move(aa); }
    void PrepareGI(rhi::IRHICommandList* cmd, he::World& world, he::SceneGraph& sg);
    GI_RSM* GetRSM() { return m_RSM.get(); }

    // 后处理（委托给子系统）
    void RenderToneMapPass(rhi::IRHICommandList* cmd);
    void RenderSkybox(rhi::IRHICommandList* cmd, he::World& world, const CameraData& camera);

    rhi::IRHIBuffer*         GetCurrentObjectBuffer() { return m_ObjectBuffers[m_CurrentFrameSlot].get(); }
    rhi::IRHIBuffer*         GetCurrentShadowBuffer() { return m_ShadowBuffers[m_CurrentFrameSlot].get(); }
    rhi::IRHIBuffer*         GetCurrentLightBuffer()  { return m_LightBuffers[m_CurrentFrameSlot].get(); }
    rhi::DescriptorSetHandle GetCurrentDescSet()      { return m_DescSets[m_CurrentFrameSlot]; }
    // 阴影专用 Object Buffer（独立于场景 Object Buffer，避免 CPU 录制时覆盖）
    rhi::IRHIBuffer*         GetCurrentShadowObjectBuffer() { return m_ShadowObjBuffers[m_CurrentFrameSlot].get(); }

    // HDR 纹理访问（供 ToneMapPass 使用）
    rhi::IRHITexture* GetHDRTarget()  const { return m_HDRTarget.get(); }
    rhi::IRHISampler* GetHDRSampler() const { return m_HDRSampler.get(); }
    GPUCulling& GetGPUCulling() { return m_GPUCulling; }
    SceneRenderer& GetSceneRenderer() { return *m_SceneRenderer; }
    ProfilerManager& GetProfiler() { return m_Profiler; }
    u32 GetLastDrawCount() const { return m_LastDrawCount; }
    u32 GetLastTriCount()  const { return m_LastTriCount; }

    // ── 任务 25：逐实例剔除统计（面板/判据用）──
    /// 逐实例剔除的实例化网格数 / 剔除后可见实例数 / 剔除前实例总数
    u32 GetCulledInstanceMeshCount() const { return m_LastCulledInstanceMeshes; }
    u32 GetVisibleInstanceCount()    const { return m_LastVisibleInstances; }
    u32 GetTotalInstanceCount()      const { return m_LastTotalInstances; }
    /// 逐实例剔除器（暴露给示例做开关与统计）
    InstanceCuller& GetInstanceCuller() { return m_InstanceCuller; }

private:
    void CollectLights(PushConstantData& pc, he::World& world, he::SceneGraph& sg, const CameraData& camera);
    void UploadMaterialBindless(he::World& world);  // 去重收集场景材质 → 写入 bindless 材质 SSBO 并注册（须在 heap->Flush() 前调用）
    void DrawMesh(rhi::IRHICommandList* cmd, he::MeshComponent* mesh,
                  const float4x4& worldMatrix, const float4x4& viewProjMatrix,
                  const PBRMaterial& material, const CameraData& camera,
                  const PushConstantData& lighting);
    void UploadLightBuffer();
    void UpdateIBLBindings(GI_IBL* gi);
    void UpdateRSMBindings();
    /// 把 `m_GIConfig` 的三个通道层栈打包进 GI 合成参数 UBO（任务 26）。
    /// 与 Deferred 走同一套语义：着色器按源数组 + 权重归一化，Forward 的 IBL/RSM 不再是
    /// 管线级开关。每帧在 Render 开头填一次（RG 路径与非 RG 路径都要用）。
    void FillGIBlendUBO();
    /// 刷新 RSM 的**固定**光源视锥（任务 34 / §9.2-AD）：按场景包围盒拟合、每 30 帧重算包围盒。
    /// 【为什么必须有】Forward 此前用 CSM 级联 0 的 VP 渲染并查找 RSM：那个 VP 拟合**相机视锥**
    /// （视角一变 RSM 内容就变，世界空间源的前提被破坏），而且由 Shadow pass 在执行时才写入
    /// `m_LightVPs`，帧图里 Shadow 与 RSM_Generate 没有依赖边 ⇒ 顺序不受保证。现在两份消费者
    /// （RSM pass 与 PBR 的内联查找）读**同一份**这个视锥 —— 写入 UV 与查找 UV 同源。
    /// 每帧在 Render 开头（填 UBO 之前）调用一次，结果同时喂 frame graph 与 UBO。
    void RefreshRSMFrustum(he::World& world, const CameraData& camera);
    rhi::IRHIDevice* m_Device = nullptr;
    std::unique_ptr<rhi::IRHIPipelineState> m_PBR_PSO;
    // 蒙皮网格 PSO（C1b）：扩展顶点布局（location 3/4 = JOINTS/WEIGHTS），同着色器
    std::unique_ptr<rhi::IRHIPipelineState> m_PBR_Skinned_PSO;

    rhi::DescriptorSetLayoutHandle m_PerFrameLayout = rhi::kInvalidLayout;  // set=0: per-frame + bindless
    rhi::DescriptorSetHandle       m_DescSets[MAX_FRAMES_IN_FLIGHT] = {};   // set=0 三缓冲
    std::unique_ptr<rhi::IRHIBuffer> m_LightBuffers[MAX_FRAMES_IN_FLIGHT];
    /// GI 分层合成参数 UBO（每飞行帧一份，与 Deferred 的 LightingPass 同结构同语义）
    std::unique_ptr<rhi::IRHIBuffer> m_GIBuffers[MAX_FRAMES_IN_FLIGHT];
    std::unique_ptr<rhi::IRHIBuffer> m_ObjectBuffers[MAX_FRAMES_IN_FLIGHT];
    std::unique_ptr<rhi::IRHIBuffer> m_ShadowBuffers[MAX_FRAMES_IN_FLIGHT];
    std::unique_ptr<rhi::IRHIBuffer> m_ShadowObjBuffers[MAX_FRAMES_IN_FLIGHT];  // 阴影专用 Object Buffer
    u32 m_CurrentFrameSlot = 0;

    // HDR 离屏渲染
    std::unique_ptr<rhi::IRHITexture> m_HDRTarget, m_HDRDepth;
    std::unique_ptr<rhi::IRHISampler> m_HDRSampler;
    u32 m_HDRWidth = rhi::kDefaultBackBufferWidth, m_HDRHeight = rhi::kDefaultBackBufferHeight;

    // Bindless 占位纹理/采样器
    std::unique_ptr<rhi::IRHITexture> m_BindlessPlaceholder;
    std::unique_ptr<rhi::IRHISampler> m_BindlessSampler;

    // Bindless 材质 SSBO（per-material 材质数据，去重后写入单个 buffer，binding 30 = u_Materials[]）
    std::unique_ptr<rhi::IRHIBuffer> m_MaterialBuffer;  // 材质数据 SSBO（元素 = GPUMaterialData，按 materialID>>2 索引）
    u32 m_MaterialCount = 0;                            // 已注册材质数（buffer 内元素数）
    bool m_UseBindlessMaterial = false;  // 默认走内联路径（GPUObjectData 字段读材质参数）；验证 bindless 后切换

    // 多线程录制
    bool m_MultiThreadRecord = true;
    bool m_UseExecuteIndirect = true;
    static constexpr u32 kMaxSecRecordLists = 8;
    // RenderGraph 模式（默认关闭，渐进迁移到声明式编排）
    bool m_UseRenderGraph = true;   // 启用 RenderGraph（含 GPU Profiler）
    std::vector<std::unique_ptr<rhi::IRHICommandList>> m_SecRecordLists;

    // 着色器
    rhi::ShaderBytecode m_VS, m_FS;

    // 子系统
    std::unique_ptr<IGlobalIllumination> m_GI;
    std::unique_ptr<GI_RSM>              m_RSM;
    GIConfig                             m_GIConfig;   // 该管线的 GI 通道配置（可用子集见 PipelineCaps::Forward）
    std::unique_ptr<IShadowSystem>       m_ShadowSystem;
    // ── RSM 固定光源视锥（任务 34 / §9.2-AD）──
    // 与 Deferred 侧同一套做法：包围盒每 30 帧重算（遍历带变换的网格包围盒不是零成本），
    // 视锥由纯几何函数 `FitRSMFrustumToBounds` 拟合，结果缓存给 frame graph 与 UBO 两处消费者。
    static constexpr u32 kSceneBoundsRefreshFrames = 30;
    he::AABB  m_SceneBounds;
    u32       m_SceneBoundsCountdown = 0;
    float4x4  m_RSMLightViewProj = float4x4(1.0f);  // 本帧 RSM pass 与 PBR 内联查找共用的 VP
    float     m_RSMVplScale      = 0.0f;            // 由该视锥的正交半宽推出的采样面积缩放
    bool      m_RSMFrustumValid  = false;           // 本帧 RSM pass 是否注册（写入与查找的共同前提）
    bool      m_RSMDirLightValid = false;           // 存在启用且投影的方向光（无则 RSM 无意义）
    std::unique_ptr<IAntiAliasing>       m_AntiAliasing;
    rhi::IRHISwapChain* m_SwapChain = nullptr;
    std::unique_ptr<ToneMapPass>         m_ToneMap;
    std::unique_ptr<SkyboxPass>          m_Skybox;
    std::unique_ptr<SceneRenderer>       m_SceneRenderer;

    // GPU Culling
    GPUCulling m_GPUCulling;
    GPUScene   m_GPUScene;
    MeshBatcher m_MeshBatcher;
    bool       m_BatchBuilt = false;
    std::vector<u32> m_GPUVisibleIndices;
    ProfilerManager m_Profiler;  // GPU 时间戳 Profiler
    u32 m_LastDrawCount = 0;
    u32 m_LastTriCount  = 0;

    // 任务 25：逐实例 GPU 视锥剔除（实例化网格的可见列表 + 间接命令）
    InstanceCuller m_InstanceCuller;
    u32 m_LastCulledInstanceMeshes = 0;   // 走逐实例剔除的实例化网格数
    u32 m_LastVisibleInstances     = 0;   // 剔除后可见实例数（读回，滞后一帧）
    u32 m_LastTotalInstances       = 0;   // 剔除前实例总数

    // Forward+（Cluster 光源剔除）
    bool                           m_UseForwardPlus = true;  // 默认开启 Forward+
    ClusteredShading               m_ClusteredShading;
    std::unique_ptr<rhi::IRHIBuffer> m_LightGridBuffer;       // binding 7: LightGrid
    std::unique_ptr<rhi::IRHIBuffer> m_LightIndexListBuffer;  // binding 8: LightIndexList
    std::vector<GPULight>          m_CachedLights;            // CPU 端光源缓存（剔除用）

    // Shader 热重载 — PSO 注册表
    struct PSORecord {
        rhi::PipelineStateDesc  desc;              // 完整 PSO 创建参数（含 ShaderBytecode 副本）
        rhi::ShaderBytecode     vsCopy;            // 顶点着色器字节码副本（自有 spirv 数据）
        rhi::ShaderBytecode     fsCopy;            // 片元着色器字节码副本
        String                  shaderNames[2];    // [0]=顶点shader名, [1]=片元shader名（如 "PBR.vert", "PBR.frag"）
        rhi::IRHIPipelineState* rawPSO = nullptr;  // 指向 m_PBR_PSO.get()
    };
    std::vector<PSORecord> m_PSORegistry;
    // 延迟销毁的旧 PSO（等 GPU 完成 3 帧后再释放 VkRenderPass）
    std::vector<std::pair<u32, std::unique_ptr<rhi::IRHIPipelineState>>> m_RetiredPSOs;

};

} // namespace he::render
