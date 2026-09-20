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
};

} // namespace he::render
