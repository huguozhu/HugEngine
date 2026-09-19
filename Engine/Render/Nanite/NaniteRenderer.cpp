// ============================================================
// Nanite/NaniteRenderer.cpp — 模块门面的实现
//   §14.8 任务 1：骨架 + 独立开关 + `Nanite_Noop` 占位 pass
//   §14.8 任务 3：占位 pass 换成「计数 → 间接绘制」链的 `Nanite_Cull` + `Nanite_Raster`
//
// 【开启档的 pass 集合】既有 12 个 pass（相对顺序不变）+ `Nanite_Cull` + `Nanite_Raster`。
//   两个 pass 的 `reads/writes` 复刻 `GB_Clear` 对 `gbDepth/gbWorldPos` 的那组 WAW 声明：
//   `Shadow` 用 `RG_WRITE(gbDepth)/RG_WRITE(gbWorldPos)` 这条**假 WAW 依赖**
//   把自己定序在 GBuffer 写入者之前（DeferredPipeline_FrameGraph.cpp:213-215）。
//   模块将来接管 GBuffer 写入时必须声明同一组依赖，否则 Shadow/Lighting 的排序会
//   静默变化（§14.5 第一条硬约束）。
//
// 【可见画面零影响】`Nanite_Raster` 渲染到模块自建的 1×1 R8 小目标（不是 GBuffer 附件），
//   `Nanite_Cull` 只写模块自持缓冲 ⇒ 开启档转储与关闭档逐位相同。
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
    "Nanite 虚拟几何模块（§14.8 任务 3）：0=关闭（默认，帧图与转储与今天逐位相同），1=开启");

// CVar: 任务 3 的假簇数量（默认 6）。与 cfg 键 `nanite_fake_clusters` 同含义，
// 只作为**启动默认**；运行期真值在 `NaniteSettings::fakeClusters`。
static he::CVar<int> cvNaniteFakeClusters("r.Nanite.FakeClusters", 6,
    "Nanite 任务 3 假簇数量（1 个实例、N 个簇；验证'计数为 k ⇒ 恰好画 k 次'）");

namespace he::render {

bool NaniteRenderer::Initialize(rhi::IRHIDevice* device, u32 width, u32 height) {
    m_Device = device;
    m_Width  = width;
    m_Height = height;

    // 配置层 → 真值：只在启动时读一次 CVar 作为默认值。之后 cfg 与面板都可以覆盖它，
    // 而 CVar 不再每帧回写（否则面板的勾选会被控制台默认值每帧抹掉）。
    m_Settings.enabled = (cvNaniteEnable.Get() != 0);
    if (cvNaniteFakeClusters.Get() > 0)
        m_Settings.fakeClusters = (u32)cvNaniteFakeClusters.Get();

    // 各段生命周期。`NaniteCull` 必须先建（它持有"已光栅化簇计数缓冲"，绘制端要引用它）。
    const bool sceneOk  = m_Scene.Initialize(device, width, height);
    const bool uploadOk = m_Upload.Initialize(device, width, height);
    const bool cullOk   = m_Cull.Initialize(device, width, height);
    const bool rasterOk = m_Raster.Initialize(device, width, height,
                                              m_Cull.GetRasterCountBuffer());

    // "骨架就绪"判据：设备有效且四段都建起来了。任务 3 起剔除/绘制段还要求
    // 自持缓冲与 PSO 真正建成（IsReady），否则开启档不会注册任何 pass。
    m_Ready = (m_Device != nullptr) && sceneOk && uploadOk
              && m_Cull.IsReady() && m_Raster.IsReady();

    HE_CORE_INFO("NaniteRenderer: 初始化完成（ready={}，开关默认={}，假簇数={}，档位={}）—— "
                 "任务 3 注册 Nanite_Cull + Nanite_Raster（计数→间接绘制链）",
                 m_Ready, m_Settings.enabled ? 1 : 0, m_Settings.fakeClusters,
                 m_Settings.rasterMode == NaniteRasterMode::Hybrid ? "混合光栅" : "软光栅");
    return m_Ready;
}

void NaniteRenderer::Shutdown() {
    // 四段自持资源必须先于本类清指针之前释放（绘制端引用了剔除段的计数缓冲，
    // 故先释放绘制端再释放剔除段）
    m_Raster.Shutdown();
    m_Cull.Shutdown();
    m_Upload.Shutdown();
    m_Scene.Shutdown();

    m_Device = nullptr;
    m_Width  = 0;
    m_Height = 0;
    m_Ready  = false;
    // 【故意不重置 m_Settings】样例在 `DeferredPipeline::Shutdown()` **之后**才回写 cfg，
    // 清掉真值会让 `nanite_enable` / `nanite_fake_clusters` 的往返有损。
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

    // 唯一真值 → 模块内部：把本帧的假簇数量交给剔除段（cfg/面板只写 NaniteSettings）
    m_Cull.SetFakeClusterCount(m_Settings.fakeClusters);

    // ── Nanite_Cull：compute 逐簇写间接命令 + 原子累加计数 ──
    // writes = {gbDepth, gbWorldPos} 是复刻 `GB_Clear` 的 WAW 声明（§14.5），
    // 本 pass 并不真的写它们；它真正的输出是模块自持的命令/计数缓冲（不走帧图资源）。
    rg.AddPass("Nanite_Cull",
        {},
        {RG_WRITE(gb.depth), RG_WRITE(gb.worldPos)},
        [this](rhi::IRHICommandList* cmd) {
            m_Cull.RecordCullPass(cmd);
        });

    // ── Nanite_Raster：DrawIndexedIndirectCount 消费计数，写模块自建的 1×1 R8 目标 ──
    // 与 Nanite_Cull 的 WAW（同一组句柄）保证它排在 Cull 之后执行。
    rg.AddPass("Nanite_Raster",
        {},
        {RG_WRITE(gb.depth), RG_WRITE(gb.worldPos)},
        [this](rhi::IRHICommandList* cmd) {
            m_Raster.RecordRasterPass(cmd,
                                      m_Cull.GetIndirectCmdBuffer(),
                                      m_Cull.GetCountBuffer(),
                                      m_Cull.GetMaxFakeClusters());
        });
}

void NaniteRenderer::LogFakePipelineReadback() {
    // 关闭档（或未就绪）不打印：关闭档的日志与转储必须与基线逐位一致。
    if (!m_Settings.enabled || !m_Ready) return;

    // X = 计数缓冲的值（GPU 原子累加"实际写入的命令条数"）
    u32 x = 0;
    if (auto* b = m_Cull.GetCountBuffer()) {
        if (void* p = b->Map()) { x = *static_cast<const u32*>(p); b->Unmap(); }
    }

    // Y = 间接命令缓冲里真正被写过的命令条数：CPU 每帧把整块填成 0xFFFFFFFF 哨兵，
    // 因此"字段合法的条目数"就是 GPU 实际写过的条数（与 X 互为独立证据）。
    u32 y = 0;
    if (auto* b = m_Cull.GetIndirectCmdBuffer()) {
        if (void* p = b->Map()) {
            const auto* cmds = static_cast<const NaniteIndirectCommand*>(p);
            for (u32 i = 0; i < kNaniteMaxFakeClusters; ++i) {
                if (cmds[i].indexCount == kNaniteFakeClusterIndexCount &&
                    cmds[i].instanceCount == 1u) {
                    ++y;
                }
            }
            b->Unmap();
        }
    }

    // Z = 绘制端片元原子计数（1×1 目标 ⇒ 每被光栅化一个簇恰好加一）
    u32 z = 0;
    if (auto* b = m_Cull.GetRasterCountBuffer()) {
        if (void* p = b->Map()) { z = *static_cast<const u32*>(p); b->Unmap(); }
    }

    // 【恰好一行】任务 3 的验收出口：X == Y == Z == N
    HE_CORE_INFO("[Nanite] fake_clusters={} count_buffer={} indirect_cmds={} rasterized_clusters={}",
                 m_Settings.fakeClusters, x, y, z);
}

} // namespace he::render
