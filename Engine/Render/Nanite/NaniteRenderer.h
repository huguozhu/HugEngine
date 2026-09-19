#pragma once

// ============================================================
// Nanite/NaniteRenderer.h — Nanite 模块的唯一门面（生命周期 + 帧图接入）
//
// 【本文件由 §14.8 任务 1 建立骨架，内容由任务 3/4/6 依次填充】
//   任务 1 的职责只有三件，且**不做任何渲染**：
//     ① 持有 `NaniteSettings`（开关/档位的唯一真值，§14.4 的"真值"层，默认 false）；
//     ② 生命周期：`Initialize / Shutdown / Resize`，由 `DeferredPipeline` 转发
//        （照抄 Lumen 的持有与生命周期写法）；
//     ③ 帧图接入点 `AddPasses`：开启且就绪时注册**一个** `Nanite_Noop`
//        （不分配资源、不改任何纹理内容 ⇒ 开启档画面与关闭档逐位相同）。
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
/// 任务 1 只用到 `depth` / `worldPos`（复刻 `GB_Clear` 的那组 WAW 声明）；
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
    void AddPasses(RenderGraph& rg, const NaniteGBufferHandles& gb);

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
