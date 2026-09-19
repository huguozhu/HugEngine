#pragma once

// ============================================================
// Lumen/LumenScene.h — Lumen 的持久资源宿主
//
// 【为什么要单独一个类】Lumen 的数据（Surface Cache atlas、Global SDF clipmap、探针缓冲）
// 都是**跨帧持久**的 GPU 资源，它们既不能走 RenderGraph 的瞬态分配 —— `rg.CreateTexture`
// 建出来的纹理 pass 拿不到 `IRHITexture*`（`RenderGraph.h:106-119` 没有由句柄取指针的接口，
// `PassExecuteFunc` 只收 `IRHICommandList*`）—— 也不该散落在 Provider 里：
//   · Provider 回答"每帧怎么算"；本类回答"数据放在哪、什么时候重建/释放"。
//
// 【生命周期】由 `DeferredPipeline` 在 Provider 注册之前 `Initialize`，并通过步骤 1 补上的
// Provider 生命周期遍历（`DeferredPipeline::OnResize/Shutdown` → `IGIProvider::OnResize/Shutdown`）
// 间接调用本类的 `OnResize/Shutdown`。这与既有 GI 源的做法一致：底层 pass 自己
// `device->CreateTexture/CreateBuffer` 并自持（`GI_RSM` / `GI_IBL` / `GI_DDGI`），
// 帧图只 `ImportTexture`、不拥有。
//
// 【当前状态】只有设备句柄与尺寸（步骤 3 的骨架）。后续步骤在这里追加真正的资源：
//   步骤 8  Mesh SDF（128³/mesh，R16F，按 mesh 缓存 + 上限）
//   步骤 10 Global SDF（512³ 单层 → 4×256³ clipmap）
//   步骤 14 Surface Cache 页表与页状态机
//   步骤 23 Screen Probe 的 SH 缓冲
// ============================================================

#include "RHI/RHI.h"
#include "Lumen/LumenSDF.h"
#include "Lumen/LumenTraceConfig.h"
#include "Lumen/SurfaceCacheTypes.h"

#include <memory>

namespace he::render {

/// Lumen 持久资源宿主（不参与每帧 orchestration —— 那是 `LumenProvider` 的事）
class LumenScene {
public:
    bool Initialize(rhi::IRHIDevice* device, u32 width, u32 height);
    void Shutdown();
    /// 视口尺寸变化：只重建与屏幕尺寸相关的资源；世界空间资源（atlas / clipmap）不在此重建
    void OnResize(u32 width, u32 height);

    [[nodiscard]] bool IsReady() const { return m_Device != nullptr; }
    [[nodiscard]] u32  GetWidth()  const { return m_Width; }
    [[nodiscard]] u32  GetHeight() const { return m_Height; }

    /// 屏幕空间的 Lumen 输出纹理（RGBA16F，可被 Lighting 采样）。
    /// 【骨架阶段】漫反射与镜面**共用这一张**；步骤 20~29 里漫反射来自 Screen Probe、
    /// 镜面来自反射路径，届时再拆成两张（并相应增加一个 binding）。
    [[nodiscard]] rhi::IRHITexture* GetOutput() const { return m_Output.get(); }
    [[nodiscard]] rhi::IRHISampler* GetOutputSampler() const { return m_OutputSampler.get(); }

    // ── 骨架阶段的全屏占位 pass（步骤 6）──
    // 帧图在 `BeginOffscreenPass` 之前调用它，使 RenderPass 与输出纹理的附件数一致；
    // 随后 `DrawSkeleton` 画一个全屏三角，颜色由 push constant 给定。
    void PreBind(rhi::IRHICommandList* cmd);
    /// value = 输出值；alpha < 0 表示"本条无数据"（合成端 skip），见 shader 里的说明
    void DrawSkeleton(rhi::IRHICommandList* cmd, float value, float alpha);

    /// 每帧推进 SDF 构建（步骤 8）：建档 → 逐帧构建 → 自检。由 LumenProvider::Render 调用。
    /// camPos：clipmap 近层跟随相机（必须在第一次调用之前给出，见 SetupGlobalGrid）。
    void StepSDF(rhi::IRHICommandList* cmd, const MeshBatcher& batcher, const float3& camPos) {
        m_SDF.SetCameraPos(camPos);
        m_SDF.Step(cmd, batcher);
    }
    [[nodiscard]] LumenSDF&       GetSDF()       { return m_SDF; }
    [[nodiscard]] const LumenSDF& GetSDF() const { return m_SDF; }

    /// 步骤 12：逐像素 SDF 追踪可视化（帧图的 SDF compute pass 每帧调用；相机参数由 Provider 传入）
    void RunSDFDebug(rhi::IRHICommandList* cmd, const float3& camPos, const float3& forward,
                     const float3& right, const float3& up, float tanHalfFov, float aspect) {
        m_SDF.RunDebugView(cmd, camPos, forward, right, up, tanHalfFov, aspect);
    }

    // ── L2 Surface Cache（步骤 14）：页表 + 页状态机 ──
    /// 用步骤 13 的卡片清单建页表（每张卡一页），并把页表镜像到 GPU 缓冲
    void BuildPageTable();
    /// 每帧推进：一次 GPU 一致性校验（读回镜像的校验和，与 CPU 侧比对）
    void StepSurfaceCache(rhi::IRHICommandList* cmd);
    /// 步骤 15：Card 捕获 —— 逐卡（本帧预算内、状态为 Capturing 的页）软件光栅化写 atlas
    void RunCardCapture(rhi::IRHICommandList* cmd, rhi::IRHITexture* gbAlbedo, rhi::IRHITexture* gbNormal,
                        rhi::IRHITexture* gbDepth, const float4x4& viewProj);
    /// 步骤 20：Screen Probe 布置与自适应合并（16×16 单元；2×2 平坦单元合并成一个探针）
    void RunProbePlacement(rhi::IRHICommandList* cmd, rhi::IRHITexture* gbNormal, rhi::IRHITexture* gbWorldPos);
    /// 步骤 21：探针半球追踪（GGX 重要性采样 + SDF march），命中结果供步骤 22 着色
    void RunProbeTrace(rhi::IRHICommandList* cmd);
    /// 步骤 22：命中点着色（从 L2 的 atlas 取材质；缺页返回中性值并记数）
    void RunSurfaceCacheShading(rhi::IRHICommandList* cmd, rhi::IRHITexture* gbAlbedo,
                                rhi::IRHITexture* gbWorldPos, const float4x4& viewProj);
    [[nodiscard]] u32 GetShadedHits() const { return m_ShadedHits; }
    [[nodiscard]] u32 GetShadedMissingPages() const { return m_ShadedMissingPages; }
    [[nodiscard]] float GetShadedAlbedoMeanDiff() const { return m_ShadedAlbedoMeanDiff; }
    [[nodiscard]] u32 GetShadedAlbedoSamples() const { return m_ShadedAlbedoSamples; }
    [[nodiscard]] u32 GetProbeRayHits() const { return m_ProbeRayHits; }
    [[nodiscard]] u32 GetProbeRayMisses() const { return m_ProbeRayMisses; }
    [[nodiscard]] u32 GetProbeRayHemisphere() const { return m_ProbeRayHemisphere; }   // 点积 > 0 的光线数
    [[nodiscard]] u32 GetProbeRaysTotal() const { return m_ProbeRaysTotal; }
    [[nodiscard]] const LumenTraceConfig& GetTraceConfig() const { return m_TraceConfig; }
    [[nodiscard]] u32 GetProbeCount() const { return m_ProbeCount; }
    [[nodiscard]] u32 GetProbeTilesFlat() const { return m_ProbeTilesFlat; }        // 偏差缓冲里"够平坦"的 tile 数
    [[nodiscard]] u32 GetProbeTilesTotal() const { return m_ProbeTilesTotal; }      // 有几何的 tile 数
    [[nodiscard]] float GetProbeMergeThreshold() const { return m_MergeNormalCos; }
    [[nodiscard]] const std::vector<float>& GetProbeTileDev() const { return m_ProbeTileDev; }
    /// 步骤 16：Feedback —— 16×16 分块产出"需要哪些页"的请求，C++ 侧排序后写回页表状态
    void RunFeedback(rhi::IRHICommandList* cmd, rhi::IRHITexture* gbWorldPos, const float3& camPos);
    [[nodiscard]] u32  GetFeedbackRequests() const { return m_FeedbackRequests; }
    [[nodiscard]] u32  GetFeedbackTopOverlap() const { return m_FeedbackTopOverlap; }
    [[nodiscard]] u32  GetFeedbackTopCount() const { return m_FeedbackTopCount; }
    [[nodiscard]] rhi::IRHITexture* GetCardAtlasAlbedo()  const { return m_AtlasAlbedo.get(); }
    [[nodiscard]] rhi::IRHITexture* GetCardAtlasNormal()  const { return m_AtlasNormal.get(); }
    [[nodiscard]] u32 GetCardCaptureHits() const { return m_CardCaptureHits; }
    [[nodiscard]] u32 GetCardCaptureMisses() const { return m_CardCaptureMisses; }
    [[nodiscard]] const SurfaceCachePageTable& GetPageTable() const { return m_PageTable; }
    [[nodiscard]] bool  IsPageTableCheckDone() const { return m_PageCheckDone; }
    [[nodiscard]] bool  IsPageTableCheckPassed() const { return m_PageCheckPassed; }

private:
    void CreateOutput();
    void CreateSkeletonPipeline();
    void DestroySkeletonPipeline();
    void CreatePageCheckGPUObjects();
    void CreateCaptureGPUObjects();

    rhi::IRHIDevice* m_Device = nullptr;
    u32 m_Width  = 0;
    u32 m_Height = 0;
    std::unique_ptr<rhi::IRHITexture> m_Output;   // 屏幕空间输出（与视口同尺寸）
    std::unique_ptr<rhi::IRHISampler> m_OutputSampler;
    // 占位 pass 的管线状态（单颜色附件 RGBA16F、无深度）。不随视口尺寸变化，只建一次。
    std::unique_ptr<rhi::IRHIPipelineState> m_SkeletonPSO;
    // 逐 mesh 距离场（步骤 8）
    LumenSDF m_SDF;
    // ── Surface Cache 页表（步骤 14）──
    SurfaceCachePageTable m_PageTable;
    bool m_PageTableBuilt = false;
    std::unique_ptr<rhi::IRHIBuffer>  m_PageTableBuf;    // GPU 侧镜像（StructuredBuffer）
    std::unique_ptr<rhi::IRHIBuffer>  m_PageCheckOut;    // GPU 校验和（CPU 可读）
    void* m_PageCheckOutMapped = nullptr;                // 持久映射（与 SDF 探针缓冲同做法）
    rhi::DescriptorSetLayoutHandle    m_PageCheckLayout = 0;
    rhi::DescriptorSetHandle          m_PageCheckSet    = 0;
    std::unique_ptr<rhi::IRHIPipelineState> m_PageCheckPSO;
    bool m_PageCheckBound = false;
    u32  m_PageCheckFrame = 0;
    bool m_PageCheckDone = false;
    bool m_PageCheckPassed = false;
    // ── Card 捕获（步骤 15）──
    static constexpr u32 kAtlasPageRes  = 64;    // 每页 64×64 texel
    static constexpr u32 kAtlasGridDim  = 8;     // atlas = 8×8 页 = 512×512
    static constexpr u32 kAtlasSize     = kAtlasPageRes * kAtlasGridDim;
    // ── 步骤 17：三个显式预算（《Lumen设计与实现》§4）──
    // 捕获 = 本帧最多写几张卡；分配 = 本帧最多分配几个物理页；反馈 = 本帧最多采纳多少条请求。
    // 三者分开是为了让"卡顿尖峰"没有来源：任何一帧的工作量都被这三个数夹住，
    // 没做完的请求留在 Requested，下一帧继续（不回退、不丢弃）。
    u32 m_BudgetCaptures     = 8;
    u32 m_BudgetAllocations  = 8;
    u32 m_BudgetFeedbackPages = 256;
    // 【验收实验】把下面三行改成 4/4/128 即可复现"预算减半 ⇒ 收敛变慢但不出现尖峰"（见 §附二十四）
    static constexpr u32 kMaxCapturesPerFrame = 8;   // 兼容旧名字（= 预算上限的默认值）
    [[nodiscard]] u32 GetBudgetCaptures() const { return m_BudgetCaptures; }
    [[nodiscard]] u32 GetBudgetAllocations() const { return m_BudgetAllocations; }
    [[nodiscard]] u32 GetBudgetFeedbackPages() const { return m_BudgetFeedbackPages; }
    /// 调整预算（验收用：预算减半 ⇒ 收敛变慢但不出现尖峰）
    void SetBudgets(u32 captures, u32 allocations, u32 feedbackPages) {
        m_BudgetCaptures      = std::max(1u, captures);
        m_BudgetAllocations   = std::max(1u, allocations);
        m_BudgetFeedbackPages = std::max(1u, feedbackPages);
    }
    [[nodiscard]] u32 GetPagesCapturedTotal() const { return m_PagesCapturedTotal; }
    [[nodiscard]] u32 GetMaxCapturesInAFrame() const { return m_MaxCapturesInAFrame; }
    /// 验收用：打开"合成漫游"（静态相机下人为轮换需要的页，逼出 LRU 淘汰）
    void SetSyntheticRoaming(bool on) { m_SyntheticRoaming = on; }
    [[nodiscard]] u32 GetEvictions() const { return m_Evictions; }
    [[nodiscard]] u32 GetAllocFailures() const { return m_AllocFailures; }
    std::unique_ptr<rhi::IRHITexture> m_AtlasAlbedo, m_AtlasNormal, m_AtlasEmissive;
    std::unique_ptr<rhi::IRHIBuffer>  m_CaptureStats;
    void* m_CaptureStatsMapped = nullptr;
    std::unique_ptr<rhi::IRHIBuffer>  m_CaptureFrameBuf;      // 每帧常量（push constant 只有 128B 上限）
    void* m_CaptureFrameMapped = nullptr;
    rhi::DescriptorSetLayoutHandle m_CaptureLayout = 0;
    rhi::DescriptorSetHandle       m_CaptureSet    = 0;
    std::unique_ptr<rhi::IRHIPipelineState> m_CapturePSO;
    bool m_CaptureBound = false;
    u32  m_CardCaptureHits = 0, m_CardCaptureMisses = 0;
    u32  m_PagesCapturedTotal = 0;      // 累计捕获页数（收敛曲线）
    // ── 步骤 18：物理页池 + LRU 淘汰（逻辑页可以远多于物理页）──
    // 逻辑页数 = min(卡片数, 1024)（§4 的"1024 页上限"），物理页 = atlas 的 8×8 = 64 块。
    // 固定池 + LRU 下不存在"碎片"：分配不到就淘汰最久未用的（Captured/Dirty）页，分配成功率恒 100%。
    std::vector<u32> m_FreePhysical;    // 空闲物理页栈
    std::vector<u32> m_PhysOwner;       // 物理页 -> 逻辑页（0xFFFFFFFF = 空闲）
    u32  m_PhysicalPages = 0;
    u32  m_AllocSuccess = 0, m_AllocFailures = 0, m_Evictions = 0;
    bool m_SyntheticRoaming = false;    // 验收用：合成"漫游"（静态相机下人为轮换需要页，逼出淘汰路径）
    /// 物理页池 + LRU：为逻辑页分配一个物理页（必要时淘汰最久未用者）
    bool AllocatePhysicalPage(u32 logicalPage,u32 frame);
    u32  FreePhysicalPages() const { return (u32)m_FreePhysical.size(); }
    u32  m_MaxCapturesInAFrame = 0;     // 单帧最多捕获了几页（应当 ≤ 预算 ⇒ 无尖峰）
    // ── Feedback（步骤 16）──
    static constexpr u32 kMaxFeedbackTiles = 16384;  // 槽位数上限（= 512×512 屏幕的 16×16 块数；1080p 只需 8160）
    std::unique_ptr<rhi::IRHIBuffer>  m_CardBuf, m_ReqCountBuf, m_ReqBuf;
    void* m_ReqCountMapped = nullptr;
    void* m_ReqMapped = nullptr;
    rhi::DescriptorSetLayoutHandle m_FeedbackLayout = 0;
    rhi::DescriptorSetHandle       m_FeedbackSet    = 0;
    std::unique_ptr<rhi::IRHIPipelineState> m_FeedbackPSO;
    bool m_FeedbackBound = false;
    u32  m_FeedbackFrame = 0;
    u32  m_FeedbackRequests = 0, m_FeedbackTopOverlap = 0, m_FeedbackTopCount = 0;
    // ── Screen Probe（步骤 20）──
    static constexpr u32 kMaxScreenProbes = 65536;    // 8K 探针量级（1080p 96×54 tile × 4）
    float m_MergeNormalCos = 0.995f;                  // 法线一致阈值（cos；越大越严格 ⇒ 探针越多）
    std::unique_ptr<rhi::IRHIBuffer> m_ProbeBuf, m_ProbeCountBuf, m_TileDevBuf;
    void* m_ProbeCountMapped = nullptr;
    void* m_TileDevMapped = nullptr;
    rhi::DescriptorSetLayoutHandle m_ProbeLayout = 0;
    rhi::DescriptorSetHandle       m_ProbeSet    = 0;
    std::unique_ptr<rhi::IRHIPipelineState> m_ProbePSO;
    bool m_ProbeBound = false;
    u32  m_ProbeFrame = 0;
    u32  m_ProbeCount = 0, m_ProbeTilesFlat = 0, m_ProbeTilesTotal = 0;
    std::vector<float> m_ProbeTileDev;
    // ── 探针半球追踪（步骤 21）──
    LumenTraceConfig m_TraceConfig;
    std::unique_ptr<rhi::IRHIBuffer> m_RayResultBuf, m_RayStatsBuf;
    void* m_RayStatsMapped = nullptr;
    rhi::DescriptorSetLayoutHandle m_TraceLayout = 0;
    rhi::DescriptorSetHandle       m_TraceSet    = 0;
    std::unique_ptr<rhi::IRHIPipelineState> m_TracePSO;
    bool m_TraceBound = false;
    u32  m_TraceFrame = 0;
    u32  m_ProbeRayHits = 0, m_ProbeRayMisses = 0, m_ProbeRaysTotal = 0, m_ProbeRayHemisphere = 0;
    // ── 命中点着色（步骤 22）──
    std::unique_ptr<rhi::IRHIBuffer> m_RayHitPosBuf, m_ShadeCardsBuf, m_ShadeOutBuf, m_ShadeOutGbBuf, m_ShadeOutBestBuf, m_ShadeStatsBuf;
    void* m_ShadeStatsMapped = nullptr;
    rhi::DescriptorSetLayoutHandle m_ShadeLayout = 0;
    rhi::DescriptorSetHandle       m_ShadeSet    = 0;
    std::unique_ptr<rhi::IRHIPipelineState> m_ShadePSO;
    bool m_ShadeBound = false;
    u32  m_ShadeFrame = 0;
    u32  m_ShadedHits = 0, m_ShadedMissingPages = 0, m_ShadedAlbedoSamples = 0;
    u32  m_ShadedNoCard = 0;   // 命中点不落在任何卡片 AABB 内（步骤 22 的缺页归因）
    float m_ShadedAlbedoMeanDiff = 0.0f;      // 单卡覆盖样本的平均 |Δalbedo|
    float m_ShadedAlbedoMeanDiffMulti = 0.0f; // 多卡覆盖样本的平均 |Δalbedo|（选卡可能选错）
    float m_ShadedAlbedoBestDiff = 0.0f;      // 多卡覆盖下"最贴合 GBuffer 的候选"的平均 |Δalbedo|（归因下界）
    u32   m_ShadedAlbedoBestSamples = 0;

    std::vector<u32> m_LastTopPages;
    void CreateFeedbackGPUObjects();
    void CreateProbeGPUObjects();
    void CreateProbeTraceGPUObjects();
    void CreateShadeGPUObjects();
    u32  m_CapturePages = 0;          // 累计捕获页数
    u32  m_CardCaptureMarchHits = 0;  // 诊断：SDF march 命中数
    bool m_CaptureStatsPending = false;
    bool m_CaptureStatsLogged  = false;
};

} // namespace he::render
