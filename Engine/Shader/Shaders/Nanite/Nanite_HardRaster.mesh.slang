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
static const uint kHardMaxVertices    = 256u;   // 4 × 64（近平面裁剪后单三角形最多 4 顶点）
static const uint kHardMaxPrimitives  = 128u;   // 2 × 64（裁剪后单三角形最多 2 图元）

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
    /// 【T8 排查】簇号（全局簇下标）：`debugView==3` 时用它着色，
    ///   用来区分"是哪些簇在画"与"哪些区域一个簇都没有"。
    nointerpolation uint clusterIndex   : TEXCOORD5;
};

// 取簇与分流判据：返回本工作组要输出的三角形数，0 表示"没有图元"（多派发的组、越界引用、
// 空实例、缺页、小簇不归硬光栅…）。
// 【为什么拆成函数、只在入口调用一次 `SetMeshOutputCounts`】D3D12（DXIL 校验）要求
//   `SetMeshOutputCounts` 在整个入口里**只调用一次**，且这次调用支配所有输出写入；此前每个
//   早退点各调用一次 `SetMeshOutputCounts(0,0)`，Vulkan 接受但 dxc 报 "cannot be called
//   multiple times"。这里把所有早退判据收拢成返回值，入口按它统一调用一次。
//   所有判据只依赖 gid（工作组一致），满足 Vulkan "所有线程调用且参数一致" 的要求。
uint hardRasterSelectCluster(uint slot, out ClusterRef ref, out SoftRasterClusterView view,
                             out NaniteInstanceBasis basis)
{
    ref = (ClusterRef)0;
    view = (SoftRasterClusterView)0;
    basis = naniteInstanceBasisIdentity();

    uint visibleCount = u_VisibleClusterCount[0];
    if (slot >= visibleCount) return 0u;                    // 多派发的组（dispatch 按容量取整）

    ref = u_VisibleClusters[slot];
    if (ref.instance >= instanceCount) return 0u;           // 越界引用：不读实例表（防御性）
    if (pagesEnabled != 0u) {
        // 【任务 24】开启档：页映射的下标空间是**资产**簇下标 ⇒ 按资产簇数收口
        //   （binding 0 这时绑的是页池，GETDIMENSIONS 的判据不适用，与软光栅两趟同一口径）
        if (ref.cluster >= assetClusterCount) return 0u;
    } else {
        uint clusterCapacity = 0u;
        uint clusterStride = 0u;
        u_Clusters.GetDimensions(clusterCapacity, clusterStride);
        if (ref.cluster >= clusterCapacity) return 0u;
    }

    InstanceObject instance = u_Instances[ref.instance];
    if (instance.indexCount == 0u) return 0u;               // 空实例：与剔除链的规则一致
    // 【T8】完整实例基（旋转 + 缩放 + 平移）：只加平移会让旋转/非均匀缩放失效
    basis.cx = instance.ltw0.xyz;
    basis.cy = instance.ltw1.xyz;
    basis.cz = instance.ltw2.xyz;
    basis.t  = instance.ltw3.xyz;

    // ── 【任务 24】页解析（与软光栅两趟逐字相同的口径）──
    //   缺页的簇在软光栅两趟里已被跳过（没有颜色也没有深度）⇒ 硬光栅也必须跳过它，
    //   否则它会在一片空白上画出一个"流式关闭档不会出现"的像素（两档就不再逐位可比）。
    view = softRasterResolveCluster(ref.cluster);
    if (!view.valid) return 0u;
    uint triangleCount = view.cluster.triangleCount;
    if (triangleCount == 0u) return 0u;

    // ── 分流：只有 `triangleCount > maxTriangles` 的簇归硬光栅 ──
    // 【这就是 §5.2 的那一行判据，也是软光栅 `if (triangleCount > maxTriangles) return;` 的
    //   镜像】两侧用的是**同一个 push constant 字段**与**同一份 triangleCount** ⇒ 并集全覆盖、
    //   交集为空。判据的来源是软光栅的 `skipped_big`，这里的 `kHardStatClusters` 应当与它相等。
    if (triangleCount <= maxTriangles) return 0u;
    // 防御：即使资产异常（triangleCount 超过簇上限），也不越过输出数组（本设备未启用
    // robustBufferAccess，但输出数组越界是 Vulkan 的未定义行为，必须自己钳）。
    return min(triangleCount, kHardMaxPrimitives);
}

// 【A5】组内广播用的共享内存（模块作用域：Slang 不允许在函数体内声明 groupshared）
//   0 号线程算一次 → 全组 128 个线程读同一份，避免每线程各做一次取簇 + 页解析。
groupshared uint                  gsTriangleCount;
groupshared ClusterRef            gsRef;
groupshared SoftRasterClusterView gsView;
groupshared NaniteInstanceBasis     gsBasis;

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
    if (naniteRasterSlot(gid) == 0u && tid == 0u) {   // 只由第 0 个工作组的 0 号线程写
        u_Stats[kHardStatDiagScreenW]   = screenWidth;
        u_Stats[kHardStatDiagScreenH]   = screenHeight;
        u_Stats[kHardStatDiagMaxTri]    = maxTriangles;
        u_Stats[kHardStatDiagExtent]    = (uint)max(0.0, meshMaxExtent * 1000.0);
        u_Stats[kHardStatDiagInstances] = instanceCount;
        u_Stats[kHardStatDiagMaterials] = materialCount;
        u_Stats[kHardStatDiagPages]     = pagesEnabled;
    }

    // ── ①② 取簇 + 分流：与软光栅第 1/2 趟**逐字相同**的取簇逻辑（同一条可见簇列表 + 同一份资产）──
    // 【A5 修复：本组只算一次，用 groupshared 广播】`hardRasterSelectCluster` 的判据只依赖
    //   工作组（`naniteRasterSlot(gid)`、可见计数、簇记录），对 128 个线程**完全一致**。
    //   此前是让 128 个线程**各算一遍**：每线程都要读 `u_VisibleClusterCount[0]`（全局/ UAV）、
    //   可见引用、实例表、簇记录与页解析 —— 而派发维度是按可见引用**上界**取的
    //   （样例实测 530,368 个组 × 128 线程 = 6,800 万个线程 × 十余次全局读）。
    //   实测 07.Nanite 因此每帧 2.07 s（`nanite_pass_ms=2063`，硬光栅一趟占 99.9%），
    //   而真正需要处理的可见簇只有 23,851 条。改成"0 号线程算一次 + 组内同步"后，每组的
    //   取簇/页解析只发生一次。
    if (tid == 0u) {
        ClusterRef            ref0;
        SoftRasterClusterView view0;
        NaniteInstanceBasis   basis0;
        gsTriangleCount = hardRasterSelectCluster(naniteRasterSlot(gid), ref0, view0, basis0);
        gsRef           = ref0;
        gsView          = view0;
        gsBasis         = basis0;
    }
    GroupMemoryBarrierWithGroupSync();

    const uint                  triangleCount = gsTriangleCount;
    const ClusterRef            ref           = gsRef;
    const SoftRasterClusterView view          = gsView;
    const NaniteInstanceBasis   basis         = gsBasis;
    NaniteCluster cluster = view.cluster;

    // 逐**角点**展开：三角形 `t` 用输出顶点 `3t / 3t+1 / 3t+2`。整个入口只调用这一次
    //（triangleCount 为 0 时即"没有图元"，下面 `tid >= triangleCount` 对所有线程成立而返回）。
    // 【不要紧跟一个 `if (triangleCount == 0u) return;`】dxc 优化时会按该分支把本调用复制成
    //   两份（0 与非 0 各一份），DXIL 校验报 "cannot be called multiple times"
    // 【近平面裁剪】每个三角形最多产出 4 个顶点 / 2 个图元（三角形或四边形扇）
    SetMeshOutputCounts(4u * triangleCount, 2u * triangleCount);

    // 读数：本簇真的被硬光栅接手 + 它输出了多少图元（只由 0 号线程记，避免重复计数）
    if (tid == 0u && triangleCount != 0u) {
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

    // ── ④ 解码三个顶点（位置 + 属性一并取出，裁剪时要插值）──
    HardRasterVertexOut vtx[3];
    bool badTri = false;
    for (uint k = 0u; k < 3u; ++k) {
        uint li = (k == 0u) ? local.x : ((k == 1u) ? local.y : local.z);
        // 越界下标：整个三角形无效（与软光栅 `naniteIsValidClusterLocalVertexIndex` 同口径）
        if (!naniteIsValidClusterLocalVertexIndex(li)) { badTri = true; break; }
        NaniteVertex v = u_Vertices[base + li];
        // 位置：簇内局部量化 + 本簇球心 + 网格最大范围 + 实例变换
        float3 meshPos = naniteDecodeVertexPosition(
            v, cluster, naniteClusterExtent(view.index, meshMaxExtent));
        float3 wp = naniteApplyInstanceBasis(basis, meshPos);
        vtx[k].position      = softRasterProjectWorld(wp);   // 未除 w（硬件做透视校正）
        vtx[k].worldPos      = wp;
        vtx[k].normal        = naniteDecodeNormal(v.packedNormal);
        vtx[k].uv            = naniteDecodeUV(v.packedUV);
        vtx[k].materialID    = cluster.materialID;
        vtx[k].instanceIndex = ref.instance;
        vtx[k].clusterIndex  = ref.cluster;
        if (any(isnan(vtx[k].position))) badTri = true;
    }

    const uint vBase = 4u * triLocal;    // 本三角形的顶点槽起点（固定 4 个）
    const uint pBase = 2u * triLocal;    // 本三角形的图元槽起点（固定 2 个）

    // ── ⑤ 退化三角形：输出零面积图元（不产生片元）──
    if (badTri) {
        HardRasterVertexOut deg = (HardRasterVertexOut)0;
        deg.position      = float4(0.0, 0.0, 2.0, 1.0);   // 深度越界 + 面积 0，双保险
        deg.worldPos      = float3(0.0, 0.0, 0.0);
        deg.normal        = float3(0.0, 0.0, 1.0);
        deg.uv            = float2(0.0, 0.0);
        deg.materialID    = cluster.materialID;
        deg.instanceIndex = ref.instance;
        deg.clusterIndex  = ref.cluster;
        for (uint i = 0u; i < 4u; ++i) verts[vBase + i] = deg;
        prims[pBase + 0u] = uint3(vBase, vBase, vBase);
        prims[pBase + 1u] = uint3(vBase, vBase, vBase);
        return;
    }

    // ── ⑥ 近平面裁剪（裁剪空间 w > eps）──
    // 【为什么必须做】此前的做法是"任一顶点 w<=eps ⇒ 整三角形退化成零面积图元"，
    //   于是贴着相机的大三角形会整块消失 —— 这正是画面轮廓上那圈**细缝/缺块**的来源。
    //   正确的做法是在裁剪空间做多边形裁剪：三角形被近平面切成 3 顶点或 4 顶点，
    //   再以扇形三角化（最多 2 个图元）。
    const float kNearEps = 1.0e-6f;
    float d[3];
    for (uint k2 = 0u; k2 < 3u; ++k2) d[k2] = vtx[k2].position.w - kNearEps;

    HardRasterVertexOut poly[4];
    uint polyN = 0u;
    if (d[0] > 0.0f && d[1] > 0.0f && d[2] > 0.0f) {
        // 全部在前方：原样输出
        poly[0] = vtx[0]; poly[1] = vtx[1]; poly[2] = vtx[2];
        polyN = 3u;
    } else if (d[0] > 0.0f || d[1] > 0.0f || d[2] > 0.0f) {
        // 部分在前方：沿三条边收集"内部点 + 交点"（三角形最多 4 个顶点）
        for (uint i = 0u; i < 3u; ++i) {
            uint j = (i + 1u) % 3u;
            const float di = d[i];
            const float dj = d[j];
            if (di > 0.0f) {
                poly[polyN++] = vtx[i];
            }
            if ((di > 0.0f) != (dj > 0.0f)) {
                const float t = di / (di - dj);                 // d 在边上是线性的 ⇒ 交点参数
                HardRasterVertexOut mv;
                mv.position      = lerp(vtx[i].position,  vtx[j].position,  t);
                mv.worldPos      = lerp(vtx[i].worldPos,  vtx[j].worldPos,  t);
                mv.normal        = lerp(vtx[i].normal,    vtx[j].normal,    t);
                mv.uv            = lerp(vtx[i].uv,        vtx[j].uv,        t);
                mv.materialID    = cluster.materialID;
                mv.instanceIndex = ref.instance;
                mv.clusterIndex  = ref.cluster;
                poly[polyN++] = mv;
            }
        }
    }

    // ── ⑦ 输出：扇形三角化（polyN == 3 或 4；0 表示完全在相机背后）──
    if (polyN < 3u) {
        HardRasterVertexOut deg = (HardRasterVertexOut)0;
        deg.position      = float4(0.0, 0.0, 2.0, 1.0);
        deg.worldPos      = float3(0.0, 0.0, 0.0);
        deg.normal        = float3(0.0, 0.0, 1.0);
        deg.uv            = float2(0.0, 0.0);
        deg.materialID    = cluster.materialID;
        deg.instanceIndex = ref.instance;
        deg.clusterIndex  = ref.cluster;
        for (uint i2 = 0u; i2 < 4u; ++i2) verts[vBase + i2] = deg;
        prims[pBase + 0u] = uint3(vBase, vBase, vBase);
        prims[pBase + 1u] = uint3(vBase, vBase, vBase);
        return;
    }

    for (uint vi = 0u; vi < polyN; ++vi) verts[vBase + vi] = poly[vi];
    // 未使用的顶点槽写 poly[0]（避免读未初始化；这些槽不会被图元引用）
    for (uint vj = polyN; vj < 4u; ++vj) verts[vBase + vj] = poly[0];

    prims[pBase + 0u] = uint3(vBase + 0u, vBase + 1u, vBase + 2u);
    prims[pBase + 1u] = (polyN >= 4u) ? uint3(vBase + 0u, vBase + 2u, vBase + 3u)
                                      : uint3(vBase, vBase, vBase);   // 零面积占位
}
