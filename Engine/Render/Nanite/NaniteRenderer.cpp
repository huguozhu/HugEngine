// ============================================================
// Nanite/NaniteRenderer.cpp — 模块门面的实现（任务 1：骨架 + 独立开关 + Nanite_Noop）
//
// 【本文件由 §14.8 任务 1 建立骨架，渲染内容由任务 3/4/6 依次填充】
//   · 任务 1 的 `AddPasses` **只注册一个** `Nanite_Noop`：不分配资源、不录制命令、
//     不改任何纹理内容 —— 开启档的画面与转储必须与关闭档逐位相同。
//   · `reads/writes` 复刻 `GB_Clear` 对 `gbDepth/gbWorldPos` 的那组 WAW 声明：
//     `Shadow` 用 `RG_WRITE(gbDepth)/RG_WRITE(gbWorldPos)` 这条**假 WAW 依赖**
//     把自己定序在 GBuffer 写入者之前（DeferredPipeline_FrameGraph.cpp:213-215）。
//     模块将来接管 GBuffer 写入时必须声明同一组依赖，否则 Shadow/Lighting 的排序会
//     静默变化（§14.5 第一条硬约束）。任务 1 先把这条声明摆好，并证明它不改变任何东西。
//
// 【§14.3 依赖禁令】模块内不得引用 `GI_*` / `Lumen*` / `GPUCulling` 的内部结构
//   （可借其 Hi-Z 纹理句柄与描述符写法）；不得依赖 `MeshBatcher` 的运行时状态
//   —— 它只当**一次性输入**。故本文件不 include 任何 GI/Lumen/GPUCulling 头。
// ============================================================

#include "Nanite/NaniteRenderer.h"

#include "Core/CVar.h"
#include "Core/Log.h"

// CVar: Nanite 独立开关（§14.4 的"配置"层，默认 0）。
// 与 `r.Decal.Project` 同风格（DeferredPipeline_FrameGraph.cpp:40）。它只是**配置载体**：
// 真值在 `NaniteSettings::enabled`，由 cfg 键 `nanite_enable` 与面板共同写入。
static he::CVar<int> cvNaniteEnable("r.Nanite.Enable", 0,
    "Nanite 虚拟几何模块（§14.8 任务 1）：0=关闭（默认，帧图与转储与今天逐位相同），1=开启");

namespace he::render {

bool NaniteRenderer::Initialize(rhi::IRHIDevice* device, u32 width, u32 height) {
    m_Device = device;
    m_Width  = width;
    m_Height = height;

    // 配置层 → 真值：只在启动时读一次 CVar 作为默认值。之后 cfg 与面板都可以覆盖它，
    // 而 CVar 不再每帧回写（否则面板的勾选会被控制台默认值每帧抹掉）。
    m_Settings.enabled = (cvNaniteEnable.Get() != 0);

    // 各段的生命周期桩（任务 1 都只记设备/尺寸；之后各自建自己的资源）
    const bool sceneOk  = m_Scene.Initialize(device, width, height);
    const bool uploadOk = m_Upload.Initialize(device, width, height);
    const bool cullOk   = m_Cull.Initialize(device, width, height);
    const bool rasterOk = m_Raster.Initialize(device, width, height);

    // 任务 1 的"骨架就绪"判据：设备有效且四段都建起来了。
    // 这里**不**依赖任何 GPU 资源的建成，因为任务 1 根本不建资源。
    m_Ready = (m_Device != nullptr) && sceneOk && uploadOk && cullOk && rasterOk;

    HE_CORE_INFO("NaniteRenderer: 骨架初始化完成（ready={}，开关默认={}，档位={}）—— "
                 "任务 1 只注册 Nanite_Noop，不做任何渲染",
                 m_Ready, m_Settings.enabled ? 1 : 0,
                 m_Settings.rasterMode == NaniteRasterMode::Hybrid ? "混合光栅" : "软光栅");
    return m_Ready;
}

void NaniteRenderer::Shutdown() {
    // 四段自持资源（任务 1 为空）必须先于本类清指针之前释放
    m_Raster.Shutdown();
    m_Cull.Shutdown();
    m_Upload.Shutdown();
    m_Scene.Shutdown();

    m_Device = nullptr;
    m_Width  = 0;
    m_Height = 0;
    m_Ready  = false;
    // 【故意不重置 m_Settings】样例在 `DeferredPipeline::Shutdown()` **之后**才回写 cfg，
    // 清掉真值会让 `nanite_enable` 的往返有损（与 gi_half_res 的往返写法保持一致）。
}

void NaniteRenderer::Resize(u32 width, u32 height) {
    m_Width  = width;
    m_Height = height;
    m_Scene.OnResize(width, height);
    m_Upload.OnResize(width, height);
    m_Cull.OnResize(width, height);
    m_Raster.OnResize(width, height);
}

void NaniteRenderer::AddPasses(RenderGraph& rg, const NaniteGBufferHandles& gb) {
    // 门控在调用方（DeferredPipeline_FrameGraph.cpp）已经判过一次；这里再判一次是兜底，
    // 保证"关闭 ⇒ 本模块一个 pass 都不注册"这条不变式不依赖调用方的正确性。
    if (!m_Settings.enabled || !m_Ready) return;

    // ── 任务 1：唯一注册的 pass = `Nanite_Noop` ──
    // reads  = {}                     —— 与 `GB_Clear` 一致（它也不读任何资源）
    // writes = {gbDepth, gbWorldPos}  —— 复刻 `GB_Clear` 对这两个句柄的 WAW 声明
    //   （`GB_Clear` 的写集合里包含这两个；`Shadow` 正是用它们把自己定序在写入者之前）。
    // 空 lambda：不分配资源、不绑定管线、不录制任何命令 ⇒ 纹理内容一位都不变。
    rg.AddPass("Nanite_Noop",
        {},
        {RG_WRITE(gb.depth), RG_WRITE(gb.worldPos)},
        [](rhi::IRHICommandList* /*cmd*/) {
            // 【任务 1 的语义就是"什么都不做"】占位 pass 的唯一作用是：
            //   ① 让"开启档 ⇒ pass 列表出现 Nanite_Noop"这条验收可观测；
            //   ② 把 §14.5 要求的那组 reads/writes 依赖提前摆进帧图，
            //      使任务 4 的软光栅接管 GBuffer 写入时，深度/排序声明已经过验证。
        });
}

} // namespace he::render
