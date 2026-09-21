// ============================================================
// Nanite_SoftRasterDepth.comp.slang — 软光栅**第 1 趟**：原子深度键（只写深度，不碰 GBuffer）
//
// 【为什么需要这一趟】见 `Nanite_SoftRasterCommon.slang` 顶部的完整裁决：本仓库的 Slang 版本
//   在 compute 里**静默丢弃** ROV 语义 ⇒ 单趟"互锁写 GBuffer"有竞态。两趟写法的第 1 趟只做
//   逐像素的 `InterlockedMin(u_DepthKey[p], key)`（R32_UINT 存储图像上的无符号原子最小，
//   零额外设备特性），把"每个像素最终归哪个三角形"定下来；第 2 趟
//   （`Nanite_SoftRaster.comp.slang`）重跑同样的光栅化并做**等值复检**后才写颜色。
//
// 【工作组织】**每簇一个工作组**（`[numthreads(16,1,1)]`，dispatch = 可见簇数）：
//   · 工作组 id = 可见簇列表里的槽位；
//   · 组内线程按 `tid + k×16` 的步长遍历该簇的三角形（≤64 ⇒ 最多 4 轮），
//     每个线程独立地把自己那个三角形的**整个 2D 包围盒**跑一遍 —— 与设计 §5.2 的
//     "逐三角形光栅化（投影到屏幕、2D 包围盒、重心坐标）"一致，只是把"每簇一个 wave、
//     像素级并行"换成"每簇一个工作组、三角形级并行 + 像素串行"，代价是 bbox 内串行，
//     换来实现简单与两趟逐位一致（本任务的验收只要求等效，不要求性能，性能归任务 23）。
//
// 【跳过规则（本任务的明确边界）】`cluster.triangleCount > maxTriangles`（默认 16，§5.2）
//   的簇**跳过并计数**（`kSoftStatSkippedClusters`），留给任务 22 的 mesh shader 硬光栅。
//   空可见列表 / 越界引用 / 空实例都跳过（与剔除链的规则一致）。
//
// 【§14.3 依赖禁令】只依赖 `NaniteTypes.slang` 与既有 GBuffer 契约。
// ============================================================

#include "Nanite_SoftRasterCommon.slang"

[numthreads(16, 1, 1)]
void main(uint3 tid : SV_GroupThreadID, uint3 gid : SV_GroupID) {
    // 【诊断】在任何提前返回**之前**，由工作组 0 的 0 号线程把 shader 实际收到的 push constant
    //   回读进读数缓冲 —— 布局错位（例如 screenWidth 读到 0）在这里一眼可见，不必靠猜。
    if (gid.x == 0u && tid.x == 0u) {
        u_Stats[kSoftStatDiagScreenW] = screenWidth;
        u_Stats[kSoftStatDiagScreenH] = screenHeight;
        u_Stats[kSoftStatDiagMaxTri]  = maxTriangles;
        u_Stats[kSoftStatDiagExtent]  = (uint)max(0.0, meshMaxExtent * 1000.0);
    }
    // ── ① 这个工作组负责哪条可见簇引用 ──
    // 【为什么按可见簇列表而不是"全部簇"】剔除链（任务 15/16）已经把"本帧可见"的簇压缩成
    //   一张列表 + 一个计数；软光栅只消费它 ⇒ "软光栅画了什么"与"剔除说可见什么"同源。
    uint visibleCount = u_VisibleClusterCount[0];
    uint slot = gid.x;
    if (slot >= visibleCount) return;               // 多派发的组：直接返回（dispatch 按容量取整）

    ClusterRef ref = u_VisibleClusters[slot];
    if (ref.instance >= instanceCount) return;   // 越界引用：不读实例表（防御性）
    uint clusterCapacity = 0u;
    uint clusterStride = 0u;
    u_Clusters.GetDimensions(clusterCapacity, clusterStride);
    if (ref.cluster >= clusterCapacity) return;

    InstanceObject instance = u_Instances[ref.instance];
    if (instance.indexCount == 0u) return;          // 空实例：与剔除链的跳过规则一致
    float3 translation = instance.ltw3.xyz;

    NaniteCluster cluster = u_Clusters[ref.cluster];
    uint triangleCount = cluster.triangleCount;
    if (triangleCount == 0u) return;

    // ── 【任务 23】簇大小分布：**在分流判据之前**计数 ⇒ 五桶覆盖"软 + 硬"两侧的全部可见簇。
    //   不变式：五桶之和 == kSoftStatRasterClusters + kSoftStatSkippedClusters
    //   （同一次枚举、同一个合法性子集），由 C++ 侧的 `size_dist` 读数行核对。
    //   只让 0 号线程加一次（每簇一个工作组），与下面两条分流计数的写法一致。
    if (tid.x == 0u) {
        InterlockedAdd(u_Stats[kSoftStatSizeBucket0 + softRasterSizeBucket(triangleCount)], 1u);
    }

    // ── ② 只对 ≤ maxTriangles 的簇走软光栅；大的跳过并计数（任务 22 的硬光栅）──
    if (triangleCount > maxTriangles) {
        if (tid.x == 0u) InterlockedAdd(u_Stats[kSoftStatSkippedClusters], 1u);
        return;
    }
    if (tid.x == 0u) InterlockedAdd(u_Stats[kSoftStatRasterClusters], 1u);
    // 【诊断】由工作组 0 的 0 号线程把"shader 实际收到的 push constant"回读进读数缓冲 ——
    // 若 push constant 布局错位（例如 screenWidth 读到 0），这里一眼可见，不必靠猜。


    // ── ③ 逐三角形：投影 → 2D 包围盒 → 重心坐标 → 原子深度键 ──
    for (uint triLocal = tid.x; triLocal < triangleCount; triLocal += 16u) {
        SoftRasterTriangle tri = softRasterFetchTriangle(cluster, triLocal, translation);
        if (!tri.valid) {
            InterlockedAdd(u_Stats[kSoftStatDegenerate], 1u);
            continue;
        }
        InterlockedAdd(u_Stats[kSoftStatTriangles], 1u);

        // 包围盒：三个顶点的 min/max，夹到帧缓冲范围（左下闭、右上开）
        float2 lo = min(min(tri.p0, tri.p1), tri.p2);
        float2 hi = max(max(tri.p0, tri.p1), tri.p2);
        int2 bmin = int2(clamp(floor(lo), float2(0.0, 0.0),
                               float2((float)screenWidth - 1.0, (float)screenHeight - 1.0)));
        int2 bmax = int2(clamp(ceil(hi), float2(0.0, 0.0),
                               float2((float)screenWidth - 1.0, (float)screenHeight - 1.0)));

        for (int py = bmin.y; py <= bmax.y; ++py) {
            for (int px = bmin.x; px <= bmax.x; ++px) {
                InterlockedAdd(u_Stats[kSoftStatTestedPixels], 1u);   // 诊断：包围盒内的像素测试数
                float3 bary;
                if (!softRasterCovered(tri, float2((float)px + 0.5, (float)py + 0.5), bary)) continue;
                InterlockedAdd(u_Stats[kSoftStatCoveredPixels], 1u);   // 诊断：覆盖测试通过的像素数

                // NDC 深度是屏幕坐标的线性函数 ⇒ 直接重心插值（不需要透视校正）
                float ndcZ = bary.x * tri.z0 + bary.y * tri.z1 + bary.z * tri.z2;
                uint key = softRasterDepthKey(ndcZ, triLocal);

                // 原子最小 = "取最近"（非负浮点位模式与无符号整数同序）
                InterlockedMin(u_DepthKey[(uint)py * screenWidth + (uint)px], key);
            }
        }
    }
}
