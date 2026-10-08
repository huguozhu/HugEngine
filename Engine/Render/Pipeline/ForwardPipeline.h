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
#include "Pipeline/SkinnedMeshBuffers.h"   // 第③段：骨骼矩阵缓冲的渲染侧状态表
#include "AntiAliasing/AntiAliasing.h"
// 阶段 1 T1.3b：光源走快照（`FrameSceneSnapshot`）而不是直接遍历 ECS。
#include "Threading/FrameSceneSnapshot.h"
// 第③段第 4 批：快照装配器（在 Engine/Render/Threading 白名单层里读世界；管线只配置口径）
#include "Threading/FrameSnapshotAssembler.h"
// 阶段 1 附录 E：meshIndex → 渲染侧资源 的注册表（快照不带指针的前提）
#include "Threading/MeshRegistry.h"
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
    void RenderScene(rhi::IRHICommandList* cmd, const CameraData& camera);
    /// GPU 视锥剔除：收集场景对象 → 上传 GPUScene SSBO → 读回上帧可见性 → Dispatch Compute。
    /// **必须在任何 render pass 之外调用**：vkCmdDispatch 不允许出现在 render pass 内部
    /// （VUID-vkCmdDispatch-None-10672），且它会采样 HDR 深度 —— 那正是本帧 Scene pass 的
    /// 深度附件，在 pass 内采样构成非法反馈。RG 路径由独立的 "GPU_Cull" compute pass 调用，
    /// 非 RG 路径在 BeginHDRPass 之前调用。
    void RunGPUCulling(rhi::IRHICommandList* cmd, const CameraData& camera);
    void EndFrame(rhi::IRHICommandList* cmd);

    // RenderGraph 模式（声明式 Pass 编排，自动 Barrier）
    void BuildFrameGraph(RenderGraph& rg, const CameraData& camera);
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
    void PrepareGI(rhi::IRHICommandList* cmd);
    GI_RSM* GetRSM() { return m_RSM.get(); }

    // 后处理（委托给子系统）
    void RenderToneMapPass(rhi::IRHICommandList* cmd);
    void RenderSkybox(rhi::IRHICommandList* cmd, const CameraData& camera);

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

    // ── 阶段 1 §15.1 第③段：快照交接（样例 → 管线）──
    /// **快照装配器**（游戏线程用）：样例每帧按
    /// `AssembleScene(world, sg, camera)` → 阴影收集 → `ResolveLightShadowIndices(...)` 的顺序调用，
    /// 然后把 `GetFrameSnapshot()` 交给 `Render`。
    /// 【为什么装配器在 Threading 白名单层】取齐渲染输入天然要读世界，而**管线的帧入口必须不收世界**
    /// （附录 B1 的收敛目标）；把这段代码放到白名单层、只把口径配置进管线，两侧要求同时满足。
    FrameSnapshotAssembler& GetFrameAssembler() { return m_Assembler; }
    /// 本帧快照（`AssembleScene` 之后有效）
    const FrameSceneSnapshot& GetFrameSnapshot() const { return m_Snapshot; }
    /// 网格注册表（阴影技术等按 `meshIndex` 取顶点/索引缓冲）
    const MeshRegistry& GetMeshRegistry() const { return m_MeshRegistry; }

private:
    void CollectLights(PushConstantData& pc);
    void UploadMaterialBindless();  // 从**快照**取已去重的材质数组 → 写入 bindless 材质 SSBO 并注册（须在 heap->Flush() 前调用）
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
    void RefreshRSMFrustum(const CameraData& camera);
    rhi::IRHIDevice* m_Device = nullptr;
    std::unique_ptr<rhi::IRHIPipelineState> m_PBR_PSO;
    // 蒙皮网格 PSO（C1b）：扩展顶点布局（location 3/4 = JOINTS/WEIGHTS），同着色器
    std::unique_ptr<rhi::IRHIPipelineState> m_PBR_Skinned_PSO;

    rhi::DescriptorSetLayoutHandle m_PerFrameLayout = rhi::kInvalidLayout;  // set=0: per-frame + bindless
    rhi::DescriptorSetHandle       m_DescSets[MAX_FRAMES_IN_FLIGHT] = {};   // set=0 三缓冲
    std::unique_ptr<rhi::IRHIBuffer> m_LightBuffers[MAX_FRAMES_IN_FLIGHT];
    // 阶段 1 T1.3b：本帧光源的**快照**（游戏线程侧收集的不可变输入）。
    // 必须是成员：帧图 lambda 在 `CollectLights` 返回之后才执行，局部变量会悬垂。
    FrameSceneSnapshot               m_Snapshot;

    // 阶段 1 附录 E（E-2）：网格注册表。
    // 【为什么每帧刷新】骨骼缓冲会重建（N 帧延迟队列后新建）⇒ 只登记一次会留下过期指针；
    // `Register` 同 key = 更新（索引不变），因此每帧刷新廉价且安全。帧内只读。
    MeshRegistry                     m_MeshRegistry;
    /// 本帧快照装配器（第③段第 4 批：在 `Threading/` 白名单层读世界；管线只配置口径）。
    /// 【为什么不是管线自己收 world】`BuildFrameSnapshot(World&, SceneGraph&, …)` 本身就是 B1 命中 ——
    /// 装配必须发生在白名单层、由样例驱动，帧入口才能真的不收世界。
    FrameSnapshotAssembler           m_Assembler;
    /// 本帧快照指针（`Render` 入口赋值；各 helper 与帧图 lambda 都通过 `FrameSnap()` 读它）。
    /// 【生命周期】帧图 lambda 在 `Render` 内部执行 ⇒ 指向调用方快照的指针始终有效。
    const FrameSceneSnapshot*        m_FrameSnapshot = nullptr;
    [[nodiscard]] const FrameSceneSnapshot& FrameSnap() const { return *m_FrameSnapshot; }
    /// 本帧快照容量是否已按实际规模预留过一次（自校准由装配器的 `ReserveOnce()` 负责）
    bool                             m_SnapshotReserved = false;
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
    // RenderGraph 模式（默认开启，声明式编排为正式路径；可经 SetUseRenderGraph 切回旧路径）
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
    // 与 Deferred 侧同一套做法：视锥由纯几何函数 `FitRSMFrustumToBounds` 拟合，
    // 结果缓存给 frame graph 与 UBO 两处消费者。
    // 【第③段第 4 批】原先这里还缓存"每 30 帧重算一次的场景包围盒"（遍历世界算的）——
    // 现在包围盒由装配器按同一公式算好放进快照（`sceneBoundsMin/Max`），本类不再持有它。
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
    /// 骨骼矩阵缓冲的渲染侧状态（第③段：与实例缓冲同款，按 `meshIndex` 索引；组件只留数据源）
    SkinnedMeshBuffers m_SkinnedBuffers;
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
