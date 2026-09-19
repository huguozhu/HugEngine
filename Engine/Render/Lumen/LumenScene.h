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
    static constexpr u32 kMaxCapturesPerFrame = 8;   // 步骤 17 的预算之一（先在这里落地）
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
    u32  m_CapturePages = 0;          // 累计捕获页数
    u32  m_CardCaptureMarchHits = 0;  // 诊断：SDF march 命中数
    bool m_CaptureStatsPending = false;
    bool m_CaptureStatsLogged  = false;
};

} // namespace he::render
