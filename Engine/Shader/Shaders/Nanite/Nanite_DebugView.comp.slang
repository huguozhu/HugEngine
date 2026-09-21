// ============================================================
// Nanite_DebugView.comp.slang — §14.8 任务 26：屏幕可视化（模块自建小目标的唯一写入者）
//
// 【这是哪一个任务】§14.8 任务 26 的验收明文点名四项可视化：**可见簇数 / 软硬光栅占比 /
//   LOD 层级 / BVH 深度**（§14.34 末尾最小范围第 1 条）。在此之前模块**没有任何屏幕可视化**：
//   所有结论都来自读数行与 A/B 转储对照，"模块到底画出了什么形状"一次都没被直接看过。
//
// 【落在哪里、怎么保证不污染既有画面】
//   · 目标 = 模块**自建**的 64×32 `R32_UINT` 小目标（8 KB），**不是**任何 GBuffer 附件
//     ⇒ 改不动可见画面（与任务 6 的 `m_MeshTestTarget` 同一做法：模块自持目标 + 读回）；
//   · 由 `NaniteSettings::debugView` 切换：**默认 0 = 关**（一个 pass、一个资源、一行日志都不产生）；
//   · 读回：dump 帧把目标 `CopyTextureToBuffer` 到 host，由
//     `NaniteRaster::LogDebugViewReadback` 打印"非零像素占比 / 不同取值个数 / 各面板统计"。
//
// 【一像素代表什么（必须一眼看清，否则可视化无法核对）】
//   屏幕被切成 `NANITE_DEBUG_VIEW_PANEL × NANITE_DEBUG_VIEW_PANEL`（32×32）个**矩形 tile**：
//     屏幕像素 (px,py) 落在 tile `(px × 32 / 屏幕宽, py × 32 / 屏幕高)`（整数除法）。
//   一个**可见簇**按它的**包围球球心**（资产空间球心 + 实例平移）投影到屏幕后，落在哪个 tile
//   就计入哪个 tile。球心投影到屏幕外（NDC 的 x 或 y 超出 [-1,1]）或在相机平面之后的簇
//   **不落在任何 tile 上**（如实丢弃 ⇒ 各面板之和可以小于可见簇数，读数的 `tiled < visible`
//   就是这批簇的数量；**不做钳制、不伪造**：钳到边缘会让热力图说谎）。
//   目标宽 64 = 两个 32 列**面板**：
//     · **面板 A**（列 [0,32)）= 该模式的主量；
//     · **面板 B**（列 [32,64)）= 该模式的辅助量（模式 2 = 硬簇数；模式 3/4 = 簇计数）。
//
// 【四种模式的编码（亮度 = 该量）】
//   1 `visible_cluster_count`：A = 该 tile 的**可见簇数**（原子累加），B 恒 0。
//   2 `raster_share`        ：A = 该 tile 里**软光栅簇数**（`triangleCount ≤ maxTriangles`），
//                             B = 该 tile 里**硬光栅簇数**（`> maxTriangles`）。
//                             【口径】按**簇数**算占比 —— 按像素的占比由 `hard_raster` 行的
//                             `hard_share_permille` 给出（分母不同，不混读）；分流判据用的是
//                             **同一个** `maxTriangles` 与**同一份**簇记录 ⇒ 与真实分流同源。
//   3 `lod_level`           ：A = 该 tile 内可见簇的 `lodLevel` **之和**（Phase 3 用的同一张
//                             LOD 元数据表），B = 该 tile 内的簇计数 ⇒ 均值 = A / B（CPU 侧算）。
//   4 `bvh_depth`           ：A = 该 tile 内可见簇的**BVH 节点深度最大值**（根 = 1；深度表由
//                             CPU 按 GPU 遍历的同一棵树逐叶展开后上传），B = 簇计数。
//                             【为什么用最大值】它直接回答"剔除链在这个 tile 上要走多深"；
//                             合法簇深度恒 ≥ 1 ⇒ A == 0 明确表示该 tile 无簇（B 也为 0 可互证）。
//
// 【清屏趟：为什么同一个入口有两种行为】存储图像上的原子累加必须从 0 开始（否则读回的
//   是跨帧累加的历史总和），而 RHI 没有"清一张存储图像"的命令（只有深度/模板的清）。
//   于是用**同一个 PSO** 先派发一次 `mode == 0` 的清屏趟（一维展平写 0），再派发累加趟：
//   两趟只差一个 push constant（`vkCmdPushConstants` 在命令缓冲里是**逐次录制**的，
//   不像描述符集那样"最后一次主机写对整段生效" ⇒ 同一条命令缓冲里的两次派发拿到各自的值；
//   本入口每帧只写**一次**描述符集，不存在那个陷阱）。
//
// 【§14.3 依赖禁令】只依赖 `NaniteTypes.slang` 的共享常量与 POD；不引用 GI/Lumen/GPUCulling，
//   也不 include `Nanite_SoftRasterCommon.slang`（本入口不读几何、不做等值复检，
//   自带最小的一组绑定 ⇒ 不必为它多绑 21 个描述符）。
// ============================================================

#include "NaniteTypes.slang"

// ── 与 `Nanite_SoftRasterCommon.slang` 的对应结构**逐字段一致**（同一条 128B 实例表契约）──
// 【为什么要在这里再写一份】本入口不 include 那份文件（它带 21 个绑定；见文件头）。
//   两份都只是 `NaniteInstanceGpuObject`（C++ 侧 128B，`NaniteTypes.h` 有 static_assert）的
//   镜像 ⇒ 三处一致同源：偏移 48 的平移列（`localToWorld[12..14]`）是全世界唯一用到的字段。
struct DebugViewClusterRef {
    uint instance;
    uint cluster;
};

struct DebugViewInstance {
    float4 ltw0;
    float4 ltw1;
    float4 ltw2;
    float4 ltw3;              // 偏移 48：xyz = 平移（列主序矩阵的第 4 列）
    float4 boundsMin;
    float4 boundsMax;
    uint   meshIndex;
    uint   materialIndex;
    uint   objectID;
    uint   visibilityFlags;
    uint   indexCount;
    uint   firstIndex;
    int    vertexOffset;
    uint   _pad;
};

// ── 绑定（set=0；顺序与 `NaniteRaster::EnsureDebugViewResources` 的布局逐条对应）──
//   0 = 可见簇引用（只读；`ClusterRef` 8B，与两条光栅路径**同一份**列表）
//   1 = 可见簇计数（只读；单个 uint，剔除链 Phase 3 的原子计数）
//   2 = 实例表（只读；128B 契约，只用平移列）
//   3 = 簇包围球（只读；16B/条，**按资产簇下标**索引 —— 与 BVH 遍历用的是同一张表）
//   4 = 每簇 LOD 元数据（只读；16B/条，Phase 3 的同一张表；只读 `lodLevel`）
//   5 = 每簇 BVH 节点深度（只读；u32/条，CPU 由同一棵树逐叶展开；根 = 1）
//   6 = 资产簇记录（只读；64B/条，只为取 `triangleCount` 做模式 2 的软/硬分类）
//       【为什么按**资产**下标索引】`ref.cluster` 就是资产簇下标；流式档位下页池是另一份
//       拷贝（0/1/2 号槽一槽两用），而**资产段仍然常驻** ⇒ 这里直接绑资产段，两种档位同一口径。
//   7 = 目标（读写；`RWTexture2D<uint>`，64×32）
[[vk::binding(0, 0)]] StructuredBuffer<DebugViewClusterRef> u_VisibleClusters;
[[vk::binding(1, 0)]] StructuredBuffer<uint>                 u_VisibleClusterCount;
[[vk::binding(2, 0)]] StructuredBuffer<DebugViewInstance>   u_Instances;
[[vk::binding(3, 0)]] StructuredBuffer<NaniteClusterSphere>  u_ClusterSpheres;
[[vk::binding(4, 0)]] StructuredBuffer<NaniteClusterLODInfo> u_LodInfo;
[[vk::binding(5, 0)]] StructuredBuffer<uint>                 u_BVHDepths;
[[vk::binding(6, 0)]] StructuredBuffer<NaniteCluster>        u_AssetClusters;
[[vk::binding(7, 0)]] [[vk::image_format("r32ui")]] RWTexture2D<uint> u_Target;

// ── push constant（96B；与 C++ 的 `NaniteDebugViewParams` 逐字段一致）──
[[vk::push_constant]] cbuffer NaniteDebugViewParams {
    float4 vpRow0;            // 偏移 0
    float4 vpRow1;            // 偏移 16
    float4 vpRow2;            // 偏移 32
    float4 vpRow3;            // 偏移 48
    uint   screenWidth;       // 偏移 64
    uint   screenHeight;      // 偏移 68
    uint   maxTriangles;      // 偏移 72：软/硬分流阈值（模式 2 用它分类）
    uint   instanceCount;     // 偏移 76：实例域上界（越界引用直接跳过）
    uint   mode;              // 偏移 80：1..4 = 四种模式；0 = 清屏趟（内部）
    uint   panelWidth;        // 偏移 84：一个面板的列数
    uint   panelHeight;       // 偏移 88：tile 行数
    uint   _pad;              // 偏移 92
};

// 与世界坐标 → 裁剪空间（与软光栅 `softRasterProjectWorld` **同一个表达式**：4 次显式点积）
float4 debugViewProject(float3 worldPos) {
    float4 hp = float4(worldPos, 1.0);
    return float4(dot(vpRow0, hp), dot(vpRow1, hp), dot(vpRow2, hp), dot(vpRow3, hp));
}

// 一个 tile 的**线性像素下标**（目标行主序：index = y × 宽 + x）
uint debugViewTileIndex(uint x, uint y) {
    return y * NANITE_DEBUG_VIEW_WIDTH + x;
}

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_GroupThreadID, uint3 gid : SV_GroupID) {
    // 本入口有**两种**展平方式，都用同一个一维编号：
    //   · 清屏趟：编号 = 目标像素下标（一维展平写 0）；
    //   · 累加趟：编号 = **可见簇列表槽位**（一个线程 = 一条可见簇引用）。
    uint flat = gid.x * NANITE_DEBUG_VIEW_THREADS + tid.x;

    // ── ⓪ 清屏趟（mode == 0；由 CPU 在同一个命令缓冲里先派发一次）──
    if (mode == NANITE_DEBUG_VIEW_OFF) {
        if (flat < NANITE_DEBUG_VIEW_WIDTH * NANITE_DEBUG_VIEW_HEIGHT) {
            u_Target[uint2(flat % NANITE_DEBUG_VIEW_WIDTH, flat / NANITE_DEBUG_VIEW_WIDTH)] = 0u;
        }
        return;
    }

    // ── ① 取簇：与两条光栅路径**逐字相同**的取簇口径（同一条可见簇列表 + 同一个早退规则）──
    uint visibleCount = u_VisibleClusterCount[0];
    uint slot = flat;
    if (slot >= visibleCount) return;               // 多派发的线程：直接返回

    DebugViewClusterRef ref = u_VisibleClusters[slot];
    if (ref.instance >= instanceCount) return;       // 越界引用：不读实例表（防御性）

    // ── ② 包围球 + 实例平移 ⇒ 世界球心（与 BVH 遍历/CPU 参考同一条构造式：纯平移）──
    uint sphereCount = 0u;
    uint sphereStride = 0u;
    u_ClusterSpheres.GetDimensions(sphereCount, sphereStride);
    if (ref.cluster >= sphereCount) return;          // 簇下标越界（损坏资产）：跳过并丢弃

    NaniteClusterSphere sphere = u_ClusterSpheres[ref.cluster];
    float3 worldCenter = sphere.center + u_Instances[ref.instance].ltw3.xyz;

    // ── ③ 投影 → 屏幕像素 → tile（与软光栅同一套 NDC→像素约定：负高度视口下 y 向下）──
    float4 clip = debugViewProject(worldCenter);
    if (!(clip.w > 1.0e-6)) return;                  // 落在相机平面后/跨越相机平面：丢弃
    float3 ndc = clip.xyz / clip.w;
    if (any(isnan(ndc))) return;
    // 【球心在屏幕外 ⇒ 不落在任何 tile 上】理由见文件头（不做钳制，宁可少计也不伪造分布）。
    if (ndc.x < -1.0 || ndc.x > 1.0 || ndc.y < -1.0 || ndc.y > 1.0) return;

    float2 pix = float2((0.5 + 0.5 * ndc.x) * (float)screenWidth,
                        (0.5 - 0.5 * ndc.y) * (float)screenHeight);
    // 整数除法把屏幕切成 tile；`min(..., panel-1)` 只用于吸收"恰好等于屏幕尺寸"的那一个边界值
    // （ndc == 1.0 ⇒ pix == 屏幕尺寸 ⇒ 商 == 面板边长），不是对屏幕外的钳制。
    uint tx = min((uint)pix.x * panelWidth / screenWidth, panelWidth - 1u);
    uint ty = min((uint)pix.y * panelHeight / screenHeight, panelHeight - 1u);
    uint idxA = debugViewTileIndex(tx, ty);
    uint idxB = debugViewTileIndex(NANITE_DEBUG_VIEW_PANEL_B_COLUMN + tx, ty);

    // ── ④ 按模式累加（面板 A = 主量、面板 B = 辅助量）──
    if (mode == NANITE_DEBUG_VIEW_VISIBLE_CLUSTERS) {
        // 模式 1：亮度 = 落在该 tile 的可见簇数
        InterlockedAdd(u_Target[uint2(tx, ty)], 1u);
    } else if (mode == NANITE_DEBUG_VIEW_RASTER_SHARE) {
        // 模式 2：A = 软光栅簇数、B = 硬光栅簇数（判据 = 与两条光栅路径同一个 `maxTriangles`）
        uint clusterCount = 0u;
        uint clusterStride = 0u;
        u_AssetClusters.GetDimensions(clusterCount, clusterStride);
        uint triangleCount = (ref.cluster < clusterCount) ? u_AssetClusters[ref.cluster].triangleCount
                                                          : 0u;
        if (triangleCount > maxTriangles) {
            InterlockedAdd(u_Target[uint2(NANITE_DEBUG_VIEW_PANEL_B_COLUMN + tx, ty)], 1u);
        } else {
            InterlockedAdd(u_Target[uint2(tx, ty)], 1u);
        }
    } else if (mode == NANITE_DEBUG_VIEW_LOD_LEVEL) {
        // 模式 3：A = lodLevel 之和（均值由 CPU 用 B 除出来）、B = 簇计数
        uint lodCount = 0u;
        uint lodStride = 0u;
        u_LodInfo.GetDimensions(lodCount, lodStride);
        uint lod = (ref.cluster < lodCount) ? u_LodInfo[ref.cluster].lodLevel : 0u;
        InterlockedAdd(u_Target[uint2(tx, ty)], lod);
        InterlockedAdd(u_Target[uint2(NANITE_DEBUG_VIEW_PANEL_B_COLUMN + tx, ty)], 1u);
    } else if (mode == NANITE_DEBUG_VIEW_BVH_DEPTH) {
        // 模式 4：A = BVH 节点深度最大值（根 = 1）、B = 簇计数
        uint depthCount = 0u;
        uint depthStride = 0u;
        u_BVHDepths.GetDimensions(depthCount, depthStride);
        // 【越界 ⇒ 记 0】被 `kNaniteMaxBVHClusters` 截断掉的簇不在深度表里；记 0 让它们
        // 在"最大值"口径下不参与（若记一个假的大值会凭空造出热点）。B 的计数照常 +1，
        // 于是"有多少簇落在表外"可以从 `A==0 且 B>0` 的 tile 数上看出来。
        uint depth = (ref.cluster < depthCount) ? u_BVHDepths[ref.cluster] : 0u;
        InterlockedMax(u_Target[uint2(tx, ty)], depth);
        InterlockedAdd(u_Target[uint2(NANITE_DEBUG_VIEW_PANEL_B_COLUMN + tx, ty)], 1u);
    }
    // 未知模式（cfg 已钳制，理论上到不了）：什么都不写 ⇒ 目标保持清屏后的全 0，
    // 读数会以"非零像素占比 = 0"如实暴露，而不是静默画出一张假图。
}
