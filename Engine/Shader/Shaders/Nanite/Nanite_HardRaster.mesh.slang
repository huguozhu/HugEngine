// ============================================================
// Nanite_HardRaster.mesh.slang — §14.8 任务 22：mesh shader **硬光栅**（混合光栅的大簇一侧）
//
// 【本文件在分流里的位置】设计 §5.2 的原文口径：
//     Hardware (Mesh Shader):  cluster.triCount >  16
//     Software (Compute):      cluster.triCount <= 16
//   软光栅（任务 18）已经按 `triangleCount > maxTriangles` **跳过并计数**（`skipped_big`），
//   本文件消费的**正是那个被跳过的集合** —— 同一个阈值、同一份可见簇列表、同一份资产段。
//   于是"谁能画"只有一条判据，两端不可能出现重叠或空洞。
//
// 【为什么一个网格工作组 = 一条可见簇引用（照抄软光栅的工作组织）】
//   软光栅第 1/2 趟都是 `dispatch = 可见簇容量`、`gid.x = 可见簇列表槽位`、按
//   `slot >= visibleCount` 早退（`Nanite_SoftRasterDepth.comp.slang`）。硬光栅沿用同一约定：
//   `DrawMeshTasks(可见簇容量)`、`SV_GroupID.x = 槽位`。好处有三：
//     ① 两条光栅路径对"第 k 个可见簇是谁"的解读**逐字相同**，不会错位；
//     ② 不需要任何新的 GPU 侧压缩/计数缓冲（可见簇列表已经是剔除链的产物，
//        §14.3 的"不新增依赖"得以保持）；
//     ③ 任务数由 CPU 常量（容量）给出、由 shader 早退收口 —— 与软光栅的"按容量派发、
//        按 GPU 计数早退"是同一条既有口径（`NaniteRaster.h` 里已写明它的理由：
//        可见计数是**同一帧 GPU 刚写出**的，CPU 读不到，读回会破坏无停顿）。
//
// 【为什么不用 DrawMeshTasksIndirectCount】RHI 已备有该入口（上一轮为任务 22 加的），但本实现
//   没有用它：可见簇计数里**混着大小簇**，用它并不能把任务数直接压到"大簇数"，仍然要靠 shader
//   早退；而"按容量派发"是模块内既有且已验收的写法。留着那条入口不影响本路径。
//
// 【为什么声明 192 个输出顶点而不是 128】簇的几何上限是 `NANITE_MAX_CLUSTER_VERTICES = 128`
//   （顶点**表**的长度），但 mesh 的输出顶点是**逐图元角点**展开的（不做索引去重）：
//   64 个三角形 × 3 个角 = 192 个输出顶点。本机 `maxMeshOutputVertices = 256` 容得下；
//   `NaniteRaster::EnsureHardRasterResources` 会在建 PSO 前拿 `DeviceCaps` 逐项核对
//   （128 / 192 / 64），不够就**不建 PSO 也不录**（不做替代方案）。
//
// 【§14.3 依赖禁令】只依赖 `NaniteTypes.slang` 与既有 GBuffer 契约。
// ============================================================

#include "Nanite_SoftRasterCommon.slang"

// ── 与 C++ 侧 `NaniteTypes.h` 的常量一一对应（改一处必须改三处：C++ / 本文件 / 文档）──
//   · `kNaniteHardRasterThreads`       = 128（= 本机 maxMeshWorkGroupInvocations，N 不得超过它）
//   · `kNaniteHardRasterMaxVertices`   = 3 × kNaniteMaxClusterTriangles
//   · `kNaniteHardRasterMaxPrimitives` = kNaniteMaxClusterTriangles
//   · 读数槽位 `kNaniteHardStat*`（C++ 侧同名常量，见下面的 static const）
static const uint kHardNumThreads     = 128u;
static const uint kHardMaxVertices    = 192u;   // 3 × 64
static const uint kHardMaxPrimitives  = 64u;    // NANITE_MAX_CLUSTER_TRIANGLES

// 【任务 22 的硬光栅读数槽位】与 C++ 侧 `kNaniteHardStat*` 一一对应。
// 【为什么另开一个缓冲而不是复用软光栅的 16 槽】软光栅那 16 槽里 0..14 全部已占用，
//   只剩 1 个空槽；而"软硬占比"至少要三个量（接手簇数 / 输出图元数 / 写入像素数）。
//   复用会让两侧的槽位语义互相纠缠（判据 8a 直接读那 16 槽的字段名），故另开一个
//   模块自持的小缓冲（4 槽），每帧用命令缓冲内的 `CopyBuffer` 清成 0。
static const uint kHardStatClusters   = 0u;   // 本帧**真的交给硬光栅**的簇数（= 软光栅的 skipped_big）
static const uint kHardStatPrimitives = 1u;   // 这些簇**输出并被光栅化**的图元数（mesh 侧累加）
static const uint kHardStatPixels     = 2u;   // 硬光栅片元数（= 真正写进 4 张 GBuffer 的像素数）
// 槽 3 = `kHardStatFallbackPixels`（材质段越界/缺失的像素数，正常 0）：只有片元侧会写它。

// ── 【任务 26 / §14.34 表格第 10 行】push constant 回读槽（4..10；容量 4 → 11）──
// 【为什么必须有】§14.31 ⑦② 的故障模式是"`SetPushConstants` 漏了 mesh 阶段"，而在此之前
//   软光栅两趟有 `diag_*` 回读、**硬光栅一条都没有** ⇒ 这类故障落在 mesh 阶段只能靠猜。
//   现在 mesh shader 在**任何提前返回之前**把收到的 push constant 原样写回，CPU 侧把它们与
//   "自己送下去的值"逐项比对（见 `NaniteRaster::LogHardRasterReadback`）。
//   【与 C++ 的互锁】下面 7 个槽与 `NaniteTypes.h` 的 `kNaniteHardStatDiag*` 一一对应
//   （C++ 侧有 static_assert 钉住"紧跟槽 3 + 连续 + 吃满容量"；改一处必须同步另一处）。
static const uint kHardStatDiagScreenW   = 4u;   // push constant 的 screenWidth（原样回读）
static const uint kHardStatDiagScreenH   = 5u;   // push constant 的 screenHeight
static const uint kHardStatDiagMaxTri    = 6u;   // push constant 的 maxTriangles（分流阈值）
static const uint kHardStatDiagExtent    = 7u;   // meshMaxExtent × 1000（定点回读，验证浮点字段没错位）
static const uint kHardStatDiagInstances = 8u;   // push constant 的 instanceCount
static const uint kHardStatDiagMaterials = 9u;   // push constant 的 materialCount
static const uint kHardStatDiagPages     = 10u;  // push constant 的 pagesEnabled（流式档位）
static const uint kHardStatsCapacity     = 11u;  // 缓冲条数（与 C++ 侧一致）

// ── mesh → fragment 的输出顶点 ──
// 【为什么 position 保留**未除 w 的裁剪空间**（`clip`，而不是 `ndc`）】
//   软光栅的属性插值走 `softRasterAttribute`：`Σ(bary_k / w_k × attr_k) / Σ(bary_k / w_k)`，
//   也就是**透视校正**插值，权重里带着各顶点的 clip.w。硬件对 varyings 也做透视校正插值，
//   但它的权重来自插值出来的 1/w —— 只有当我们把**原始 clip.w** 交给硬件，两者才是同一份
//   权重。若这里把 w 归一成 1（`float4(ndc, 1)`），硬件的插值会退化成屏幕空间线性插值
//   ⇒ 同一批几何的两条路径会给出不同的 worldPos / uv，任务 22 的"画面一致"直接失效。
//   （深度不受影响：两种写法下 `z/w` 都是屏幕空间线性量。）
struct HardRasterVertexOut {
    float4 position     : SV_POSITION;   // 裁剪空间（未除 w）
    float3 worldPos     : TEXCOORD0;     // 世界坐标（= 网格量化解码 + 实例平移）
    float3 normal       : TEXCOORD1;     // 世界法线（未归一化，片元里 normalize）
    float2 uv           : TEXCOORD2;     // 反照率 UV
    // 【为什么这两个要 nointerpolation】材质下标与实例下标是**整数量**，插值会得到无意义的
    //   中间值（材质 0 与 1 之间没有"半个材质"）。一条簇内它们本来就是常量，故直接用
    //   `nointerpolation` 取 provoking vertex 的值。
    nointerpolation uint materialID     : TEXCOORD3;
    nointerpolation uint instanceIndex  : TEXCOORD4;
};

// 两个"没有图元"的收尾（`SetMeshOutputCounts(0,0)` 之后立即返回）。
// 【为什么必须由所有线程调用且参数一致】Vulkan 规范要求 `SetMeshOutputCounts` 在本工作组的
//   所有调用里都执行、且两个参数逐位一致（`Nanite_MeshTest.mesh.slang:48-50` 已写明）。
//   本函数只在**工作组一致**的条件上被调用（槽位/引用/簇记录/三角形数都只依赖 gid），
//   因此分支是 uniform 的，合法。
// 【为什么写成 `do { … } while (false)` 而不是 `while (0)`】Slang 对 `while (0)` 报
//   `E30081 implicit conversion from 'int' to 'bool' is not recommended`（6 次调用 = 6 条警告）。
//   写成 `false` 条件类型正确、展开语义一模一样（`if (cond) MACRO();` 仍是单条语句）。
#define HARD_RASTER_BAILOUT() do { SetMeshOutputCounts(0u, 0u); return; } while (false)

[numthreads(128, 1, 1)]
[outputtopology("triangle")]
void main(
    uint tid : SV_GroupIndex,
    uint3 gid : SV_GroupID,
    out vertices HardRasterVertexOut verts[kHardMaxVertices],
    out indices uint3 prims[kHardMaxPrimitives])
{
    // ── ⓪ 【任务 26 / §14.34 第 10 行】push constant 回读（必须**在任何提前返回之前**）──
    // 【为什么放这里而不是"画完再写"】本入口有多处 `HARD_RASTER_BAILOUT()` 早退（多派发的组、
    //   越界引用、空实例、缺页、小簇不归硬光栅…）。若把回读写在这些早退之后，最常见的档
    //   （`visibleCount == 0` 或全是小簇）就一个字节都写不进去 ⇒ 读回恒 0，回读判据空洞通过。
    //   放在最前面，由**恰好一个**线程（工作组 0 的 0 号线程）写一次，与软光栅第 1 趟的
    //   `kSoftStatDiag*` 完全同一手法（那里也是 `gid.x == 0 && tid.x == 0`）。
    //   【为什么用非原子写而不是原子累加】它是"原样回读"，语义是**赋值**而不是计数；
    //   多个工作组写同一个槽反而会让结果取决于执行顺序。只让 0 号线程写 ⇒ 无竞态。
    if (gid.x == 0u && tid == 0u) {
        u_Stats[kHardStatDiagScreenW]   = screenWidth;
        u_Stats[kHardStatDiagScreenH]   = screenHeight;
        u_Stats[kHardStatDiagMaxTri]    = maxTriangles;
        u_Stats[kHardStatDiagExtent]    = (uint)max(0.0, meshMaxExtent * 1000.0);
        u_Stats[kHardStatDiagInstances] = instanceCount;
        u_Stats[kHardStatDiagMaterials] = materialCount;
        u_Stats[kHardStatDiagPages]     = pagesEnabled;
    }

    // ── ① 取簇：与软光栅第 1/2 趟**逐字相同**的取簇逻辑（同一条可见簇列表 + 同一份资产）──
    uint visibleCount = u_VisibleClusterCount[0];
    uint slot = gid.x;
    if (slot >= visibleCount) HARD_RASTER_BAILOUT();          // 多派发的组（dispatch 按容量取整）

    ClusterRef ref = u_VisibleClusters[slot];
    if (ref.instance >= instanceCount) HARD_RASTER_BAILOUT();  // 越界引用：不读实例表（防御性）
    if (pagesEnabled != 0u) {
        // 【任务 24】开启档：页映射的下标空间是**资产**簇下标 ⇒ 按资产簇数收口
        //   （binding 0 这时绑的是页池，GETDIMENSIONS 的判据不适用，与软光栅两趟同一口径）
        if (ref.cluster >= assetClusterCount) HARD_RASTER_BAILOUT();
    } else {
        uint clusterCapacity = 0u;
        uint clusterStride = 0u;
        u_Clusters.GetDimensions(clusterCapacity, clusterStride);
        if (ref.cluster >= clusterCapacity) HARD_RASTER_BAILOUT();
    }

    InstanceObject instance = u_Instances[ref.instance];
    if (instance.indexCount == 0u) HARD_RASTER_BAILOUT();      // 空实例：与剔除链的规则一致
    float3 translation = instance.ltw3.xyz;

    // ── 【任务 24】页解析（与软光栅两趟逐字相同的口径）──
    //   缺页的簇在软光栅两趟里已被跳过（没有颜色也没有深度）⇒ 硬光栅也必须跳过它，
    //   否则它会在一片空白上画出一个"流式关闭档不会出现"的像素（两档就不再逐位可比）。
    SoftRasterClusterView view = softRasterResolveCluster(ref.cluster);
    if (!view.valid) HARD_RASTER_BAILOUT();
    NaniteCluster cluster = view.cluster;
    uint triangleCount = cluster.triangleCount;
    if (triangleCount == 0u) HARD_RASTER_BAILOUT();

    // ── ② 分流：只有 `triangleCount > maxTriangles` 的簇归硬光栅 ──
    // 【这就是 §5.2 的那一行判据，也是软光栅 `if (triangleCount > maxTriangles) return;` 的
    //   镜像】两侧用的是**同一个 push constant 字段**与**同一份 triangleCount** ⇒ 并集全覆盖、
    //   交集为空。判据的来源是软光栅的 `skipped_big`，这里的 `kHardStatClusters` 应当与它相等。
    if (triangleCount <= maxTriangles) HARD_RASTER_BAILOUT();
    // 防御：即使资产异常（triangleCount 超过簇上限），也不越过输出数组（本设备未启用
    // robustBufferAccess，但输出数组越界是 Vulkan 的未定义行为，必须自己钳）。
    if (triangleCount > kHardMaxPrimitives) triangleCount = kHardMaxPrimitives;

    // 逐**角点**展开：三角形 `t` 用输出顶点 `3t / 3t+1 / 3t+2`
    SetMeshOutputCounts(3u * triangleCount, triangleCount);

    // 读数：本簇真的被硬光栅接手 + 它输出了多少图元（只由 0 号线程记，避免重复计数）
    if (tid == 0u) {
        InterlockedAdd(u_Stats[kHardStatClusters], 1u);
        InterlockedAdd(u_Stats[kHardStatPrimitives], triangleCount);
    }

    // ── ③ 一个线程负责一个三角形（≤64 ⇒ 128 线程里的前 `triangleCount` 个干活）──
    // 【为什么不是"一顶点一线程"】输出顶点是逐角点展开的（192 个），而**是否有效**是逐三角形
    //   的性质（三个角点的 clip.w 都要 > 0）。按三角形分工可以让"有效性判据"只算一次，
    //   也避免三个线程各算一遍同样的条件。代价是线程利用率最多 50%（64/128）—— 本任务
    //   验收的是"分流真的发生、画面一致"，性能归任务 23。
    if (tid >= triangleCount) return;

    uint triLocal = tid;
    NanitePackedTriangle packed = u_Triangles[view.triangleBase + triLocal];
    uint3 local = naniteDecodeTriangle(packed);
    uint base = view.vertexBase;

    // ── ④ 解码三个顶点（与 `softRasterFetchTriangle` 逐字一致）──
    float3 world[3];
    float4 clip[3];
    bool valid = true;
    for (uint k = 0u; k < 3u; ++k) {
        uint li = (k == 0u) ? local.x : ((k == 1u) ? local.y : local.z);
        // 越界下标：软光栅把它当作"整个三角形无效"（`naniteIsValidClusterLocalVertexIndex`）
        if (!naniteIsValidClusterLocalVertexIndex(li)) { valid = false; break; }
        NaniteVertex v = u_Vertices[base + li];
        // 位置：簇内局部量化 + **本簇球心** + 网格最大范围（§14.19 硬约束①）+ 实例平移
        float3 meshPos = naniteDecodeVertexPosition(v, cluster, meshMaxExtent);
        world[k] = meshPos + translation;
        // 投影：与软光栅用**同一个** `softRasterProjectWorld`（4 次显式点积，不是矩阵乘法）
        // ⇒ 两条路径的顶点位置逐位同源，任务 22 的 A/B 相关系数才不会掉。
        clip[k] = softRasterProjectWorld(world[k]);
        // 跨越/落在相机平面后 ⇒ 丢弃（软光栅的同一条简单口径：不做近平面裁剪）
        if (!(clip[k].w > 1.0e-6)) { valid = false; break; }
        if (any(isnan(clip[k]))) { valid = false; break; }
    }

    // ── ⑤ 无效三角形 ⇒ 退化图元 ──
    // 【为什么用一个"深度在远平面之外"的公共顶点】硬件没有"丢掉这个图元"的接口：
    //   `SetMeshOutputCounts` 报的图元数是**统一值**，不能逐图元跳过（跳过就要在工作组内做
    //   前缀压缩，成本与复杂度都不划算）。替代做法是输出一个**保证不产生片元**的图元：
    //   三个顶点取同一个位置（面积恒为 0）且深度 2.0 > w=1（超出 [0,1] 深度范围被裁剪）。
    //   两道保险任一都足以让光栅器一个片元都不产生。
    if (!valid) {
        HardRasterVertexOut degenerate;
        degenerate.position    = float4(0.0, 0.0, 2.0, 1.0);
        degenerate.worldPos    = float3(0.0, 0.0, 0.0);
        degenerate.normal      = float3(0.0, 0.0, 1.0);
        degenerate.uv          = float2(0.0, 0.0);
        degenerate.materialID  = 0u;
        degenerate.instanceIndex = ref.instance;
        verts[3u * triLocal + 0u] = degenerate;
        verts[3u * triLocal + 1u] = degenerate;
        verts[3u * triLocal + 2u] = degenerate;
        prims[triLocal] = uint3(3u * triLocal + 0u, 3u * triLocal + 1u, 3u * triLocal + 2u);
        return;
    }

    // ── ⑥ 三个角点的属性（法线 / UV 按**簇内局部下标**取，与软光栅同一口径）──
    for (uint c = 0u; c < 3u; ++c) {
        uint li = (c == 0u) ? local.x : ((c == 1u) ? local.y : local.z);
        NaniteVertex v = u_Vertices[base + li];
        HardRasterVertexOut outVert;
        outVert.position      = clip[c];                       // 保留原始 w ⇒ 硬件做透视校正插值
        outVert.worldPos      = world[c];
        outVert.normal        = naniteDecodeNormal(v.packedNormal);
        outVert.uv            = naniteDecodeUV(v.packedUV);
        // 【任务 19 口径】材质下标来自簇记录（三角形多数票映射写下的 `materialID`），
        //   片元里用 `softRasterEvaluateMaterial` 求值 —— 与软光栅**同一个函数、同一份材质段**。
        outVert.materialID    = cluster.materialID;
        // objectIndex 分区：Nanite 段起点 + 实例下标（MRT7 的页号，与软光栅同一编码）
        outVert.instanceIndex = ref.instance;
        verts[3u * triLocal + c] = outVert;
    }

    // 图元索引（每个三角形只被一个线程写 ⇒ 无竞态）
    prims[triLocal] = uint3(3u * triLocal + 0u, 3u * triLocal + 1u, 3u * triLocal + 2u);
}

#undef HARD_RASTER_BAILOUT
