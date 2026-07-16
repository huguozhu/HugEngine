// ParticleSort.comp — Bitonic Sort 深度排序
// 按 particle_depth 降序排列 SortIndices，实现远→近渲染顺序
// 当前版本：单 workgroup 512 线程，shared memory 排序
//   (粒子数 ≤ 512 时一次 Dispatch 完成)

#include "ParticleTypes.slang"

// 输入/输出
[[vk::binding(0, 0)]] RWStructuredBuffer<SortInfo>  u_SortIndices;       // 待排序 + 输出
[[vk::binding(1, 0)]] StructuredBuffer<ParticleCounters> u_Counters;    // 读取 render_count

// Shared memory
groupshared SortInfo g_Shared[BITONIC_BLOCK_SIZE];

[shader("compute")]
[numthreads(BITONIC_BLOCK_SIZE, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID, uint3 gtid : SV_GroupThreadID) {
    // 读取本帧可见粒子数
    uint sortCount = u_Counters[0].render_count;

    // 限制在排序容量内
    if (sortCount > BITONIC_BLOCK_SIZE)
        sortCount = BITONIC_BLOCK_SIZE;

    // ── 加载数据到 Shared Memory ──
    uint idx = gtid.x;
    if (idx < sortCount) {
        g_Shared[idx] = u_SortIndices[idx];
    } else {
        // 超出范围的填充哨兵值（置于末尾）
        g_Shared[idx].particle_depth = -1e38;
        g_Shared[idx].particle_index = 0xFFFFFFFFu;
    }

    GroupMemoryBarrierWithGroupSync();

    // ── Bitonic Sort（降序：深度大的在前 = 远处先渲染）──
    for (uint k = 2u; k <= BITONIC_BLOCK_SIZE; k <<= 1u) {
        for (uint j = k >> 1u; j > 0u; j >>= 1u) {
            uint ixj = gtid.x ^ j;

            if (ixj > gtid.x) {
                // bitonic 标准方向: (idx & k)==0 → 升序, 否则降序
                bool ascending = ((gtid.x & k) == 0u);

                SortInfo a = g_Shared[gtid.x];
                SortInfo b = g_Shared[ixj];

                bool shouldSwap = false;

                // 哨兵处理：无效条目始终排到末尾
                if (a.particle_index == 0xFFFFFFFFu && b.particle_index != 0xFFFFFFFFu) {
                    shouldSwap = true;
                } else if (b.particle_index == 0xFFFFFFFFu) {
                    shouldSwap = false;
                } else if (ascending) {
                    // 降序总结果：gtid.x（小索引）存较大 depth（远处粒子）
                    shouldSwap = (a.particle_depth < b.particle_depth);
                } else {
                    // 降序总结果：gtid.x（小索引）存较大 depth
                    shouldSwap = (a.particle_depth > b.particle_depth);
                }

                if (shouldSwap) {
                    g_Shared[gtid.x] = b;
                    g_Shared[ixj]    = a;
                }
            }

            GroupMemoryBarrierWithGroupSync();
        }
    }

    // ── 写回排序结果 ──
    if (idx < sortCount) {
        u_SortIndices[idx] = g_Shared[idx];
    }
}
