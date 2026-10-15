#pragma once

#include "Core/Types.h"
#include "Threading/JobSystem.h"

#include <span>
#include <vector>

// ============================================================
// ParallelCollect —— 并行收集的共享骨架（分块并行 + 本地缓冲 + 确定性合并）
//
// 【为什么要有这个文件】本方案的三项改进（确定性写回 / 并行快照构建 / 上传并行化）都要用
// 同一段骨架：**按 chunk 并行处理，再以确定的方式合并**。合并方式选错会让输出顺序随线程
// 完成序变化 —— 那是本引擎已经踩过的坑（`SceneRenderer::Prepare` 原先用"加锁 append"合并，
// 顺序跨帧不稳定）：顺序不稳会打乱下游 TAA / 时域降噪的历史对应，也让 A/B 读数不可重复。
//
// 【确定性合并怎么做】不用锁、也不按完成序 append，而是两步：
//   ① 标记：`hit[index] = 0/1` —— 各线程各写各的下标，无冲突 ⇒ 不需要锁
//   ② 紧凑化：对 `hit` 求前缀和得到每个命中项的唯一 rank ⇒ 按下标升序写回
// 于是"输出顺序 = 输入下标升序"，与线程调度无关、跨帧稳定。
//
// 【前缀和为什么是分块的】完整并行扫描（Hillis-Steele/Blelloch）实现复杂度不划算：本文件用
// 经典的**分块扫描** —— 每块并行数命中数，块间做一次 O(块数) 的串行扫描，再并行在块内按下标
// 升序写回。工作量仍是 O(n)，串行部分只有块数级别（n=4096/chunk=64 ⇒ 64 次加法）。
// ============================================================

namespace he::render {

/// 并行收集/紧凑化的默认分块大小（与 `SceneRenderer::Prepare` 的剔除分块保持一致）
constexpr u32 kCollectChunkSize = 64;

/// 低于该候选数时走串行：小场景下并行（任务派发 + 等待）反而更慢
constexpr u32 kCollectMinParallelItems = 1024;

/// 串行参考实现。并行实现必须与它**逐元素一致**（这是本方案的判据之一）。
/// @param hit 命中标记（长度 = 候选总数）
/// @param out 输出下标（容量必须 >= hit.size()；只写 [0, 返回值)）
/// @return 命中数
inline u32 CompactHitsSerial(std::span<const u8> hit, std::span<u32> out) {
    u32 rank = 0;
    for (u32 i = 0; i < static_cast<u32>(hit.size()); ++i) {
        if (hit[i] != 0u) out[rank++] = i;
    }
    return rank;
}

/// 并行紧凑化：把命中标记压成**按原下标升序**的下标序列。
/// 【确定性】输出顺序只由 `hit` 决定（与 worker 数量、调度顺序无关）⇒ 同输入两次运行逐元素相同。
/// @param hit              命中标记（长度 = 候选总数）
/// @param out              输出下标（容量必须 >= hit.size()）
/// @param chunkSize        分块大小（0 ⇒ 用 `kCollectChunkSize`）
/// @param minParallelItems 低于此数走串行参考实现
/// @return 命中数
inline u32 CompactHits(std::span<const u8> hit, std::span<u32> out,
                       u32 chunkSize = 0u, u32 minParallelItems = kCollectMinParallelItems) {
    const u32 n = static_cast<u32>(hit.size());
    if (n == 0u) return 0u;
    if (chunkSize == 0u) chunkSize = kCollectChunkSize;

    // 小规模走串行（并行的派发/等待成本高于收益）
    if (n < minParallelItems) return CompactHitsSerial(hit, out);

    const u32 numChunks = (n + chunkSize - 1u) / chunkSize;

    // ① 每块命中数：每块只写自己的槽位 ⇒ 并行无冲突
    std::vector<u32> chunkBase(numChunks, 0u);
    JobSystem::Instance().ParallelForChunked(n, chunkSize, [&](u32 start, u32 end) {
        u32 count = 0u;
        for (u32 i = start; i < end; ++i) count += (hit[i] != 0u) ? 1u : 0u;
        chunkBase[start / chunkSize] = count;
    });

    // ② 块起始 rank：O(块数) 的串行扫描（块数远小于 n）
    u32 running = 0u;
    for (u32 c = 0; c < numChunks; ++c) {
        const u32 count = chunkBase[c];
        chunkBase[c] = running;
        running += count;
    }

    // ③ 块内按下标升序写回：每块只写 [base, base+count)，各块互不重叠
    JobSystem::Instance().ParallelForChunked(n, chunkSize, [&](u32 start, u32 end) {
        u32 rank = chunkBase[start / chunkSize];
        for (u32 i = start; i < end; ++i) {
            if (hit[i] != 0u) out[rank++] = i;
        }
    });

    return running;
}

} // namespace he::render
