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
#include "Lumen/LumenFarFieldPass.h"
#include "Lumen/LumenSDF.h"
#include "Lumen/LumenTraceConfig.h"
#include "Lumen/SurfaceCacheTypes.h"
#include "PostProcess/DenoiseSignal.h"   // 步骤 35：时域历史走统一池（DenoiseHistoryPool）

#include <memory>

namespace he::render {

/// Lumen 持久资源宿主（不参与每帧 orchestration —— 那是 `LumenProvider` 的事）
/// 探针半球采样模式（与 `ScreenProbeSampling.slang` 的约定一致，追踪与投影必须同值）
enum LumenProbeSampleMode : u32 {
    kSampleModeGgx               = 0u,   // 步骤 21 原样：GGX 半向量采样
    kSampleModeUniformHemisphere = 1u,   // 步骤 23 起默认：均匀半球（pdf = 1/(2π)，白炉 SH 的 l0 有解析值）
};

class LumenScene {
public:
    bool Initialize(rhi::IRHIDevice* device, u32 width, u32 height);
    void Shutdown();
    /// 视口尺寸变化：只重建与屏幕尺寸相关的资源；世界空间资源（atlas / clipmap）不在此重建
    void OnResize(u32 width, u32 height);

    [[nodiscard]] bool IsReady() const { return m_Device != nullptr; }
    /// 【步骤 37】首帧把 Lumen 自持的**存储图像**统一转换到 GENERAL（UnorderedAccess）。
    ///
    /// 【为什么必须有这一步】Lumen 的内部 pass 只用"全局内存屏障"（`ComputeBarrier`），它不管布局；
    /// 而这些纹理是自持的（帧图不会替它们转换）⇒ 它们从未进入 GENERAL，校验层提交时报
    /// "expects VK_IMAGE_LAYOUT_GENERAL — instead … UNDEFINED"（基线里 10 行），而按规范此时的
    /// 存储写是未定义行为（本步实测到过"统计正常、图像整幅黑"的形态）。在**首个** Lumen 计算
    /// pass 里转换一次即可（`Undefined → GENERAL` 会丢弃内容，而首帧的内容本来就是垃圾）。
    void TransitionStorageImagesOnce(rhi::IRHICommandList* cmd);
    /// 【步骤 37】逐 mesh 距离场的每帧构建预算是否已跑完（区分启动期与稳态帧时）
    [[nodiscard]] bool IsMeshBuildComplete() const { return m_SDF.IsMeshBuildComplete(); }
    [[nodiscard]] u32  GetWidth()  const { return m_Width; }
    [[nodiscard]] u32  GetHeight() const { return m_Height; }

    /// 【步骤 35】把"降噪历史"的统一分配池交给本类（非拥有；须在 Initialize 之前或之后、首帧之前设置）。
    /// 探针的时域历史（两份探针镜像 + 两份单元映射）从这里取，与其他 GI 源的历史资源同账。
    void SetHistoryPool(DenoiseHistoryPool* pool) { m_HistoryPool = pool; }

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

    /// 步骤 12：逐像素 SDF 追踪可视化（帧图的 SDF compute pass 每帧调用；相机参数由 Provider 传入）。
    ///
    /// 【步骤 37：默认关掉】它是一张**只给工具看**的调试纹理（`lumen_sdf_trace` 转储、L1 验收用），
    /// 却每帧都在全屏跑一次 sphere tracing —— 实测稳态占 1.67 ms，是 Lumen 计算 pass（4.9 ms）的 34%。
    /// 关掉它**不影响画面**（它不是 GI 输出，没有任何 pass 采样它）。需要转储时用
    /// `HE_LUMEN_SDF_DEBUG=1` 打开（`build/verify/lumen_smoke.ps1` 已经这么设）。
    void RunSDFDebug(rhi::IRHICommandList* cmd, const float3& camPos, const float3& forward,
                     const float3& right, const float3& up, float tanHalfFov, float aspect) {
        if (!m_SdfDebugView) return;
        m_SDF.RunDebugView(cmd, camPos, forward, right, up, tanHalfFov, aspect);
    }
    [[nodiscard]] bool IsSdfDebugViewEnabled() const { return m_SdfDebugView; }

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
    /// 步骤 23：把每条光线结果投成二阶 SH（4 系数 RGB），写回探针的 shR/shG/shB；
    /// `furnace = true` 时强制辐射度 L ≡ 1（白炉），此时 l0 必须等于 √π（解析值，可断言）。
    void RunScreenProbeSHProject(rhi::IRHICommandList* cmd, bool furnace);
    [[nodiscard]] float GetSHMeanL0() const { return m_SHMeanL0; }
    [[nodiscard]] float GetSHFurnaceL0Deviation() const { return m_SHFurnaceL0Dev; }
    [[nodiscard]] float GetSHIrradianceMeanDiff() const { return m_SHIrradianceDiff; }
    [[nodiscard]] u32 GetSHProbes() const { return m_SHProbes; }
    [[nodiscard]] u32 GetSHRays() const { return m_SHRays; }
    // ── 步骤 35：Screen Probe 的空间 + 时域滤波（§10 统一降噪框架的 Lumen 信号）──
    /// 3×3 单元（YCoCg AABB）+ 时域重投影 EMA。必须在 SH 投影之后、逐像素辐照度之前调用：
    /// 下游（辐照度 / DDGI 输入）读的是**过滤后**的探针缓冲。
    void RunScreenProbeFilter(rhi::IRHICommandList* cmd, const float4x4& viewProj);
    /// 过滤后的探针缓冲（下游唯一该读的那一份）
    [[nodiscard]] rhi::IRHIBuffer* GetProbeFilteredBuffer() const { return m_ProbeFilteredBuf.get(); }
    /// 滤波模式：0 = 直通（= 没有这一步），1 = 仅空间，2 = 空间 + 时域（默认）
    [[nodiscard]] u32 GetProbeFilterMode() const { return m_ProbeFilterMode; }
    /// 噪声读数：l0 亮度的 std/mean（输入 = 未过滤；输出 = 过滤后），同一次运行内自成对照
    [[nodiscard]] float GetProbeNoiseIn() const  { return m_ProbeNoiseIn; }
    [[nodiscard]] float GetProbeNoiseOut() const { return m_ProbeNoiseOut; }
    [[nodiscard]] float GetProbeL0MeanIn() const  { return m_ProbeL0MeanIn; }
    [[nodiscard]] float GetProbeL0MeanOut() const { return m_ProbeL0MeanOut; }
    /// 有历史的探针里，"本帧输出 vs 上一帧历史"的平均相对变化（时域稳定性读数）
    [[nodiscard]] float GetProbeFrameChange() const { return m_ProbeFrameChange; }
    [[nodiscard]] u32 GetProbeHistoryUsed() const { return m_ProbeHistoryUsed; }
    /// 探针身份声明（供信号登记打印一条可核对的说明）
    [[nodiscard]] const char* GetProbeFilterNote() const;
    /// 步骤 24：由探针 SH 采样出逐像素入射辐照度（写进屏幕尺寸的辐照度纹理）
    void RunProbeIrradiance(rhi::IRHICommandList* cmd, rhi::IRHITexture* gbNormal,
                            rhi::IRHITexture* gbAlbedo, rhi::IRHITexture* gbWorldPos);
    /// 步骤 24 的辐照度纹理（Provider 的输出 pass 采样它写进 GI 输出）
    [[nodiscard]] rhi::IRHITexture* GetIrradianceTexture() const { return m_IrradianceTex.get(); }
    [[nodiscard]] float GetIrradianceMean() const { return m_IrradianceMean; }
    [[nodiscard]] float GetIrradianceMax() const { return m_IrradianceMax; }
    [[nodiscard]] u32   GetIrradianceCoveredPixels() const { return m_IrradianceCovered; }
    /// 步骤 24：把辐照度纹理贴到 Provider 输出（帧图在 offscreen pass 里调它）
    void DrawIrradiance(rhi::IRHICommandList* cmd, float gain);

    // ── 步骤 26：远场硬件光追 ──
    /// 每帧由帧图注入 RT 侧输入（TLAS 与场景材质纹理来自 RTPass；光源来自本帧的光源缓冲）
    void SetRTInputs(rhi::IRHIAccelerationStructure* tlas, rhi::IRHITexture* materialTex,
                     rhi::IRHITexture* triangleNormals, rhi::IRHIBuffer* lightBuffer, u32 lightCount) {
        m_TLAS = tlas; m_RTMaterialTex = materialTex; m_RTTriangleNormals = triangleNormals;
        m_RTLightBuffer = lightBuffer; m_RTLightCount = lightCount;
    }
    /// 用与步骤 21 **完全相同**的探针光线发一次硬件光追（远场），并与 SDF 结果逐光线比较
    void RunFarFieldRT(rhi::IRHICommandList* cmd);
    [[nodiscard]] bool  IsFarFieldReady() const { return m_FarField != nullptr; }
    [[nodiscard]] u32   GetFarFieldRays() const { return m_FarFieldRays; }
    [[nodiscard]] u32   GetFarFieldBothHit() const { return m_FarFieldBothHit; }
    [[nodiscard]] float GetFarFieldMeanRelDiff() const { return m_FarFieldMeanRel; }
    [[nodiscard]] float GetFarFieldMaxRelDiff() const { return m_FarFieldMaxRel; }
    [[nodiscard]] float GetFarFieldAgree5Pct() const { return m_FarFieldAgree5; }
    [[nodiscard]] float GetFarFieldAgree20Pct() const { return m_FarFieldAgree20; }
    [[nodiscard]] u32   GetFarFieldSdfOnly() const { return m_FarFieldSdfOnly; }
    [[nodiscard]] u32   GetFarFieldRtOnly() const { return m_FarFieldRtOnly; }
    [[nodiscard]] const std::vector<u32>& GetFarFieldRelHist() const { return m_FarFieldHist; }
    [[nodiscard]] float GetFarFieldNearAgree20() const { return m_FarFieldNear20; }
    [[nodiscard]] float GetFarFieldFarAgree20() const { return m_FarFieldFar20; }
    [[nodiscard]] float GetFarFieldNearMeanRel() const { return m_FarFieldNearMeanRel; }
    [[nodiscard]] float GetFarFieldFarMeanRel() const { return m_FarFieldFarMeanRel; }
    [[nodiscard]] float GetFarFieldOverlap() const { return m_FarFieldOverlap; }
    void SetFarFieldOverlap(float v) { m_FarFieldOverlap = v; }
    [[nodiscard]] u32 GetFadeSdfOnly() const { return m_FadeSdfOnly; }
    [[nodiscard]] u32 GetFadeRtOnly() const { return m_FadeRtOnly; }
    [[nodiscard]] u32 GetFadeBlend() const { return m_FadeBlend; }
    [[nodiscard]] u32 GetFadeBlendedRays() const { return m_FadeBlendedRays; }
    [[nodiscard]] u32 GetFadeBandRays() const { return m_FadeBandRays; }
    [[nodiscard]] u32 GetFadeNoAltRays() const { return m_FadeNoAltRays; }
    [[nodiscard]] const std::vector<u32>& GetDistBinCount() const { return m_DistBinCount; }
    [[nodiscard]] const std::vector<u32>& GetDistBinLum() const { return m_DistBinLum; }
    [[nodiscard]] u32 GetShadedMissingPages() const { return m_ShadedMissingPages; }
    [[nodiscard]] float GetShadedAlbedoMeanDiff() const { return m_ShadedAlbedoMeanDiff; }
    [[nodiscard]] u32 GetShadedAlbedoSamples() const { return m_ShadedAlbedoSamples; }
    [[nodiscard]] u32 GetProbeRayHits() const { return m_ProbeRayHits; }
    [[nodiscard]] u32 GetProbeRayMisses() const { return m_ProbeRayMisses; }
    [[nodiscard]] u32 GetProbeRayHemisphere() const { return m_ProbeRayHemisphere; }   // 点积 > 0 的光线数
    [[nodiscard]] u32 GetProbeRaysTotal() const { return m_ProbeRaysTotal; }
    [[nodiscard]] const LumenTraceConfig& GetTraceConfig() const { return m_TraceConfig; }
    [[nodiscard]] u32 GetProbeCount() const { return m_ProbeCount; }
    /// 步骤 31：把 Screen Probe 的结果交给 DDGI（Radiance Cache）当作输入
    [[nodiscard]] rhi::IRHIBuffer* GetProbeBuffer() const { return m_ProbeBuf.get(); }
    [[nodiscard]] rhi::IRHIBuffer* GetCellProbeBuffer() const { return m_CellProbeBuf.get(); }
    [[nodiscard]] u32 GetScreenCellsX() const { return (m_Width + 15u) / 16u; }
    [[nodiscard]] u32 GetScreenCellsY() const { return (m_Height + 15u) / 16u; }
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
    /// 计算 pass 之间的显式屏障：Lumen 的每个内部 pass 都读"上一个 pass 刚写的缓冲/纹理"，
    /// 而它们**在同一个 pass 内连续 Dispatch**、不对帧图声明依赖 ⇒ 缺少屏障。
    /// 【实测后果】探针缓冲"布置写 → 追踪读 → SH 写"存在写后读/读后写竞态：背靠背两次运行
    /// 的 hit 数完全一致，但探针 SH 的 l0 均值不同（0.3274 vs 0.3249），逐像素辐照度 98.6% 不同。
    void ComputeBarrier(rhi::IRHICommandList* cmd);
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
    /// 【步骤 37】槽位缓冲**双缓冲**：GPU 写第 N 帧的槽位，CPU 同时读第 N-1 帧的槽位。
    /// 此前只有一块缓冲，CPU 在 GPU 正在写它的时候逐元素读（实测那一读要 12.95 ms）——
    /// 读到的内容取决于 GPU 当时写到哪了（实测同一配置两次运行请求数 8103/8115 不同），
    /// 这是一个**数据竞争**：算法没错，但读数不确定、画面也就不可复现。
    static constexpr u32 kFeedbackSlots = 2;
    std::unique_ptr<rhi::IRHIBuffer>  m_CardBuf, m_ReqCountBuf;
    std::unique_ptr<rhi::IRHIBuffer>  m_ReqBuf[kFeedbackSlots];
    void* m_ReqCountMapped = nullptr;
    void* m_ReqMapped[kFeedbackSlots] = {nullptr, nullptr};
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
    // ── SH 投影（步骤 23）──
    std::unique_ptr<rhi::IRHIBuffer> m_IrradShBuf, m_IrradRefBuf, m_SHStatsBuf;
    void* m_SHStatsMapped = nullptr;
    rhi::DescriptorSetLayoutHandle m_SHLayout = 0;
    rhi::DescriptorSetHandle       m_SHSet    = 0;
    std::unique_ptr<rhi::IRHIPipelineState> m_SHPSO;
    bool m_SHBound = false;
    u32  m_SHFrame = 0;
    u32  m_SHProbes = 0, m_SHRays = 0, m_SHIrradianceSamples = 0;
    float m_SHMeanL0 = 0.0f;            // 所有探针 l0 的均值（白炉下应为 √π ≈ 1.7725）
    float m_SHFurnaceL0Dev = 0.0f;      // 白炉下 l0 相对 √π 的**平均绝对偏差**（验收口径"误差为 0"）
    float m_SHIrradianceDiff = 0.0f;    // SH 重建辐照度 vs 逐光线求和参考的平均相对差
    // ── 步骤 35：探针滤波（空间 3×3 单元 YCoCg AABB + 时域重投影 EMA）──
    DenoiseHistoryPool* m_HistoryPool = nullptr;   // 非拥有：时域历史由统一池分配
    bool m_StorageImagesTransitioned = false;       // 步骤 37：存储图像的布局是否已转换过
    rhi::DescriptorSetLayoutHandle m_ProbeFilterLayout = 0;
    rhi::DescriptorSetHandle       m_ProbeFilterSet    = 0;
    std::unique_ptr<rhi::IRHIPipelineState> m_ProbeFilterPSO;
    std::unique_ptr<rhi::IRHIBuffer> m_ProbeFilteredBuf;             // 过滤结果（96 B/探针，下游读它）
    std::unique_ptr<rhi::IRHIBuffer> m_ProbeFilterHistBuf[2];        // 历史探针镜像（80 B/探针，乒乓）
    std::unique_ptr<rhi::IRHIBuffer> m_CellProbeHistBuf[2];          // 历史单元映射（乒乓）
    /// 历史资源来自统一池时用这两个（池持有，本类只借；为空表示池没给，退回自建）
    rhi::IRHIBuffer* m_ProbeHistFromPool[2] = {nullptr, nullptr};
    rhi::IRHIBuffer* m_CellHistFromPool[2]  = {nullptr, nullptr};
    std::unique_ptr<rhi::IRHIBuffer> m_ProbeFilterStatsBuf;          // 11 项统计（CPU 可读）
    void* m_ProbeFilterStatsMapped = nullptr;
    bool  m_ProbeFilterBound = false;
    u32   m_ProbeFilterFrame = 0;
    u32   m_ProbeFilterHistIdx = 0;         // 当前作为"历史"的那一份（另一份写）
    bool  m_ProbeFilterHistoryReady = false;
    float4x4 m_ProbePrevViewProj = float4x4(1.0f);   // 上一帧的 viewProj（重投影用）
    u32   m_ProbeFilterMode = 2u;           // 0 直通 / 1 仅空间 / 2 空间+时域（默认；环境变量可覆盖）
    float m_ProbeFilterGamma = 1.0f;        // AABB 的 γ
    float m_ProbeFilterAlpha = 0.9f;        // 时域历史权重 α
    float m_ProbeFilterDistTol = 0.02f;     // 重投影位置容差（占视图深度比例）
    float m_ProbeNoiseIn = 0.0f, m_ProbeNoiseOut = 0.0f;    // l0 亮度的 std/mean（输入/输出）
    float m_ProbeL0MeanIn = 0.0f, m_ProbeL0MeanOut = 0.0f;
    float m_ProbeFrameChange = 0.0f;        // 有历史的探针：|out − hist| / mean
    u32   m_ProbeHistoryUsed = 0;
    // ── 由 SH 采样出逐像素辐照度（步骤 24）──
    rhi::DescriptorSetLayoutHandle m_IrrLayout = 0;
    rhi::DescriptorSetHandle       m_IrrSet    = 0;
    std::unique_ptr<rhi::IRHIPipelineState> m_IrrPSO;
    std::unique_ptr<rhi::IRHIBuffer>        m_CellProbeBuf;
    std::unique_ptr<rhi::IRHITexture>       m_IrradianceTex;
    std::unique_ptr<rhi::IRHIBuffer>        m_IrradianceStatsBuf;   // 0=覆盖像素 1=亮度定点累加 2=亮度定点最大
    void* m_IrrStatsMapped = nullptr;
    bool m_IrrBound = false;
    u32  m_IrrFrame = 0;
    float m_IrradianceMean = 0.0f;
    float m_IrradianceMax = 0.0f;
    u32   m_IrradianceCovered = 0;
    // ── 步骤 26：远场硬件光追 + SDF/三角形对照 ──
    std::unique_ptr<LumenFarFieldPass> m_FarField;
    rhi::IRHIAccelerationStructure* m_TLAS = nullptr;
    rhi::IRHITexture* m_RTMaterialTex = nullptr;
    rhi::IRHITexture* m_RTTriangleNormals = nullptr;
    rhi::IRHIBuffer*  m_RTLightBuffer = nullptr;
    u32               m_RTLightCount = 0;
    rhi::DescriptorSetLayoutHandle m_FarCmpLayout = 0;
    rhi::DescriptorSetHandle       m_FarCmpSet    = 0;
    std::unique_ptr<rhi::IRHIPipelineState> m_FarCmpPSO;
    rhi::DescriptorSetLayoutHandle m_FarMergeLayout = 0;
    rhi::DescriptorSetHandle       m_FarMergeSet    = 0;
    std::unique_ptr<rhi::IRHIPipelineState> m_FarMergePSO;
    std::unique_ptr<rhi::IRHIBuffer> m_RayFadeBuf;      // 步骤 27：每光线的副命中点 + 混合权重
    void* m_RayFadeMapped = nullptr;                    // 诊断：CPU 侧读 fade 缓冲
    std::unique_ptr<rhi::IRHIBuffer> m_FarMergeStatsBuf, m_DistBinBuf;
    void* m_FarMergeStatsMapped = nullptr;
    void* m_DistBinMapped = nullptr;
    std::unique_ptr<rhi::IRHIBuffer> m_FarCmpStatsBuf, m_FarCmpHistBuf;
    void* m_FarCmpStatsMapped = nullptr;
    void* m_FarCmpHistMapped  = nullptr;
    float m_FarFieldThreshold = 50.0f;    // 远场阈值（重叠带的中心）
    float m_FarFieldOverlap   = 0.2f;     // 步骤 27：重叠带半宽（占阈值比例）；0 = 退化成硬切换
    u32   m_FadeSdfOnly = 0, m_FadeRtOnly = 0, m_FadeBlend = 0, m_FadeBlendedRays = 0;
    u32   m_FadeBandRays = 0, m_FadeNoAltRays = 0, m_FadePositiveW = 0;
    u32   m_FadeInvariantViolations = 0;   // 步骤 29：切换分类不变量违例（应恒为 0）
    u32   m_FadeInvNearViol = 0, m_FadeInvFarViol = 0, m_FadeInvBranchViol = 0;
    u32   m_ShadeUnimplementedRays = 0;   // 步骤 28：着色源未实现（返回中性值）的命中光线数
    float m_FadeMaxW = 0.0f;
    u32   m_FadeAltNoCard = 0, m_FadeAltNoPage = 0;
    u32   m_FadeCpuPositive = 0;
    float m_FadeCpuMaxW = -2.0f;
    std::vector<u32> m_DistBinCount, m_DistBinLum;
    float m_FarFieldNearBand  = 50.0f;    // 近带/远带分界（近带里 SDF 是准的 ⇒ 可作 HW RT 的对照）
    float m_FarFieldNear20 = 0.0f, m_FarFieldFar20 = 0.0f;
    u32   m_FarFieldSelfHits = 0;      // SDF 命中里 t < 1 的条数（自交诊断）
    float m_FarFieldSdfMeanT = 0.0f;   // SDF 命中距离均值
    float m_FarFieldNearMeanRel = 0.0f, m_FarFieldFarMeanRel = 0.0f;
    float m_FarFieldTMax      = 500.0f;   // 光追最大距离（比 SDF 的 marchMaxDist 远，才能看"只有三角形命中"）
    u32   m_FarFieldFrame = 0;
    u32   m_FarFieldRays = 0, m_FarFieldBothHit = 0, m_FarFieldSdfOnly = 0, m_FarFieldRtOnly = 0;
    float m_FarFieldMeanRel = 0.0f, m_FarFieldMaxRel = 0.0f;
    float m_FarFieldAgree5 = 0.0f, m_FarFieldAgree20 = 0.0f;
    std::vector<u32> m_FarFieldHist;
    // 输出 pass（把辐照度纹理贴到 Provider 输出）
    rhi::DescriptorSetLayoutHandle m_IrrCopyLayout = 0;
    rhi::DescriptorSetHandle       m_IrrCopySet    = 0;
    std::unique_ptr<rhi::IRHIPipelineState> m_IrrCopyPSO;

    std::vector<u32> m_LastTopPages;
    void CreateFeedbackGPUObjects();
    void CreateProbeGPUObjects();
    void CreateProbeTraceGPUObjects();
    void CreateShadeGPUObjects();
    void CreateSHGPUObjects();
    void CreateProbeFilterGPUObjects();   // 步骤 35：探针滤波
    /// 探针光线 march 的 eps（近层体素倍数）：0.1 ⇒ 约 2.1 世界单位。见 RunProbeTrace 的说明。
    static constexpr float kProbeMarchEpsVoxels = 0.1f;

    /// 确定性验收模式（环境变量 HE_LUMEN_DETERMINISTIC=1）：每帧末把 GPU 等干净，
    /// 使"回读 GPU 计数 → 决定下一帧行为"的时机固定下来，从而让背靠背转储逐位一致。
    bool m_Deterministic = false;
    /// 【步骤 37】是否每帧跑 SDF 调试可视化（`HE_LUMEN_SDF_DISABLE_DEBUG=1` 关闭；默认开启，
    /// 理由见 `RunSDFDebug` 的说明：它只服务工具/转储，但既有验收脚本依赖它的产物）
    bool m_SdfDebugView = true;
    void CreateIrradianceGPUObjects();
    void CreateIrradianceCopyPipeline();
    void CreateIrradianceTexture();
    void CreateFarFieldCompareGPUObjects();
    /// 步骤 29：把 Lumen 自持的显存占用逐条打出来（对照 §15.3 的预算表）
    void LogMemoryUsage() const;
    u32  m_CapturePages = 0;          // 累计捕获页数
    u32  m_CardCaptureMarchHits = 0;  // 诊断：SDF march 命中数
    bool m_CaptureStatsPending = false;
    bool m_CaptureStatsLogged  = false;
};

} // namespace he::render
