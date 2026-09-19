#pragma once

// ============================================================
// Nanite/NaniteRenderer.h — Nanite 模块的唯一门面（生命周期 + 帧图接入）
//
// 【本文件由 §14.8 任务 1 建立骨架，内容由任务 3/4/6 依次填充】
//   任务 1：骨架 + 独立开关（唯一注册的 pass 是 `Nanite_Noop`）。
//   任务 3：把占位 pass 换成「计数 → 间接绘制」链的两个 pass：
//     · `Nanite_Cull`   —— compute 写间接命令 + 计数（`NaniteCull`）
//     · `Nanite_Raster` —— `DrawIndexedIndirectCount` 消费计数，写模块自建的 1×1 R8 目标
//   `Nanite_Noop` 同时被移除（它的历史作用只是验证门控）。
//
// 【§14.3 依赖禁令】模块内不得引用 `GI_*` / `Lumen*` / `GPUCulling` 的内部结构
//   （可借其 Hi-Z 纹理句柄与描述符写法）；不得依赖 `MeshBatcher` 的运行时状态
//   —— 它只当**一次性输入**。本类只依赖 `RenderGraph` 与 RHI 的公开接口，
//   不 include 任何 GI/Lumen/GPUCulling 头。
//
// 【公共面】§14.3：模块公共面只有三个 —— 本类、`NaniteSettings`、`NaniteTypes.h` 的 POD。
//   `NaniteScene / NaniteUpload / NaniteCull / NaniteRaster` 是模块内部实现，
//   外部（含 `DeferredPipeline`）只通过本类使用它们。
// ============================================================

#include "Nanite/NaniteCull.h"
#include "Nanite/NaniteRaster.h"
#include "Nanite/NaniteScene.h"
#include "Nanite/NaniteSettings.h"
#include "Nanite/NaniteUpload.h"

#include "RHI/RHI.h"
#include "RenderGraph.h"

namespace he::render {

/// 帧图接入所需的 GBuffer 句柄组（与 `GBufferRenderer::Handles` 同构）
///
/// 任务 3 只用到 `depth` / `worldPos`（复刻 `GB_Clear` 的那组 WAW 声明，§14.5 第一条硬约束）；
/// 其余字段为任务 4/5 的光栅与材质解析预留 —— 届时模块**自己**建 PSO/附件布局、
/// 直接写这些既有句柄，从而不需要给 `GBufferRenderer` 加 `Mode::Nanite`（§14.5）。
struct NaniteGBufferHandles {
    ResourceHandle albedo      = kInvalidHandle;
    ResourceHandle normal      = kInvalidHandle;
    ResourceHandle emissive    = kInvalidHandle;
    ResourceHandle velocity    = kInvalidHandle;
    ResourceHandle worldPos    = kInvalidHandle;
    ResourceHandle disneyA     = kInvalidHandle;
    ResourceHandle disneyB     = kInvalidHandle;
    ResourceHandle lightmapKey = kInvalidHandle;
    ResourceHandle depth       = kInvalidHandle;

    /// 【任务 4 新增】albedo 的**纹理对象**本身（不只是帧图句柄）。
    /// 任务 4 的 `Nanite_TestWrite` 要把它绑成存储图像（UAV）并从它取真实分辨率，
    /// 这两件事都只有 `IRHITexture*` 能做；句柄只够帧图排序。
    /// 【生命周期】纹理归 `GBufferRenderer` 所有；模块只在一个 pass 内借用（不持有）。
    /// 前一处挂钩（`AddPasses`）不用它，故默认 nullptr。
    rhi::IRHITexture* albedoTexture = nullptr;
};

/// Nanite 模块门面：资源生命周期 + 帧图接入 + 耗时读数/诊断（后两者是后续任务）
class NaniteRenderer {
public:
    NaniteRenderer() = default;
    ~NaniteRenderer() = default;

    NaniteRenderer(const NaniteRenderer&) = delete;
    NaniteRenderer& operator=(const NaniteRenderer&) = delete;

    /// 建立骨架并读入配置层的启动默认值。
    /// 【说明】CVar `r.Nanite.Enable` 只在这里读一次，作为**启动默认**；
    /// 运行期真值是 `m_Settings`（cfg 与面板都写它），三层不会互相覆盖。
    bool Initialize(rhi::IRHIDevice* device, u32 width, u32 height);

    /// 释放模块自持资源。**不重置开关真值** —— 样例的配置回写发生在
    /// `DeferredPipeline::Shutdown()` 之后，清掉真值会让 cfg 往返丢键。
    void Shutdown();

    /// 视口变化（GBuffer 尺寸变了，软光栅/剔除的目标随之变化）
    void Resize(u32 width, u32 height);

    /// 骨架就绪（任务 1：`Initialize` 成功即 true）。
    /// 帧图门控要用它：若这里恒为 false，`nanite_enable=1` 时不会出现 `Nanite_Noop`。
    [[nodiscard]] bool IsReady() const { return m_Ready; }

    // ── 开档与档位（§14.4：唯一真值在本类，外部只拿 const 引用读）──
    [[nodiscard]] const NaniteSettings& GetSettings() const { return m_Settings; }
    void SetSettings(const NaniteSettings& settings) { m_Settings = settings; }

    /// 帧图接入点（在 `DeferredPipeline_FrameGraph.cpp` 的 GBuffer 段被调用）。
    /// 【门控只有一处】`DeferredPipeline_FrameGraph.cpp` 里的
    /// `if (m_Nanite.GetSettings().enabled && m_Nanite.IsReady())`。
    /// 开启时注册两个 pass：`Nanite_Cull` + `Nanite_Raster`（原序 12 个 pass 一个不动）。
    void AddPasses(RenderGraph& rg, const NaniteGBufferHandles& gb);

    /// 【§14.8 任务 4：GBuffer 之后的后置挂钩】在 **GBuffer 几何段结束之后、任何读取 GBuffer
    /// 的 pass（SSAO/Lighting 等）之前**调用第二处注册点：
    ///   · 为什么必须有这一处：`AddPasses` 的位置在既有 `GB_Clear` **之前**，模块的写入会被
    ///     `GB_Clear` 覆盖，无法被同帧的 Lighting 读到；任务 4 的验收（"compute 写 GBuffer 且
    ///     同帧被 Lighting 读到"）只能在 GBuffer 之后注册才能成立。
    ///   · 开关守卫与 `AddPasses` 是**同一个真值**（`NaniteSettings::enabled` + `IsReady()`），
    ///     不是新门控；`nanite_test_write` 只是模块内部的第二个条件。
    ///   · 关闭档 / 未就绪 / `testWrite=false` ⇒ 一个 pass 都不注册（§14.2 不变式 1）。
    ///   · 注意：这里写的是 `gbAlbedo` 的 UAV，因此**不能**声明 `gbDepth/gbWorldPos` 的那组
    ///     WAW —— `GB_Clear` 已经在前面写过它们，再声明只会多出一条无意义的依赖。
    ///
    /// 【将来】任务 26 的调试可视化落点也在这里（GBuffer 之后的可视化叠加）。
    void AddPostGBufferPasses(RenderGraph& rg, const NaniteGBufferHandles& gb);

    /// dump 帧（`HE_DUMP_GI_FRAME`）打印**恰好一行**真实 GPU 读回：
    ///   `[Nanite] fake_clusters=<N> count_buffer=<X> indirect_cmds=<Y> rasterized_clusters=<Z>`
    ///
    /// X = 计数缓冲的值（GPU 原子累加的命令条数）；Y = 间接命令缓冲里**实际被写过**的
    /// 命令条数（读回时按"非哨兵且字段合法"统计）；Z = 绘制端片元原子计数的值。
    /// 任务 3 的验收就是 X == Y == Z == N。
    ///
    /// 【同步约定】本函数**只做 Map 读回，不做任何等待** —— 调用方必须在 GPU 完成后调用
    /// （样例的 dump 路径已经有 `device->WaitIdle()`，照抄既有白炉探针/落盘的读数方式）。
    /// 关闭档下直接返回（不打印），保证关闭档日志与基线一致。
    void LogFakePipelineReadback();

    // ── 模块内部各段（任务 1 只有生命周期桩；外部不得越过本类直接驱动它们）──
    [[nodiscard]] NaniteScene&  GetScene()  { return m_Scene; }
    [[nodiscard]] NaniteUpload& GetUpload() { return m_Upload; }
    [[nodiscard]] NaniteCull&   GetCull()   { return m_Cull; }
    [[nodiscard]] NaniteRaster& GetRaster() { return m_Raster; }

    [[nodiscard]] u32 GetWidth()  const { return m_Width; }
    [[nodiscard]] u32 GetHeight() const { return m_Height; }

private:
    rhi::IRHIDevice* m_Device = nullptr;
    u32 m_Width  = 0;
    u32 m_Height = 0;
    bool m_Ready = false;   ///< "骨架就绪"（任务 1 的语义，见 IsReady 的说明）

    /// 开关与档位的唯一真值（默认 `enabled = false` ⇒ §14.2 不变式 1）
    NaniteSettings m_Settings;

    // 模块内部各段（§14.3 的六个文件各司其职，本类只做门面与生命周期转发）
    NaniteScene  m_Scene;
    NaniteUpload m_Upload;
    NaniteCull   m_Cull;
    NaniteRaster m_Raster;
};

} // namespace he::render
