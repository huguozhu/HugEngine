#pragma once

// ============================================================
// GI/LumenProvider.h — Lumen 的 GI Provider
//
// 【它在 GI 架构里的位置】Lumen 是"一个 GI 源"，与 DDGI/SSGI/RTGI 同级：注册进
// `DeferredPipeline::m_GIProviders`，由帧图遍历注册 pass，输出交给 Lighting 的分层合成。
// 它**不是**一条新管线，也不替代 Lighting。
//
// 【为什么按 Provider 而不是拆成多个 Provider】Lumen 内部的多趟（Surface Cache 捕获 →
// SDF 注入 → Screen Probe Gather → Filter）之间存在数据依赖，而 `IGIProvider` 之间没有
// 传递资源的机制（`GIProviderContext` 只是只读快照）——拆开就没有通道把 SDF 交给探针。
// 故：**一个 Provider 对外，内部按 stage 编排**（多级结构照 `RTProvider.h:266` 的
// `std::vector<Stage>` 范式）。
//
// 【骨架阶段（步骤 6）】输出由帧图的 clear 决定：白炉模式清成 1.0（供白炉标度判据），
// 其余清成 0.0（中性，不影响画面）。真实内容由步骤 8~25 依次填入 `Render()`。
// ============================================================

#include "GI/IGIProvider.h"
#include "Lumen/LumenScene.h"
#include "Pipeline/Camera.h"   // 步骤 12：调试视图要用相机基向量与视场角

#include <cmath>

namespace he::render {

class LumenProvider final : public IGIProvider {
public:
    void SetScene(LumenScene* scene) { m_Scene = scene; }
    /// 合并几何来源（MeshBatcher 由 DeferredPipeline 持有；SDF 构建在首帧用到它）
    void SetMeshBatcher(const MeshBatcher* batcher) { m_Batcher = batcher; }

    // ── 身份 ──
    [[nodiscard]] GISourceId GetSourceId() const override { return GISourceId::Lumen; }
    /// 一份估计量同时服务漫反射与镜面（与 IBL 同形）；正因为认领两个通道，
    /// 帧图里必须**按 Provider 而不是按 source id** 注册，否则会被两条循环各跑一遍。
    [[nodiscard]] bool Handles(GISourceId id) const override { return id == GISourceId::Lumen; }
    [[nodiscard]] GIPassKind GetPassKind() const override { return GIPassKind::Offscreen; }
    [[nodiscard]] bool IsValid() const override {
        return m_Scene != nullptr && m_Scene->GetOutput() != nullptr;
    }
    [[nodiscard]] const char* GetName() const override { return "Lumen"; }
    /// 【步骤 35】本帧是否真的产出：由 `SyncToStack` 按层栈写入。
    /// 不能再用 `IsValid()`（它只说明输出纹理在，Lumen 不进任何层栈时也恒真）——
    /// 否则转储会落到 `prov6_*` 这些"上一帧/未使用"的纹理上，信号登记也会报一条没跑的源。
    [[nodiscard]] bool ProducedThisFrame() const override { return m_InStack; }

    // ── 调度 ──
    /// 层栈是唯一真值：Lumen 自己不需要"enabled"开关（与 DDGI 的 SetEnabled 不同，
    /// 它的 pass 门控完全由 NeedsPass(层栈) 决定）。但"本帧是否产出"必须记下来 ——
    /// 它同时决定转储是否有效（见 `ProducedThisFrame`）与信号是否登记。
    void SyncToStack(const GIChannelStack& stack) override {
        m_InStack = stack.Has(GISourceId::Lumen);
    }
    /// 待落地：Screen Probe 的入射辐射度可以用共享的"前帧 HDR 辐射度"（§6 的探针 miss 回退）。
    /// 骨架阶段不消费，故保持 false —— 声明为真会让 `CaptureRadiance` 白捕获一次。
    [[nodiscard]] bool NeedsRadianceHistory() const override { return false; }

    // ── 输出（骨架阶段漫反射与镜面共用一张）──
    [[nodiscard]] rhi::IRHITexture* GetDiffuseOutput()  const override { return Output(); }
    [[nodiscard]] rhi::IRHITexture* GetSpecularOutput() const override { return Output(); }
    [[nodiscard]] rhi::IRHISampler* GetOutputSampler()  const {
        return m_Scene ? m_Scene->GetOutputSampler() : nullptr;
    }

    // ── 生命周期（转调 LumenScene：资源的真正持有者）──
    bool Initialize(rhi::IRHIDevice* /*device*/, u32 /*width*/, u32 /*height*/) override {
        return IsValid();
    }
    void Shutdown() override {
        if (m_Scene) m_Scene->Shutdown();
    }
    void OnResize(u32 width, u32 height) override {
        if (m_Scene) m_Scene->OnResize(width, height);
    }

    /// GBuffer 输入：Screen Probe 生成、Surface Cache Feedback 都要用（步骤 16/20）
    void SetInputs(rhi::IRHITexture* depth, rhi::IRHITexture* normal,
                   rhi::IRHITexture* albedo) override {
        m_Depth  = depth;
        m_Normal = normal;
        m_Albedo = albedo;
    }

    /// 每帧主 pass。**骨架阶段**只画一个常量色全屏三角（白炉 1.0 / 常态 0.0）：
    /// 目的是让"pass 是否注册、输出是否被合成、白炉读数是否 1.0"三件事可独立验证。
    /// 步骤 8 起这里改为按 stage 顺序录制 SurfaceCache_Capture → SDF_Inject → ScreenProbeGather。
    /// 每帧主 pass。**骨架阶段**只画一个常量色全屏三角（白炉 1.0 / 常态 0.0）：
    /// 目的是让"pass 是否注册、输出是否被合成、白炉读数是否 1.0"三件事可独立验证。
    /// 步骤 8 起这里改为按 stage 顺序录制 SurfaceCache_Capture → SDF_Inject → ScreenProbeGather。
    void Render(rhi::IRHICommandList* cmd, const GIProviderContext& ctx) override {
        if (!m_Scene) return;
        // 【步骤 37 诊断】HE_LUMEN_TRACE_OUT=1：每 60 帧打印一次输出 pass 走了哪条分支。
        // 用来区分"辐照度贴图路径"（alpha=+1）与"占位骨架路径"（alpha=-1）——转储里
        // alpha=+1 却 RGB=0 时，只可能是前者采样到了空纹理。
        if (std::getenv("HE_LUMEN_TRACE_OUT")) {
            static u32 s_outFrames = 0;
            if ((++s_outFrames % 60u) == 0u) {
                HE_CORE_INFO("LumenProvider 输出 pass: furnace={} irradianceTex={} → {}",
                             ctx.furnace, (void*)m_Scene->GetIrradianceTexture(),
                             (!ctx.furnace && m_Scene->GetIrradianceTexture()) ? "辐照度贴图" : "占位骨架");
            }
        }
        // 【步骤 24】非白炉：把本帧算出的逐像素辐照度贴到输出上（Provider 输出仍是"本帧真实内容"，
        // Lighting 侧完全不改）。白炉：仍走常量骨架 pass（输出 1.0），把白炉读数与辐照度内容解耦。
        if (!ctx.furnace && m_Scene->GetIrradianceTexture()) {
            m_Scene->DrawIrradiance(cmd, 1.0f);
            return;
        }
        // 注意：SDF 构建**不在这里**做 —— 本函数在 offscreen render pass 内执行，
        // 而 Vulkan 不允许在 render pass 内 dispatch compute（实测直接访问违例崩溃）。
        // 帧图为它单独注册了一个 compute pass（"Lumen_SDF_Build"），见 StepSDF。
        m_Scene->DrawSkeleton(cmd, ctx.furnace ? 1.0f : 0.0f, ctx.furnace ? 1.0f : -1.0f);
    }

    /// 步骤 8：逐 mesh 距离场构建（由帧图的独立 compute pass 调用，见 FrameGraph 的 Lumen 段）。
    /// 相机位置用于让 clipmap 近层跟随相机（第一次调用之前必须给出）。
    void StepSDF(rhi::IRHICommandList* cmd, const CameraData& cam) {
        if (m_Scene && m_Batcher) m_Scene->StepSDF(cmd, *m_Batcher, cam.position);
    }

    /// 步骤 14：页表推进（含 GPU 镜像一致性校验），与 SDF 同一个 compute pass
    void StepSurfaceCache(rhi::IRHICommandList* cmd) {
        if (m_Scene) m_Scene->StepSurfaceCache(cmd);
    }

    /// 步骤 20：Screen Probe 布置（用 GBuffer 的法线与世界坐标，帧图已注入）
    void RunProbePlacement(rhi::IRHICommandList* cmd) {
        if (m_Scene) m_Scene->RunProbePlacement(cmd, m_Normal, m_WorldPos);
    }
    /// 步骤 21：探针半球追踪（在布置之后、同一 compute pass 内）
    void RunProbeTrace(rhi::IRHICommandList* cmd) {
        if (m_Scene) m_Scene->RunProbeTrace(cmd);
    }
    /// 步骤 22：命中点着色 —— 用命中点从 Surface Cache atlas 取材质（不再依赖屏幕 GBuffer），
    /// 并顺带做一次 GBuffer 对照，供 CPU 侧验收"同一几何上材质一致"。
    void RunSurfaceCacheShading(rhi::IRHICommandList* cmd, const CameraData& cam) {
        if (m_Scene) m_Scene->RunSurfaceCacheShading(cmd, m_Albedo, m_WorldPos, cam.GetViewProjMatrix());
    }
    /// 步骤 26：注入 RT 侧输入（TLAS 与场景材质纹理来自 RTPass；光源来自本帧光源缓冲）
    void SetRTInputs(rhi::IRHIAccelerationStructure* tlas, rhi::IRHITexture* materialTex,
                     rhi::IRHITexture* triangleNormals, rhi::IRHIBuffer* lightBuffer, u32 lightCount) {
        if (m_Scene) m_Scene->SetRTInputs(tlas, materialTex, triangleNormals, lightBuffer, lightCount);
    }
    /// 步骤 26：同一条探针光线的远场硬件光追 + 与 SDF 结果的逐光线对照
    void RunFarFieldRT(rhi::IRHICommandList* cmd) {
        if (m_Scene) m_Scene->RunFarFieldRT(cmd);
    }
    [[nodiscard]] bool  IsFarFieldReady() const { return m_Scene && m_Scene->IsFarFieldReady(); }
    [[nodiscard]] u32   GetFarFieldRays() const { return m_Scene ? m_Scene->GetFarFieldRays() : 0u; }
    [[nodiscard]] u32   GetFarFieldBothHit() const { return m_Scene ? m_Scene->GetFarFieldBothHit() : 0u; }
    [[nodiscard]] float GetFarFieldMeanRelDiff() const { return m_Scene ? m_Scene->GetFarFieldMeanRelDiff() : 0.0f; }
    [[nodiscard]] float GetFarFieldMaxRelDiff() const { return m_Scene ? m_Scene->GetFarFieldMaxRelDiff() : 0.0f; }
    [[nodiscard]] float GetFarFieldAgree5Pct() const { return m_Scene ? m_Scene->GetFarFieldAgree5Pct() : 0.0f; }
    [[nodiscard]] float GetFarFieldAgree20Pct() const { return m_Scene ? m_Scene->GetFarFieldAgree20Pct() : 0.0f; }
    [[nodiscard]] u32   GetFarFieldSdfOnly() const { return m_Scene ? m_Scene->GetFarFieldSdfOnly() : 0u; }
    [[nodiscard]] u32   GetFarFieldRtOnly() const { return m_Scene ? m_Scene->GetFarFieldRtOnly() : 0u; }
    [[nodiscard]] const std::vector<u32>& GetFarFieldRelHist() const {
        static const std::vector<u32> kEmpty;
        return m_Scene ? m_Scene->GetFarFieldRelHist() : kEmpty;
    }
    /// 步骤 24：由探针 SH 采样出逐像素入射辐照度（写进屏幕尺寸的辐照度纹理）
    void RunProbeIrradiance(rhi::IRHICommandList* cmd, rhi::IRHITexture* gbNormal,
                            rhi::IRHITexture* gbAlbedo, rhi::IRHITexture* gbWorldPos) {
        if (m_Scene) m_Scene->RunProbeIrradiance(cmd, gbNormal, gbAlbedo, gbWorldPos);
    }
    [[nodiscard]] float GetIrradianceMean() const { return m_Scene ? m_Scene->GetIrradianceMean() : 0.0f; }
    /// 步骤 37：逐像素入射辐照度纹理（转储用：它是 Lumen 输出 pass 的**输入**）
    [[nodiscard]] rhi::IRHITexture* GetIrradianceTexture() const {
        return m_Scene ? m_Scene->GetIrradianceTexture() : nullptr;
    }
    [[nodiscard]] float GetIrradianceMax() const { return m_Scene ? m_Scene->GetIrradianceMax() : 0.0f; }
    [[nodiscard]] u32 GetIrradianceCoveredPixels() const {
        return m_Scene ? m_Scene->GetIrradianceCoveredPixels() : 0u;
    }
    /// 步骤 23：把每条光线结果投成二阶 SH（4 系数 RGB）写回探针；白炉下 l0 必须等于 √π
    void RunScreenProbeSHProject(rhi::IRHICommandList* cmd, bool furnace) {
        if (m_Scene) m_Scene->RunScreenProbeSHProject(cmd, furnace);
    }
    [[nodiscard]] float GetSHMeanL0() const { return m_Scene ? m_Scene->GetSHMeanL0() : 0.0f; }
    [[nodiscard]] float GetSHFurnaceL0Deviation() const { return m_Scene ? m_Scene->GetSHFurnaceL0Deviation() : 0.0f; }
    [[nodiscard]] float GetSHIrradianceMeanDiff() const { return m_Scene ? m_Scene->GetSHIrradianceMeanDiff() : 0.0f; }
    [[nodiscard]] u32 GetSHProbes() const { return m_Scene ? m_Scene->GetSHProbes() : 0u; }
    [[nodiscard]] u32 GetSHRays() const { return m_Scene ? m_Scene->GetSHRays() : 0u; }
    /// 【步骤 37】mesh 距离场的构建预算是否跑完（帧时读数要分开启动期与稳态）
    [[nodiscard]] bool IsMeshBuildComplete() const {
        return m_Scene && m_Scene->IsMeshBuildComplete();
    }
    // ── 步骤 35：探针滤波（空间 3×3 单元 YCoCg AABB + 时域重投影 EMA）──
    /// 必须在 SH 投影之后、逐像素辐照度之前调用（下游读的是过滤后的探针缓冲）
    void RunProbeFilter(rhi::IRHICommandList* cmd, const CameraData& cam) {
        if (m_Scene) m_Scene->RunScreenProbeFilter(cmd, cam.GetViewProjMatrix());
    }
    [[nodiscard]] float GetProbeNoiseIn()  const { return m_Scene ? m_Scene->GetProbeNoiseIn()  : 0.0f; }
    [[nodiscard]] float GetProbeNoiseOut() const { return m_Scene ? m_Scene->GetProbeNoiseOut() : 0.0f; }
    [[nodiscard]] float GetProbeFrameChange() const { return m_Scene ? m_Scene->GetProbeFrameChange() : 0.0f; }
    [[nodiscard]] u32 GetProbeFilterMode() const { return m_Scene ? m_Scene->GetProbeFilterMode() : 0u; }
    // ── 步骤 31（L5）：DDGI（Radiance Cache）要把输入换成我们的 Screen Probe 结果 ──
    /// 【步骤 35 起返回**过滤后**的那一份】DDGI 段注册在 Lumen 段之前 ⇒ 它读到的是上一帧的
    /// 过滤结果（步骤 31 已定的"一帧延迟"语义），正是降噪后的输入。
    [[nodiscard]] rhi::IRHIBuffer* GetProbeBuffer() const {
        if (!m_Scene) return nullptr;
        rhi::IRHIBuffer* filtered = m_Scene->GetProbeFilteredBuffer();
        return filtered ? filtered : m_Scene->GetProbeBuffer();
    }
    /// 未过滤的探针缓冲（诊断对照用）
    [[nodiscard]] rhi::IRHIBuffer* GetRawProbeBuffer() const {
        return m_Scene ? m_Scene->GetProbeBuffer() : nullptr;
    }
    [[nodiscard]] rhi::IRHIBuffer* GetCellProbeBuffer() const {
        return m_Scene ? m_Scene->GetCellProbeBuffer() : nullptr;
    }
    [[nodiscard]] u32 GetScreenCellsX() const { return m_Scene ? m_Scene->GetScreenCellsX() : 0u; }
    [[nodiscard]] u32 GetScreenCellsY() const { return m_Scene ? m_Scene->GetScreenCellsY() : 0u; }
    [[nodiscard]] u32 GetShadedHits() const { return m_Scene ? m_Scene->GetShadedHits() : 0u; }
    [[nodiscard]] u32 GetShadedMissingPages() const { return m_Scene ? m_Scene->GetShadedMissingPages() : 0u; }
    [[nodiscard]] float GetShadedAlbedoMeanDiff() const {
        return m_Scene ? m_Scene->GetShadedAlbedoMeanDiff() : 0.0f;
    }
    [[nodiscard]] u32 GetShadedAlbedoSamples() const {
        return m_Scene ? m_Scene->GetShadedAlbedoSamples() : 0u;
    }
    /// 步骤 16：Feedback 需要 GBuffer 的世界坐标（由帧图注入）
    void SetWorldPosInput(rhi::IRHITexture* worldPos) { m_WorldPos = worldPos; }
    /// 步骤 16：Feedback —— 16×16 分块产出请求列表并写回页表状态
    void RunFeedback(rhi::IRHICommandList* cmd, const CameraData& cam) {
        if (m_Scene) m_Scene->RunFeedback(cmd, m_WorldPos, cam.position);
    }
    /// 步骤 15：Card 捕获（用 GBuffer 当材质源；页状态门控 + 每帧预算）
    void RunCardCapture(rhi::IRHICommandList* cmd, const CameraData& cam) {
        if (!m_Scene) return;
        m_Scene->RunCardCapture(cmd, m_Albedo, m_Normal, m_Depth, cam.GetViewProjMatrix());
    }
    /// 步骤 15 的 atlas（供 GI 转储做可视化验收）
    [[nodiscard]] rhi::IRHITexture* GetCardAtlasAlbedo() const {
        return m_Scene ? m_Scene->GetCardAtlasAlbedo() : nullptr;
    }

    /// 步骤 12：逐像素 SDF 追踪可视化（同一 compute pass 内、SDF 构建之后）。
    /// 相机基向量由这里从 `CameraData` 推出，与 `CameraData::GetViewMatrix()` 用同一套约定
    /// （s = normalize(cross(f, up))、u = cross(s, f)），避免"调试视图左右镜像"这类静默错误。
    void RunSDFDebug(rhi::IRHICommandList* cmd, const CameraData& cam) {
        if (!m_Scene) return;
        const float3 f = glm::normalize(cam.forward);
        const float3 r = glm::normalize(glm::cross(f, cam.up));
        const float3 u = glm::cross(r, f);
        const float  tanHalf = std::tan(glm::radians(cam.fov) * 0.5f);
        m_Scene->RunSDFDebug(cmd, cam.position, f, r, u, tanHalf, cam.aspectRatio);
    }
    /// 步骤 13 的卡片覆盖率可视化（L2 Surface Cache 的输入），供 GI 采样路径转储
    [[nodiscard]] rhi::IRHITexture* GetCardCoverageTexture() const {
        return m_Scene ? m_Scene->GetSDF().GetCardCoverageTexture() : nullptr;
    }
    /// 步骤 12 的可视化产物（供 06.GILab 的 GI 采样路径整幅转储）
    [[nodiscard]] rhi::IRHITexture* GetSDFDebugTexture() const {
        return m_Scene ? m_Scene->GetSDF().GetDebugTexture() : nullptr;
    }

    /// 帧图在 BeginOffscreenPass 之前调用：必须绑定**单颜色附件**的管线，
    /// 否则 RenderPass 会继承上一个 pass（如 GBuffer 的 9 附件）而附件数不匹配。
    void PreBind(rhi::IRHICommandList* cmd) override {
        if (m_Scene) m_Scene->PreBind(cmd);
    }

    // ── 供帧图读取的只读状态（非 IGIProvider 接口）──
    [[nodiscard]] rhi::IRHITexture* GetDebugDepth()  const { return m_Depth; }
    [[nodiscard]] rhi::IRHITexture* GetDebugNormal() const { return m_Normal; }
    [[nodiscard]] rhi::IRHITexture* GetDebugAlbedo() const { return m_Albedo; }

    /// 【步骤 35（§10 的 11.3）】把 Lumen 自己的降噪信号登记进统一框架。
    ///
    /// 它是一条**缓冲类**信号：载体不是一张图，而是探针 SH 缓冲（8 万探针量级）。
    /// 登记的判据是 `ProducedThisFrame()`（层栈）而不是 `IsValid()` —— 后者在 Lumen 没进任何
    /// 层栈时也恒真，会把没跑的源报成"待降噪信号"（这正是步骤 34 在 RT 上抓到的那个假读数）。
    /// 核不是双边上双边滤波，故引导参数位改用 `note` 说明，避免 LogSummary 打出两个假的 sigma。
    void DescribeSignals(DenoiseSignalRegistry& registry, rhi::IRHITexture* depth,
                         rhi::IRHITexture* normal, rhi::IRHITexture* /*velocity*/) override {
        if (!ProducedThisFrame() || !m_Scene) return;
        rhi::IRHIBuffer* in  = m_Scene->GetProbeBuffer();
        rhi::IRHIBuffer* out = m_Scene->GetProbeFilteredBuffer();
        if (!in || !out) return;
        const u32 probes = m_Scene->GetProbeCount();
        DenoiseSignal s;
        s.name         = "Lumen_ScreenProbe";
        s.inputBuffer  = in;
        s.outputBuffer = out;
        s.depth        = depth;    // 引导：重投影要用 GBuffer 的法线/深度做一致性校验
        s.normal       = normal;
        s.width        = probes;   // 缓冲信号：分辨率 = 元素数 × 1
        s.height       = 1u;
        s.targetWidth  = probes;
        s.targetHeight = 1u;
        s.needsUpscale = false;    // 探针是"每 16×16 像素一个"的稀疏载体，由逐像素辐照度级重建
        s.note         = m_Scene->GetProbeFilterNote();
        registry.Register(s);
    }

private:
    [[nodiscard]] rhi::IRHITexture* Output() const {
        return m_Scene ? m_Scene->GetOutput() : nullptr;
    }

    LumenScene* m_Scene = nullptr;   // 非拥有：由 DeferredPipeline 持有
    const MeshBatcher* m_Batcher = nullptr;   // 非拥有：合并几何（SDF 构建输入）
    bool m_InStack = false;          // 本帧层栈是否要求了 Lumen（由 SyncToStack 写入）
    // GBuffer 输入（非拥有，帧图每帧注入）
    rhi::IRHITexture* m_Depth  = nullptr;
    rhi::IRHITexture* m_Normal = nullptr;
    rhi::IRHITexture* m_Albedo = nullptr;
    rhi::IRHITexture* m_WorldPos = nullptr;   // 步骤 16：Feedback 的世界坐标输入
};

} // namespace he::render
