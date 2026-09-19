#pragma once

// ============================================================
// Lumen/SurfaceCacheTypes.h — Surface Cache 页表（步骤 14，L2）
//
// 【数据布局】页表项与请求结构定义在 **`Lumen/SurfaceCache.slang`**（C++ 与 Slang 共享一份），
// 这里只做三件事：
//   1. 把 Slang 侧的六态常量包成 C++ 可读的名字 + 合法性判据（`IsLegalPageTransition`）；
//   2. 给出 CPU 侧页状态机 `SurfaceCachePageTable`（分配/捕获/置脏/淘汰，非法迁移立刻断言）；
//   3. 用 static_assert 把两侧布局钉死（大小 + 关键字段偏移）。
//
// 【为什么状态机要在 CPU 侧先立起来】页的分配/淘汰是全局策略（LRU、优先级、显存预算），
// 由 CPU 决策、GPU 执行；GPU 侧只需要"按页号读写"。所以"合法性"这件事在 CPU 侧定死，
// GPU 侧的写入由捕获 pass 依据状态做门控（步骤 15+）。
//
// 【非法迁移】`HE_ASSERT` 在 Debug 下直接炸；Release 下（宏为空）函数返回 false 且不改状态 ——
// 保证"表永远不会进入非法状态"，这比"悄悄改坏"安全。
// ============================================================

#include "Core/Assert.h"
#include "Core/Types.h"   // u32/u64（本头要能被单测直接 include，不能依赖 RHI.h 先被包含）

#include <cstdint>
#include <vector>

#include "Lumen/SurfaceCache.slang"   // 共享布局（C++/Slang 镜像）

namespace he::render {

using SurfaceCachePageState = u32;

/// 六态的中文名（日志/断言信息用）
[[nodiscard]] const char* SurfaceCachePageStateName(u32 state);

/// 迁移合法性：唯一真值表（单测直接覆盖它）
///   Invalid    → Requested
///   Requested  → Allocating | Invalid(取消)
///   Allocating → Capturing  | Invalid(分配失败)
///   Capturing  → Captured   | Invalid(捕获失败/回收)
///   Captured   → Dirty      | Invalid(淘汰)
///   Dirty      → Capturing  | Invalid(淘汰)
/// 任何状态 → Invalid 都合法（释放/淘汰是最通用的兜底）
[[nodiscard]] bool IsLegalPageTransition(u32 from, u32 to);

/// 页表项的 CPU 侧状态机 + 统计。GPU 侧的镜像缓冲由 `LumenScene` 维护。
class SurfaceCachePageTable {
public:
    /// 建表（pageCount 个条目，全部 Invalid）
    void Resize(u32 pageCount);
    /// 从已有的条目数组恢复（GPU 回读校验用）
    void Reset(std::vector<SurfaceCachePageEntry> entries);

    [[nodiscard]] u32 Size() const { return (u32)m_Entries.size(); }
    [[nodiscard]] const SurfaceCachePageEntry& Get(u32 page) const { return m_Entries[page]; }
    [[nodiscard]] const std::vector<SurfaceCachePageEntry>& Entries() const { return m_Entries; }

    /// 请求：Invalid → Requested（记录卡片与请求帧）
    bool Request(u32 page, u32 cardIndex, u32 frame);
    /// 分配：Requested → Allocating，随后立刻认为自己已分配好物理页（Allocating → Capturing 由 BeginCapture 触发）
    bool Allocate(u32 page, u32 physicalPage);
    /// 开始捕获：Allocating | Dirty → Capturing
    bool BeginCapture(u32 page);
    /// 捕获结束：Capturing → Captured（ok=false 则 → Invalid）
    bool EndCapture(u32 page, bool ok);
    /// 置脏：Captured → Dirty
    bool MarkDirty(u32 page);
    /// 淘汰/释放：任意状态 → Invalid（同时释放物理页号）
    bool Evict(u32 page);
    /// 标记最近使用（LRU）
    void Touch(u32 page, u32 frame);

    /// 统计某状态的页数
    [[nodiscard]] u32 Count(u32 state) const;
    /// 是否还有"活着"的页（非 Invalid）
    [[nodiscard]] bool AnyLive() const;

    /// 校验和（**32 位**，避开 SPIR-V 的 Int64 能力）：CPU 计算，GPU 侧用同一套公式算
    /// 公式必须与 shader 里逐字一致（任一侧改动都会被 smoke 时的比对抓住）。
    [[nodiscard]] u32 Checksum() const;

private:
    /// 唯一的迁移入口：非法 ⇒ HE_ASSERT + 返回 false（Release 下不改状态）
    bool Transition(u32 page, u32 to);

    std::vector<SurfaceCachePageEntry> m_Entries;
};

} // namespace he::render
