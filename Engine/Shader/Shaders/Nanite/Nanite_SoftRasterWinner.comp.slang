// ============================================================
// Nanite_SoftRasterWinner.comp.slang — 软光栅**第 2.5 趟（新增的 B 趟）**：确定性赢家仲裁
//
// 【为什么需要这一趟（本文件存在的唯一理由）】
//   既有的两趟写法是："第 1 趟原子定最小深度键（A 趟）→ 第 2 趟重跑光栅化、与 `u_DepthKey[p]`
//   做**等值复检**、相等才写 GBuffer（C 趟）"。这套写法有一个**被实测证实的缺陷**：
//   深度键的低 8 位只是**簇内**三角形下标（`key = (asuint(ndcZ) & 0xFFFFFF00u) | (triLocal & 0xFFu)`），
//   它对"同一个簇"内部是全序，**跨簇完全没有唯一性**。于是：
//     · 簇 X 的三角形 t 与簇 Y 的三角形 t'（t' 与 t 的低 8 位相同、深度位模式也相同）
//       会算出**逐位相同**的键；
//     · 第 1 趟的 `InterlockedMin` 之后，`u_DepthKey[p]` 就是这一个键值；
//     · 第 2 趟里 X 和 Y 的这两个候选**都**通过等值复检 ⇒ 两个候选都去写同一批像素；
//     · "谁最后写谁赢"完全由 UAV 写序决定 ⇒ 模块接管档**两次相同的运行不是逐位可复现**的
//       （本仓库实测，同一配置背靠背两次转储：阈值 16 档 4 张 GBuffer 有 68 个字段不同；
//        阈值 64 档 4 张 GBuffer 有 587 万个字段不同、全部转储合计 800 万个）。
//   【修法】在第 1 趟与第 2 趟之间插入这一趟：把"每个像素的赢家"从"写序"改成一个**全局唯一的
//   候选号**的原子最小值 ⇒ 赢家只由 (键, 候选号) 这个二元组决定，与任何写序无关。
//     · 候选号 = `(ref.cluster << 12) | (ref.instance << 6) | (triLocal & 0x3Fu)` —— **稳定身份**
//       （`ref` = `u_VisibleClusters[slot]`，即 (instance, cluster) 这条引用；`triLocal` 是簇内
//       三角形下标）。**为什么不能拿 `gid.x` 当身份**见下面 ④ 的长注释：可见簇列表是剔除链用
//       `InterlockedAdd` 追加出来的，**槽位顺序由 GPU 线程调度决定、两次运行不同** ⇒ 用槽位做
//       仲裁等于没仲裁（实测：两次运行的槽位顺序不同，仲裁出来的赢家跟着变）。
//     · 本趟只对"通过等值复检"的 (像素, 候选) 做 `InterlockedMin(u_WinnerID[p], candidateID)`；
//       `u_WinnerID` 每帧被清成 `0xFFFFFFFF`（哨兵 = 没有候选），故原子最小等价于"取最小候选号"。
//     · 最终每个像素的赢家 = 键最小者；键相同 ⇒ 候选号最小者。**全序、与写序无关、与列表顺序无关**。
//
// 【三趟的口径（零额外设备特性）】
//   A 趟（既有）`Nanite_SoftRasterDepth.comp.slang`：`InterlockedMin(u_DepthKey[p], key)` —— 深度最小；
//   B 趟（本文件）：重跑**与 C 趟逐字相同**的光栅化，对等值复检命中的候选 `InterlockedMin` 候选号；
//   C 趟（既有，已加一道判定）`Nanite_SoftRaster.comp.slang`：等值复检之后再判
//     `u_WinnerID[p] == candidateID`，不等则 `continue` ⇒ 每个像素**恰好一个**赢家写 GBuffer。
//   【不引入任何新设备特性】只用了 `RWStructuredBuffer<uint>` 上的 `InterlockedMin` —— 与 A 趟
//   完全同一个原子、同一个格式、同一套缓冲创建路径（同尺寸同格式的 R32_UINT storage buffer
//   再要一块而已）。**没有** 64 位原子、**没有** ROV/interlock、**没有** 新扩展。
//   【帧图不变】三个派发都录在既有 `Nanite_CullChain3` 的 pass 体内（用命令缓冲里的显式屏障定序）
//   ⇒ 帧图的 pass 列表、资源依赖、冻结指纹都不变（与任务 15/16/18 的"录在产出 pass 体内"同一口径）。
//
// 【本趟的读写边界（必须严格守住）】
//   · 只写 `u_WinnerID`；**不写任何 GBuffer**、**不写 `u_DepthKey`**；
//   · **不加任何一个 `kSoftStat*` 计数**（`pixels_written` / `material_pixels` / `fallback_pixels` /
//     `depth_key_ties` 一个都不碰）—— 仲裁趟不是"写像素"，它只决定"谁有资格写"，
//     把它计进像素读数会让 `soft_raster` 读数行的语义被悄悄改掉。
//
// 【§14.3 依赖禁令】只依赖 `NaniteTypes.slang` 与既有 GBuffer 契约（与另外两趟同源）。
// ============================================================

#include "Nanite_SoftRasterCommon.slang"

// 赢家候选号缓冲（R32_UINT，与深度键**同尺寸同格式**；每帧由命令缓冲内的拷贝清成 0xFFFFFFFF）
// 【为什么放在 binding 21】写色趟（C 趟）的集合 `m_SoftColorSet` 已经占了 0..20；21 是紧接着的
//   第一个空闲槽位。本趟**复用写色趟的描述符集布局与集合**（不必新建布局/集合），多出来的这一个
//   绑定：只有本趟与写色趟声明它（写色趟要用它做候选号复检）。**第 1 趟（A，深度键）的布局里
//   没有 21** ⇒ 录制时绝不能给那个集合写这个绑定（对不存在的绑定写描述符是越界写）。
[[vk::binding(21, 0)]] RWStructuredBuffer<uint> u_WinnerID;

[numthreads(16, 1, 1)]
void main(uint3 tid : SV_GroupThreadID, uint3 gid : SV_GroupID) {
    // ── ① 与第 1 趟 / 第 2 趟**逐字相同**的取簇逻辑（三趟口径一致是等值复检成立的前提）──
    uint visibleCount = u_VisibleClusterCount[0];
    uint slot = gid.x;
    if (slot >= visibleCount) return;

    ClusterRef ref = u_VisibleClusters[slot];
    if (ref.instance >= instanceCount) return;
    if (pagesEnabled != 0u) {
        // 【任务 24】与第 1/2 趟逐字相同的收口口径（见 `Nanite_SoftRasterDepth.comp.slang`）
        if (ref.cluster >= assetClusterCount) return;
    } else {
        uint clusterCapacity = 0u;
        uint clusterStride = 0u;
        u_Clusters.GetDimensions(clusterCapacity, clusterStride);
        if (ref.cluster >= clusterCapacity) return;
    }

    InstanceObject instance = u_Instances[ref.instance];
    if (instance.indexCount == 0u) return;
    float3 translation = instance.ltw3.xyz;

    // ── 【任务 24】与第 1/2 趟**逐字相同**的页解析（缺页的簇没有深度键 ⇒ 本趟也必须跳过）──
    SoftRasterClusterView view = softRasterResolveCluster(ref.cluster);
    if (!view.valid) return;
    NaniteCluster cluster = view.cluster;
    uint triangleCount = cluster.triangleCount;
    // 【防御：夹到资产契约的上限 64】与第 1 趟同一条硬界。夹取之后本趟枚举的 `triLocal` 范围
    //   与第 1 趟**写键**的范围完全一致（第 1 趟也是先夹到 64、再判阈值）⇒ 第 1 趟可能写下的
    //   任何深度键，本趟都有机会枚举到对应三角形去仲裁；> 64 的越界三角形（只可能来自损坏
    //   资产）本来就不在第 1 趟的键集合里，本趟不看它不会漏掉任何**合法**候选。
    if (triangleCount > 64u) triangleCount = 64u;
    if (triangleCount == 0u) return;
    if (triangleCount > maxTriangles) return;    // 与第 1/2 趟同一条跳过规则（计数只在第 1 趟做）

    // ── ② 逐三角形：重跑光栅化 → 等值复检 → 原子最小候选号 ──
    for (uint triLocal = tid.x; triLocal < triangleCount; triLocal += 16u) {
        SoftRasterTriangle tri = softRasterFetchTriangle(view, triLocal, translation);
        if (!tri.valid) continue;

        float2 lo = min(min(tri.p0, tri.p1), tri.p2);
        float2 hi = max(max(tri.p0, tri.p1), tri.p2);
        int2 bmin = int2(clamp(floor(lo), float2(0.0, 0.0),
                               float2((float)screenWidth - 1.0, (float)screenHeight - 1.0)));
        int2 bmax = int2(clamp(ceil(hi), float2(0.0, 0.0),
                               float2((float)screenWidth - 1.0, (float)screenHeight - 1.0)));

        for (int py = bmin.y; py <= bmax.y; ++py) {
            for (int px = bmin.x; px <= bmax.x; ++px) {
                float2 pixelCenter = float2((float)px + 0.5, (float)py + 0.5);
                float3 bary;
                if (!softRasterCovered(tri, pixelCenter, bary)) continue;

                // NDC 深度是屏幕坐标的线性函数 ⇒ 直接重心插值（与第 1/2 趟逐位同式）
                float ndcZ = bary.x * tri.z0 + bary.y * tri.z1 + bary.z * tri.z2;
                uint key = softRasterDepthKey(ndcZ, triLocal);

                // ── ③ 与第 2 趟**逐字相同**的深度等值复检：只有命中的候选才参与仲裁 ──
                uint pixelIndex = (uint)py * screenWidth + (uint)px;
                if (u_DepthKey[pixelIndex] != key) continue;

                // ── ④ 确定性仲裁：全局唯一候选号的原子最小 ──
                // 【为什么**不能**用 `gid.x`（可见簇列表槽位）当身份（实测踩过，本趟的第一版就是这么写的）】
                //   可见簇列表是剔除链（`Nanite_ClusterBVH.comp.slang`）用
                //   `InterlockedAdd(u_VisibleClusterCount[0], 1u, slot)` **追加**出来的：每个实例一个
                //   线程各自遍历自己的 BVH 并抢号 ⇒ **谁先抢到哪个槽位由 GPU 线程调度决定**，两次
                //   运行并不相同（同一帧的 `cull3` 行只保证**集合**一致 —— 它在比较前把列表按
                //   (instance, cluster) 排序了，所以 `mismatch=0` 完全不能说明顺序稳定）。
                //   后果：用 `(gid.x << 8) | triLocal` 仲裁时，赢家会跟着槽位洗牌而变 ——
                //   实测两次运行转储仍有 800 万个元素不同（"面片归属换了一批"，不是数值噪声）。
                //   ⇒ 候选号必须只依赖**与列表顺序无关的稳定身份**。
                //
                // 【稳定身份的打包（单射，26 位）】三个字段都取自 `ref` 与 `triLocal`（都与槽位无关）：
                //     bits 12..25 : `ref.cluster`  —— 资产簇下标，上界 `kNaniteMaxBVHClusters = 16384 = 2^14`
                //     bits  6..11 : `ref.instance` —— 实例下标，  上界 `kNaniteMaxBVHInstances = 64 = 2^6`
                //     bits  0..5  : `triLocal`     —— 簇内三角形，上界 `kNaniteMaxClusterTriangles = 64 = 2^6`
                //   三段**互不重叠**且各自的取值都在位宽之内 ⇒ 映射是单射（不同的 (实例, 簇, 三角形)
                //   三元组绝不会撞号）。最大值 `16383<<12 | 63<<6 | 63 = 0x3FFFFF` < 2^26，不溢出。
                //   【⚠ 上限一旦调大就会撞号】这三个位段与上面的三个常量**耦合**：把任何一个常量
                //   调大到超过它的位宽（例如簇数上限 > 16384），两个不同的候选就会算出同一个编号，
                //   仲裁随即失效（等价于退回"写序决定"）。C++ 侧 `NaniteTypes.h` 有编译期断言钉住
                //   这三个上界，**改大上限必须同步改这里的位段**（断言会先报错，不会静默出错）。
                //   【与深度键低 8 位的关系】深度键的低 8 位仍然只是"簇内三角形下标"（那一位的语义
                //   不变，也不需要变）；本趟的候选号是**另一个**、独立的编码，只在仲裁里使用。
                uint candidateID = (ref.cluster << 12) | (ref.instance << 6) | (triLocal & 0x3Fu);
                InterlockedMin(u_WinnerID[pixelIndex], candidateID);
            }
        }
    }
}
