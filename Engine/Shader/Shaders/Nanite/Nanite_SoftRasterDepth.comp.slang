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
    if (pagesEnabled != 0u) {
        // 【任务 24】开启档：页映射的下标空间是**资产**簇下标 ⇒ 抗越界判据按资产簇数收口
        //   （binding 0 这时绑的是页池，它的元素个数可能比簇数小，GetDimensions 不适用）
        if (ref.cluster >= assetClusterCount) return;
    } else {
        // 【关闭档：与任务 23 逐字相同的既有判据，一字未改】
        uint clusterCapacity = 0u;
        uint srcStride = 0u;
        u_Clusters.GetDimensions(clusterCapacity, srcStride);
        if (ref.cluster >= clusterCapacity) return;
    }

    InstanceObject instance = u_Instances[ref.instance];
    if (instance.indexCount == 0u) return;          // 空实例：与剔除链的跳过规则一致
    float3 translation = instance.ltw3.xyz;

    // ── 【任务 24】页解析：取记录 + 三段基址（缺页 ⇒ 跳过、计数、上报请求）──
    // 【位置：必须在任何分流判据之前】这样"每个可见簇的页归属"只判定一次，且与
    //   `soft + skipped_big + page_misses == visible` 这条不变式对齐（三条互斥且全覆盖）。
    SoftRasterClusterView view = softRasterResolveCluster(ref.cluster);
    if (!view.valid) {
        if (view.page != 0xFFFFFFFFu) {          // 页未驻留（不是非法引用）
            if (tid.x == 0u) {
                InterlockedAdd(u_Stats[kSoftStatPageMissClusters], 1u);
                softRasterRequestPage(view.page);   // 由 0 号线程上报一次（每簇一个工作组）
            }
        }
        return;
    }
    NaniteCluster cluster = view.cluster;
    // 【防御：把三角形数夹到**资产契约的上限 64**，而不是夹到 `maxTriangles`】
    //   【为什么不能夹到 maxTriangles（实测踩过）】那会把 64 个三角形的簇"变成"16 个，
    //   于是它们**不再被分流判据跳过**、全部涌进软光栅：实测 `soft=31526 / skipped_big=0 /
    //   buckets=[61,0,31465,0,0] / tested_px=285M`（健康档是 `soft=61 / skipped_big=31587 /
    //   buckets=[61,0,0,0,31587] / tested_px=9767`）—— 读数与画面全变，是**语义破坏**而非加固。
    //   【夹到 64 是语义不可见的】`NANITE_MAX_CLUSTER_TRIANGLES` 是资产契约：合法簇恒 ≤64；
    //   而 >64 的簇在夹取前后都会走"超阈值跳过"那条路，分桶也都是最后一桶 ⇒ 任何读数都不变。
    //   它只是给下面那个 `for` 与第 2 趟的循环加一道与资产契约同源的硬界（防损坏记录）。
    uint triangleCount = cluster.triangleCount;
    if (triangleCount > 64u) triangleCount = 64u;
    if (triangleCount == 0u) return;
    cluster.triangleCount = triangleCount;

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
        SoftRasterTriangle tri = softRasterFetchTriangle(view, triLocal, translation);
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

                // 原子最小 = "取最近"（非负浮点位模式与无符号整数同序）。
                // 【任务 26 / §14.34 第 7 行：顺手拿到"平局次数"】
                //   原子操作会**返回旧值** `prev`：若 `prev == key`，说明这个像素上已经有一个
                //   **键完全相同**的三角形先到（低 8 位含簇内三角形下标，故"键相同"= 深度位模式
                //   与该三角形下标都相同）⇒ 本次就是一次平局，也就是"这个像素会被多个三角形
                //   通过等值复检"的那一次 —— §14.31 ⑩ 记录的"UAV 写序决定像素"正是从这里来的。
                //   【为什么必须排除哨兵】第 1 趟之前缓冲被清成 0xFFFFFFFF；若某个像素的旧值仍是
                //   哨兵而本次写入的键恰好也是 0xFFFFFFFF（合法几何不可能：NDC 深度的浮点位模式
                //   恒 < 0x3F800100），那只是"第一次写入"，不是平局。显式排掉它，不改判据语义。
                uint prev = 0u;
                InterlockedMin(u_DepthKey[(uint)py * screenWidth + (uint)px], key, prev);
                if (prev == key && key != 0xFFFFFFFFu) {
                    InterlockedAdd(u_Stats[kSoftStatDepthKeyTies], 1u);
                }
            }
        }
    }
}
