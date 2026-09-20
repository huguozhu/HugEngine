// ============================================================
// Nanite/NaniteRenderer.cpp — 模块门面的实现
//   §14.8 任务 1：骨架 + 独立开关 + `Nanite_Noop` 占位 pass
//   §14.8 任务 3：占位 pass 换成「计数 → 间接绘制」链的 `Nanite_Cull` + `Nanite_Raster`
//
// 【开启档的 pass 集合】既有 12 个 pass（相对顺序不变）+ `Nanite_Cull` + `Nanite_Raster`。
//   任务 4 追加一个**可选**的第三个 pass `Nanite_TestWrite`（仅在 `NaniteSettings::testWrite`
//   为真时注册；它写既有 GBuffer albedo 的 UAV，必须在 GBuffer 之后注册才能被 Lighting 同帧读到）。
//   任务 6 追加另一个**可选**的第四个 pass `Nanite_MeshTest`（仅在 `NaniteSettings::meshTest`
//   为真且设备支持 `VK_EXT_mesh_shader` 时注册；它只画模块自建的 1×1 R8 目标 + 写模块自持计数缓冲，
//   因此与 GBuffer 无关，注册在第一处挂钩即可）。
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

    // ── 任务 5：一次性启动日志 —— 打印 objectIndex 分区表与本次容量，便于人工核对 ──
    // 只打一行、只在启动时打（不是每帧），因此关闭档的每帧开销与日志都与基线一致。
    HE_CORE_INFO("NaniteRenderer: objectIndex 分区 = 普通段[{}, {}) 容量 {} | Nanite 段[{}, {}) 容量 {} | "
                 "哨兵 {} | MRT7(gb_lightmapkey) 页号 float16 精确上限 {}",
                 kNormalObjectIndexBegin, kNaniteObjectIndexBegin, kNormalObjectIndexCapacity,
                 kNaniteObjectIndexBegin, kObjectIndexTotalCapacity, kNaniteObjectIndexCapacity,
                 kInvalidObjectIndex, kLightmapKeyExactObjectIndexLimit);
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

    // ── §14.8 任务 6：Nanite_MeshTest（最小 mesh PSO 通道）──
    // 【为什么注册在**这一处**（GBuffer 之前的第一处挂钩），而不是 GBuffer 之后那一处】
    //   1. 本 pass **完全不碰 GBuffer**：它画进模块自建的 1×1 R8 小目标、写模块自持的计数缓冲，
    //      因此对 GBuffer 没有任何读写依赖 —— 第二处挂钩（`AddPostGBufferPasses`）的语义是
    //      "必须在 GBuffer 几何段之后、Lighting 之前注册的那些 pass"（任务 4 的 UAV 自证），
    //      把不碰 GBuffer 的 pass 放进去只会让那处挂钩的语义变模糊。
    //   2. 三处模块私有 pass（Cull / Raster / MeshTest）集中在同一个注册点，便于对照与回退。
    // 【为什么不声明 gbDepth/gbWorldPos 的那组假 WAW】那组声明是 `GB_Clear` 写入者之间的排序
    //   契约（§14.5）。本 pass 没有任何帧图资源，声明空 reads/writes 反而是**最保守**的选择：
    //   `RenderGraph::CullDeadPasses` 明确不裁剪 `writes.empty()` 的 pass，而
    //   `TopologicalSort` 把它放在所有 inDegree=0 的 pass 之前 —— 也就是说它既不改变任何既有
    //   pass 之间的相对顺序，也不会被裁剪（既有 pass 顺序逐位不变）。
    // 【相对顺序】它注册在 `Nanite_Cull`/`Nanite_Raster` 之后，既有 12 个 pass 的注册位置不变。
    // 【门控】`meshTest`（默认 0）**且**设备支持 `VK_EXT_mesh_shader`；两者任一不满足就
    //   一个 pass 都不注册（关闭档的 pass 集合与今天逐位一致）。
    if (m_Settings.meshTest && m_Raster.IsMeshTestSupported()) {
        rg.AddPass("Nanite_MeshTest",
            {},
            {},
            [this](rhi::IRHICommandList* cmd) {
                m_Raster.RecordMeshTestPass(cmd);
            });
    }
}

void NaniteRenderer::AddPostGBufferPasses(RenderGraph& rg, const NaniteGBufferHandles& gb) {
    // 门控在调用方（DeferredPipeline_FrameGraph.cpp）已经判过一次；这里再判一次是兜底，
    // 保证"关闭 ⇒ 本模块一个 pass 都不注册"这条不变式不依赖调用方的正确性。
    // 【与 AddPasses 是同一个真值】不是新门控：`enabled`（独立开关）+ `IsReady()`（模块就绪）。
    if (!m_Settings.enabled || !m_Ready) return;

    // 任务 4 的 UAV 自证通道：默认关闭（`nanite_test_write=0`）⇒ 这里什么都不注册。
    if (!m_Settings.testWrite) return;

    // 需要 albedo 的帧图句柄（排序 + 读写状态）与纹理对象（UAV 绑定 + 分辨率）两者都在。
    if (gb.albedo == kInvalidHandle || !gb.albedoTexture) return;

    rhi::IRHITexture* albedo = gb.albedoTexture;   // 按值捕获：帧图执行发生在 BuildFrameGraph 返回之后

    // ── Nanite_TestWrite：compute 用 RWTexture2D 直接写既有 GBuffer albedo ──
    // 【writes 用 UAV 而不是 Write】`ResourceAccess::Write` 映射到 `RenderTarget`（颜色附件布局），
    // 而本 pass 是**存储图像写入**，必须映射到 `UnorderedAccess`（VK_IMAGE_LAYOUT_GENERAL）——
    // 见 RenderGraph::AccessToState。这样帧图会插入 `颜色附件 → GENERAL`（本 pass 前）与
    // `GENERAL → ShaderResource`（Lighting 读 albedo 前）两条转换，这正是"同帧被 Lighting 读到"
    // 所依赖的排序；pass 内部还会补两条 ComputeShader 阶段的显式屏障（见 NaniteRaster）。
    // 【顺序约束】与 `GB_Clear` 同为 gbAlbedo 的写入者 ⇒ 帧图的 WAW 依赖天然把本 pass 排在
    // `GB_Clear` 之后（也正是"模块的写入不会被 GB_Clear 覆盖"的保证）。
    rg.AddPass("Nanite_TestWrite",
        {},
        {{gb.albedo, ResourceAccess::UAV}},
        [this, albedo](rhi::IRHICommandList* cmd) {
            m_Raster.RecordTestWritePass(cmd, albedo);
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

void NaniteRenderer::LogMeshTestReadback() {
    // 关闭档 / 未就绪 / 未开 mesh 自证：不打印（关闭档与"只开 enabled"的日志必须与基线一致）。
    if (!m_Settings.enabled || !m_Ready) return;
    if (!m_Settings.meshTest) return;

    // 两个数都是**真实 GPU 读回**（样例在 dump 帧已 `WaitIdle()`，与白炉探针同一套同步做法）：
    //   n = 片元原子计数（被光栅化的 mesh 图元数），v = 1×1 R8 目标的像素值。
    const u32 n = m_Raster.ReadbackMeshTestOutputs();
    const u32 v = m_Raster.ReadbackMeshTestTargetMax();

    // 【恰好一行】任务 6 的验收出口：mesh_pso=ok 且 n >= 1 / v > 0 即"输出非空"。
    HE_CORE_INFO("[Nanite] mesh_pso={} meshlet_outputs={} target_max={}",
                 m_Raster.IsMeshTestPSOReady() ? "ok" : "fail", n, v);
}

} // namespace he::render
