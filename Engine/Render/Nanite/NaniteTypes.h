#pragma once

// ============================================================
// Nanite/NaniteTypes.h — Nanite 模块的纯数据类型与 objectIndex 分区契约
//
// 【本文件由 §14.8 任务 1 建立骨架】
//   任务 1 只放"编译得过、可被别处 include"的 POD 与常量；真正的数据格式（文件头/顶点/
//   索引/cone）在任务 7 定稿，量化与打包的边界判据在任务 10/11。
//
// 【为什么必须 RHI-free】§14.7：Scene 侧的 `MeshComponent` 只存"资产路径 + 不透明 u64 句柄"，
//   它需要 include 本头文件取常量与 POD，却**不能**因此牵入 Render 的类型（否则形成
//   Scene → Render 的反向依赖）。因此本文件只允许依赖 `Core/Types.h` 与标准库：
//   **不得 include 任何 RHI 头**，`rhi::` 类型、纹理/缓冲句柄一律不得出现在这里。
//
// 【§14.3 依赖禁令（Nanite 模块内每个文件共同遵守）】
//   · 模块内不得引用 `GI_*` / `Lumen*` / `GPUCulling` 的**内部结构**
//     （可以借其 Hi-Z 纹理句柄与描述符写法，但那属于后续任务在 .cpp 里的实现细节）；
//   · 不得依赖 `MeshBatcher` 的运行时状态 —— 它只当**一次性输入**（合并几何）。
//   · 公共面只有三个：`NaniteRenderer`（生命周期 + 帧图接入）、`NaniteSettings`（开关/档位）、
//     本文件的 POD。
// ============================================================

#include "Core/Types.h"

#include <cstddef>   // offsetof（钉住 POD 的字段偏移）

namespace he::render {

// ============================================================
// 光栅档位（§14.4 面板下拉："软光栅 / 混合光栅"）
//
// 任务 1 只放**枚举占位**：真值落在 `NaniteSettings::rasterMode`，当前没有任何 pass 消费它。
// 软光栅的落点（A1 给 GBuffer 加 UAV / A2 模块自建 VisBuffer）由任务 4 裁决（§14.5）。
// ============================================================
enum class NaniteRasterMode : u32 {
    Soft   = 0,   // 软光栅：compute 写 GBuffer（路线见 §14.5，任务 4 裁决 A1/A2）
    Hybrid = 1,   // 混合光栅：软光栅 + mesh shader 硬光栅分支（任务 6 / 22 起）
};

// ============================================================
// objectIndex 分区契约（§14.5「模块实例占独立 index 空间」；为任务 5 铺路）
//
// 【为什么需要分区】GBuffer 的 MRT7（`gb_lightmapkey`）把 objectIndex 当**页号**写进
//   `float4(uv0, objectIndex, 0)`（GBuffer.frag.slang:134）。若 Nanite 实例沿用普通段的
//   编号空间，同一个数字会同时指向 GPUScene 的对象条目与 Nanite 的实例条目，
//   任务 5 的判据（"混排场景下 gb_lightmapkey 解析正确、无越界"）就无从判定。
//
// 【分区表】
//   普通段  = [kNormalObjectIndexBegin, kNaniteObjectIndexBegin)          = [0, 1024)
//   Nanite 段 = [kNaniteObjectIndexBegin, kObjectIndexTotalCapacity)       = [1024, 17408)
//   · 普通段容量取 `kGPUMaxObjects = 1024`（ShaderTypes.slang:209 / Render/Pipeline/Material.h:45）。
//     这里**硬编码**该数字而**不** include Material.h：后者会牵入 RHI 头，破坏本文件的
//     RHI-free 约束。改 `kGPUMaxObjects` 时必须同步这里（单测在任务 5/11 钉住）。
//   · Nanite 段的"本地实例下标" = objectIndex − kNaniteObjectIndexBegin，索引的是模块
//     **自持**的 NaniteInstance 缓冲（不复用 GPUScene 的 object 缓冲）——
//     这正是 §14.5 所说"三处枚举一致性契约（SceneRenderer / MeshBatcher / GPUScene）
//     不受影响"的原因。
//   · 总容量 17408 远小于 2^24，保证写进 RGBA16F 的页号仍是**精确整数**
//     （Tools/gi/lightmap_key_check.py 的判据之一就是"页号是精确整数"）。
// ============================================================
inline constexpr u32 kNormalObjectIndexBegin    = 0u;
inline constexpr u32 kNormalObjectIndexCapacity = 1024u;   // = kGPUMaxObjects（见上面的说明）
inline constexpr u32 kNaniteObjectIndexBegin    = kNormalObjectIndexBegin + kNormalObjectIndexCapacity;
inline constexpr u32 kNaniteObjectIndexCapacity = 16384u;
inline constexpr u32 kObjectIndexTotalCapacity  = kNaniteObjectIndexBegin + kNaniteObjectIndexCapacity;

/// 该 objectIndex 是否落在 Nanite 段（任务 5 的解析入口：先判段，再取本地下标）
[[nodiscard]] constexpr bool IsNaniteObjectIndex(u32 objectIndex) {
    return objectIndex >= kNaniteObjectIndexBegin && objectIndex < kObjectIndexTotalCapacity;
}

/// Nanite 段内的本地实例下标（调用方须先判 IsNaniteObjectIndex）
[[nodiscard]] constexpr u32 NaniteLocalIndex(u32 objectIndex) {
    return objectIndex - kNaniteObjectIndexBegin;
}

/// 越界判据：混排场景里任何一个 objectIndex 都必须落在这两段之内
[[nodiscard]] constexpr bool IsValidObjectIndex(u32 objectIndex) {
    return objectIndex < kObjectIndexTotalCapacity;
}

// ============================================================
// 任务 3（§14.8）：「计数 → 间接绘制」链的共享 POD
//
// 【为什么放在本文件】它是 Nanite 模块与 Slang 通道之间的**二进制契约**：
//   C++ 侧（NaniteCull/NaniteRaster）与 GPU 侧（Nanite_Cull.comp.slang /
//   Nanite_Raster.frag.slang）必须对同一段内存给出完全一致的解释。契约只由
//   `static_assert` 钉住，**不引入任何 RHI 类型**，因此本文件仍是 RHI-free 的。
//
// 【与任务 7 的关系】任务 7 定稿的是"真实 .nanite 资产"的头部/顶点/索引格式；
//   这里只是任务 3 用假数据验证"计数 → 间接绘制"链所需的最小 POD，不含量化与 cone。
// ============================================================

/// 间接绘制命令：必须与 `VkDrawIndexedIndirectCommand` **二进制兼容**
/// （GLSL/Slang 侧见 `Nanite_Cull.comp.slang` 的 `IndirectCmd`）。
struct alignas(4) NaniteIndirectCommand {
    u32 indexCount    = 0;   // 偏移 0
    u32 instanceCount = 0;   // 偏移 4
    u32 firstIndex    = 0;   // 偏移 8
    i32 vertexOffset  = 0;   // 偏移 12（注意是**有符号**，与 Vulkan 一致）
    u32 firstInstance = 0;   // 偏移 16
};
static_assert(sizeof(NaniteIndirectCommand) == 20,
              "间接命令必须是 20 字节（VkDrawIndexedIndirectCommand / DGC 步长）");
static_assert(offsetof(NaniteIndirectCommand, indexCount)    == 0,  "indexCount 必须在偏移 0");
static_assert(offsetof(NaniteIndirectCommand, instanceCount) == 4,  "instanceCount 必须在偏移 4");
static_assert(offsetof(NaniteIndirectCommand, firstIndex)    == 8,  "firstIndex 必须在偏移 8");
static_assert(offsetof(NaniteIndirectCommand, vertexOffset)  == 12, "vertexOffset 必须在偏移 12");
static_assert(offsetof(NaniteIndirectCommand, firstInstance) == 16, "firstInstance 必须在偏移 16");

/// 假簇条目（任务 3 的输入；与 `Nanite_Cull.comp.slang` 的 `FakeCluster` 一致）
struct alignas(4) NaniteFakeCluster {
    u32 clusterId     = 0;   // 簇编号（假数据 = 顺序编号）
    u32 instanceId    = 0;   // 所属实例（任务 3 固定 0：1 个实例）
    u32 triangleCount = 1;   // 该簇三角形数（假数据固定 1）
    u32 _pad          = 0;   // 填充到 16 B
};
static_assert(sizeof(NaniteFakeCluster) == 16, "假簇条目必须 16 字节（4×u32，便于对齐读回）");

// ── 计数缓冲布局 ──
// 计数缓冲是**单个 u32**：GPU compute 用 InterlockedAdd 累加"实际写入的间接命令条数"，
// 绘制端把这个值直接交给 `DrawIndexedIndirectCount`（由它替代 CPU 决定绘制条数）。
inline constexpr u32 kNaniteCountBufferU32 = 1u;
inline constexpr u32 kNaniteCountBufferSize = sizeof(u32) * kNaniteCountBufferU32;

// ── 假簇链路的容量上限 ──
// 间接命令缓冲 / 计数缓冲 / 光栅化计数缓冲都按它分配；`nanite_fake_clusters` 会被
// 钳制到该上限（超出部分不绘制，而不是越界）。
inline constexpr u32 kNaniteMaxFakeClusters = 1024u;

/// 每条间接命令的 indexCount（假数据：1 个三角形 = 3 个索引）。
/// 读回时用它判定"这个槽位确实被 GPU 写过"（未写过的槽位会被 CPU 预填成哨兵值）。
inline constexpr u32 kNaniteFakeClusterIndexCount = 3u;

// ── 绘制端自建的小目标尺寸 ──
// 【为什么是 1×1】绘制端只用它来"数次数"：1×1 目标 + 1×1 视口 ⇒ 每条间接命令恰好
// 产生 1 个片元 ⇒ 片元里的原子加就等于被光栅化的簇数。它**不是** GBuffer 的任何附件，
// 因此这个 pass 对可见画面零影响（§14.2 不变式 1、§14.8 任务 3 的验收）。
inline constexpr u32 kNaniteRasterTargetSize = 1u;

} // namespace he::render
