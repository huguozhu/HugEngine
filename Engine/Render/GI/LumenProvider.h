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

    // ── 调度 ──
    /// 层栈是唯一真值：Lumen 自己不需要"enabled"开关（与 DDGI 的 SetEnabled 不同，
    /// 它的 pass 门控完全由 NeedsPass(层栈) 决定）
    void SyncToStack(const GIChannelStack& /*stack*/) override {}

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

private:
    [[nodiscard]] rhi::IRHITexture* Output() const {
        return m_Scene ? m_Scene->GetOutput() : nullptr;
    }

    LumenScene* m_Scene = nullptr;   // 非拥有：由 DeferredPipeline 持有
    const MeshBatcher* m_Batcher = nullptr;   // 非拥有：合并几何（SDF 构建输入）
    // GBuffer 输入（非拥有，帧图每帧注入）
    rhi::IRHITexture* m_Depth  = nullptr;
    rhi::IRHITexture* m_Normal = nullptr;
    rhi::IRHITexture* m_Albedo = nullptr;
};

} // namespace he::render
