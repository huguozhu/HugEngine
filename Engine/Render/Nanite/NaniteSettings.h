#pragma once

// ============================================================
// Nanite/NaniteSettings.h — Nanite 的开关与档位（§14.4 三层里的"真值"层）
//
// 【本文件由 §14.8 任务 1 建立骨架；任务 3 加了假簇数量，任务 4 加了 UAV 自证开关】
//
// 【唯一真值】§14.4：`NaniteSettings::enabled` 是**唯一真值**，由 `NaniteRenderer` 持有。
//   另外两层只是配置载体，不得各自缓存一份状态：
//     · 配置层：CVar `r.Nanite.Enable`（默认 0）+ cfg 键 `nanite_enable`（默认 0）；
//     · 面板层：样例 07.Nanite 的 ImGui 勾选框 + 档位下拉（改动即写回本结构）。
//   三层之间的优先级（任务 1 的默认选择）：CVar = 启动默认 → cfg = 启动覆盖 →
//   面板 = 运行期真值。**默认 false** 是 §14.2 不变式 1 的前提：
//   关闭 ⇒ 帧图与转储逐位相同、且不产生任何新的每帧 CPU 开销。
//
// 【§14.3 依赖禁令】模块内不得引用 `GI_*` / `Lumen*` / `GPUCulling` 的内部结构
//   （可借其 Hi-Z 纹理句柄与描述符写法）；不得依赖 `MeshBatcher` 的运行时状态
//   —— 它只当**一次性输入**。本头文件同样 RHI-free，可被样例/Scene 侧 include。
// ============================================================

#include "Nanite/NaniteTypes.h"

namespace he::render {

/// Nanite 的开关与档位（唯一真值由 `NaniteRenderer` 持有，外部只拿 const 引用读）
struct NaniteSettings {
    /// 独立开关。默认 false：关闭时模块一个 pass 都不注册，帧图与今天逐位相同。
    bool enabled = false;

    /// 光栅档位（§14.4 面板下拉）。任务 1 只是枚举占位，没有消费者：
    /// 软光栅/混合光栅的实现分别在任务 4 与任务 6/22。
    NaniteRasterMode rasterMode = NaniteRasterMode::Soft;

    /// 任务 3 的假簇数量（§14.8 任务 3 的验收输入：1 个实例、N 个簇）。
    /// 配置层 = cfg 键 `nanite_fake_clusters`（默认 6），样例负责解析/序列化；
    /// 模块把它交给 `NaniteCull`（超上限时钳制到 `kNaniteMaxFakeClusters`）。
    /// 它只在任务 3 的验证链路上有意义，任务 8 起被真实簇划分取代。
    u32 fakeClusters = 6;

    /// 【§14.8 任务 4】UAV 自证开关（默认 **false**）。
    /// 开启后模块在 GBuffer 几何段**之后**追加一个 `Nanite_TestWrite` compute pass：
    /// 用 `RWTexture2D<float4>` 往既有 GBuffer albedo 写 8×8 棋盘（A1 路线的证据），
    /// 供"compute 写 GBuffer 且同帧被 Lighting 读到"这条验收使用。
    /// 配置层 = cfg 键 `nanite_test_write`（默认 0），样例负责解析/序列化 + 面板勾选框。
    /// 【默认 false 的理由】§14.2 不变式 1：关闭时模块一个 pass 都不注册，画面与今天逐位相同；
    /// 它只改 albedo 的**内容**（不改任何既有 pass 的声明与顺序），是纯粹的自证开关。
    bool testWrite = false;

    /// 【§14.8 任务 6】mesh PSO 自证开关（默认 **false**）。
    /// 开启后模块注册一个 `Nanite_MeshTest` pass：用 `PipelineStateDesc::meshShader` 建一条
    /// **最小 mesh 管线**（`Nanite_MeshTest.mesh.slang` 真正输出 4 顶点 / 2 图元），画进
    /// 模块自建的 1×1 R8 小目标，并读回两个 GPU 数值证明"确实输出了非空图元"。
    /// 配置层 = cfg 键 `nanite_mesh_test`（默认 0），样例负责解析/序列化 + 面板勾选框。
    /// 【默认 false 的理由】§14.2 不变式 1：关闭时该 pass **完全不注册**，
    /// 开启档 / 关闭档的 pass 集合与转储逐位不变；它写的是模块私有目标与私有缓冲，
    /// 即使打开也不会改动可见画面（这正是任务 6 与任务 22 的边界：任务 22 才接硬光栅分流）。
    bool meshTest = false;

    /// 【§14.8 任务 13】合成实例网格的条数（默认 `kNaniteDefaultTestInstances` = 64）。
    /// 实例剔除的验收要求"与 CPU 参考逐项一致"，而真实场景的实例表要到任务 14+ 才接得上，
    /// 所以本任务用一张**合成实例网格**做可比对的实例集（来源与坐标系见 `NaniteCull.cpp`）。
    /// 配置层 = cfg 键 `nanite_instance_test_count`（默认 64），样例负责解析/序列化。
    /// 模块把它钳制到 `kNaniteMaxTestInstances`（256）。
    /// 【0 的含义】不生成任何合成实例 ⇒ 该 pass 派发 0 个线程、可见数恒 0（仍照常注册）。
    u32 instanceTestCount = kNaniteDefaultTestInstances;

    /// 【§14.8 任务 15】Phase 2 的 **Hi-Z 遮挡剔除开关**（默认 **false**）。
    ///
    /// 【语义】开启后，簇球（世界 AABB）投影到屏幕空间后与既有 Hi-Z 金字塔做遮挡测试
    ///   （4 角最小深度比较，层数下限 1：金字塔 mip0 从未被写入）；关闭时**退化为"不遮挡"**
    ///   （判据的第一句就返回 false，`hizMipCount = 0`），没有别的副作用。
    /// 【为什么默认关闭（而不是默认打开）】默认档要保持"与 CPU 参考剔除**逐簇一致**"这条硬验收。
    ///   CPU 拿不到 Hi-Z 金字塔的逐 texel 内容（RHI 的 `CopyTextureToBuffer` 只读 mip0，而
    ///   `BuildHiZPyramid` 从不写 mip0）⇒ 遮挡剔除**不可能**在 CPU 侧复现；打开时 GPU 与 CPU 的
    ///   差异是"可解释的"（`cull3` 行给出差集大小与选层分布），但不是"逐簇一致"。因此：
    ///     默认 false ⇒ mismatch=0（硬验收）；`nanite_hiz=1` ⇒ 差异可解释（软验收，单独统计）。
    /// 【依赖】Hi-Z 金字塔由既有 `GPUCulling::BuildHiZPyramid` 构建（`DeferredPipeline` 以回调
    ///   形式传给模块）。若该管线未启用（`gpu_cull=0`）或纹理不可用 ⇒ 本开关**自动退化**为关闭
    ///   （`cull3` 行里的 `hiz=off` + `hiz_req=1 hiz_mips=0` 就是这个情形）。
    /// 配置层 = cfg 键 `nanite_hiz`（默认 0），样例负责解析/序列化。
    bool hiz = false;

    /// 【P0 修复】Hi-Z 采样 UV 的 **y 翻转**（默认 **true** = 负高度视口的正确约定）。
    ///
    /// 【为什么必须有这一项】本引擎的离屏通道用**负高度视口**（`GBufferRenderer_CPU.cpp:61`：
    ///   `SetViewport({0,h,w,-h,0,1})`）⇒ NDC y=+1 落在帧缓冲**第 0 行**，而纹理 UV 的 v 向下增长
    ///   ⇒ 正确的采样 UV 是 `s = (ndc.x*0.5+0.5, 0.5-0.5*ndc.y)`；历史写法 `ndc.xy*0.5+0.5`
    ///   把 v 当"y 向上"用，采样到**上下颠倒**的 texel（同引擎内 `GI/SSR.frag.slang:67-68`
    ///   的 `NdcToUv` 就是正确的那条，并由平面镜解析对照实测确认）。**默认 true = 已修**。
    /// 【为什么还留 false 这一档】它是"镜像是否真的存在"这条结论的**可复现 A/B 对照**：
    ///   同一场景、同一相机、同一帧，只切换 `hiz_flip` 就能比较被遮挡簇的数量与落屏半屏分布；
    ///   配置层 = cfg 键 `nanite_hiz_flip`（默认 1），样例负责解析/序列化。
    ///   读数里的 `hiz_flip=` 与 `occl_uv=[上半屏,下半屏]` 是这条对照的出口。
    bool hizFlip = true;

    /// 【§14.8 任务 16】绘制来源：**假簇链自证 / 退化通道**（默认 **false** = 走可见簇列表）。
    ///
    /// 【语义】false（默认）⇒ 光栅端消费 `Nanite_ClusterBVH.comp.slang` 从**可见簇列表**写出的
    ///   真实间接命令（每条命令对应一个可见簇：真实 indexCount/firstIndex/vertexOffset/簇号）；
    ///   true ⇒ 消费任务 3 的**假簇链**（`nanite_fake_clusters` 条固定命令），可见链只算不画。
    /// 【为什么默认走可见簇列表】任务 16 的验收就是"绘制次数 = 可见簇数"，默认档必须是它。
    /// 【为什么保留假簇链】① 它是任务 3 验收口径（`count_buffer == indirect_cmds ==
    ///   rasterized_clusters == N`）的唯一载体，保留即可回归；② 可见链不可用时（BVH 未入库 /
    ///   实例域为 0 / 资产为空）它就是**退化路径** —— 此时若强行走可见链，绘制条数恒 0，
    ///   "模块确实画了东西"这条自证会消失。两条路复用同一段录制代码，切换只在一个判据上。
    /// 配置层 = cfg 键 `nanite_fake_chain`（默认 0），样例负责解析/序列化。
    bool fakeChain = false;

    /// 【§14.8 任务 16】间接绘制容量（默认 **0 = 用容量上界**）。
    ///
    /// 【语义】它是"可见簇 → 间接命令"这一步的截断门：只有槽位 < 本值的簇才写命令并计入
    ///   绘制计数（`visible` 读数保持**真值**不变）。0 或超过上界 ⇒ 用容量上界
    ///   （`kNaniteMaxIndirectDraws`），即正常路径**不截断**。
    /// 【为什么要有这个键】它是截断路径在**真实 GPU** 上的可复现自证开关：设小之后
    ///   `visible` 保持真值、`draws` 被钳住、截断计数非 0，而绘制恒不超过 `maxDrawCount`
    ///   （`vkCmdDrawIndexedIndirectCount` 的硬约束），可以逐位观察"截断而**不越界**"。
    /// 配置层 = cfg 键 `nanite_draw_capacity`（默认 0），样例负责解析/序列化。
    u32 drawCapacity = 0u;

    /// 【§14.8 任务 18】软光栅写 GBuffer 开关（默认 **true**）。
    ///
    /// 【语义】true（默认）⇒ **模块成为 GBuffer 段的几何写入者**：既有 `GB_Clear` 的
    ///   几何绘制**让位**（帧图侧门控，见 `DeferredPipeline_FrameGraph.cpp`），改为由模块
    ///   ①清屏 8×MRT + 深度、②软光栅两趟（原子深度键 + 等值复检写 albedo/normal/worldPos/
    ///   lightmapKey）、③深度解析（深度键 → `SV_Depth` 写既有深度附件）；
    ///   false ⇒ 既有 `GB_Clear` 的几何路径**原样执行**（模块只跑剔除链与光栅占位通道）。
    /// 【为什么默认 true】§14.2 不变式 3："开关打开时 GBuffer 的几何写入者唯一"—— 任务 18 是
    ///   模块真正接管几何写入的那一步。默认关闭会让 `nanite_enable=1` 的语义退回任务 16。
    /// 【为什么还留这个开关】它是**同场景同相机对照**的钥匙：`enabled=1; softRaster=0` 与
    ///   `enabled=1; softRaster=1` 除"谁写 GBuffer"之外其余全同，用于把差异归因到软光栅；
    ///   同时是回退档（软光栅出问题时不必关掉整个模块）。
    /// 配置层 = cfg 键 `nanite_soft_raster`（默认 1），样例负责解析/序列化。
    bool softRaster = true;

    /// 【§14.8 任务 18】走软光栅的**每簇三角形数上限**（默认 **16**，§5.2 的阈值）。
    ///
    /// 【语义】`cluster.triangleCount > softMaxTriangles` 的簇**跳过并计数**
    ///   （读数 `skipped_big`），留给任务 22 的 mesh shader 硬光栅（§5.2 的混合光栅分流）。
    /// 【为什么可配】验收要看"软光栅真的画出了东西"：Sponza 的簇绝大多数是满簇（64 tri），
    ///   阈值 16 下覆盖极少（这正是"覆盖范围差异"的来源，见实施记录）；把它调到 64 就能让
    ///   软光栅吃下全部可见簇，用于"同相机对照"里对照软光栅自身的正确性（而不是覆盖率）。
    /// 配置层 = cfg 键 `nanite_soft_max_triangles`（默认 16，钳到 [1, 64]），样例负责解析/序列化。
    u32 softMaxTriangles = 16u;
};

} // namespace he::render
