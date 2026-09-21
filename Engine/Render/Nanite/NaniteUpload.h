#pragma once

// ============================================================
// Nanite/NaniteUpload.h — 离线簇切分（CPU 侧） + `.nanite` 资产 → GPU 缓冲
//
// 【本文件的多次填充】
//   · 任务 1（骨架）：上传类的生命周期桩（Initialize / Shutdown / OnResize / IsReady），
//     **不加载任何资产、不创建任何 GPU 缓冲**。
//   · 任务 8（本任务）：`BuildNaniteClusters()` —— 用 meshoptimizer v0.22 的
//     `meshopt_buildMeshlets` 把"位置 + 索引"切成 ≤64 三角形 / ≤128 顶点的簇
//     （§14.8 任务 8、设计 §4.1 L246-247），并**直接填进任务 7 定稿的 `NaniteClusterRecord`**。
//     这一段是**纯 CPU 的"资产准备侧"代码**，放进 `NaniteUpload.{h,cpp}` 正是 §14.3 的模块
//     边界（本任务不新增模块文件）。
//   · 任务 9（本任务）：`BuildNaniteClusterDAG()` —— LOD 链（逐级减半）+ DAG 去重（内容哈希）。
//   · 任务 10（本任务）：`PackNaniteClusters()` —— 量化（位置/法线/UV）+ 打包（索引/材质/段表）
//     成**最终 GPU 侧字节布局**，并把量化误差/段字节数做成实测读数。
//   · 任务 12：`.nanite` 资产读取 + 从 `MeshBatcher` 的合并几何**读一次** → 上传 GPU 缓冲。
//   · 任务 14：`BuildNaniteClusterBVH()` —— 按簇记录（`boundsCenterRadius`）构建 per-instance
//     cluster BVH（§5.1 Phase 2 的"构建"部分；遍历与 GPU 侧落在 `NaniteTypes.h` / `NaniteCull`）。
//
// 【为什么本头文件不再 include RHI（任务 8 的改动）】
//   簇切分必须 RHI-free 才能被单测直接调用（`Tests/TestNaniteBuilder.cpp`；单测目标只加
//   include 路径、**不链接 HugEngineRender**，见 `Tests/CMakeLists.txt`）。上传类只**持有**
//   `rhi::IRHIDevice*` 而从不调用它的成员（`NaniteUpload.cpp` 里也只有赋值与判空），因此这里
//   用**前置声明**即可，不需要完整类型 —— 于是 `NaniteUpload.h/.cpp` 整个翻译单元都是
//   RHI-free 的，可被单测目标直接编译。需要 RHI 的地方自己 include（如 `NaniteRenderer.h`）。
//
// 【§14.3 依赖禁令】模块内不得引用 `GI_*` / `Lumen*` / `GPUCulling` 的内部结构
//   （可借其 Hi-Z 纹理句柄与描述符写法）；不得依赖 `MeshBatcher` 的运行时状态
//   —— 它只当**一次性输入**：本文件的接口只收"几何快照"式参数（位置/索引 span），
//   不持有 `MeshBatcher*` 去每帧回读它的内部表。
// ============================================================

#include "Nanite/NaniteTypes.h"   // 任务 7 的 POD 与上限常量（NaniteClusterRecord / NanitePackedTriangle）

#include <span>    // 输入几何的只读视图（不拷贝调用方的数据）
#include <vector>  // 产物容器

namespace he::rhi { class IRHIDevice; }   // 前置声明：见上面"为什么不再 include RHI"

namespace he::render {

// ============================================================
// §14.8 任务 8：离线 cluster 切分（CPU 侧，RHI-free）
//
// 【目标】把网格切成簇：每簇 **≤64 三角形 / ≤128 顶点**。上限直接取任务 7 已定稿的
//   `kNaniteMaxClusterTriangles` = 64 与 `kNaniteMaxClusterVertices` = 128（不另立数字）。
//
// 【meshoptimizer v0.22 的真实接口与参数约束】（声明见
//   `Engine/External/meshoptimizer/src/meshoptimizer.h`，实现见同目录 `clusterizer.cpp`）
//   · `size_t meshopt_buildMeshlets(meshopt_Meshlet* meshlets, unsigned int* meshlet_vertices,
//        unsigned char* meshlet_triangles, const unsigned int* indices, size_t index_count,
//        const float* vertex_positions, size_t vertex_count, size_t vertex_positions_stride,
//        size_t max_vertices, size_t max_triangles, float cone_weight);`
//       —— `meshlet_vertices` 是 u32 数组（每簇的"簇内局部下标 → 网格顶点下标"），
//          `meshlet_triangles` 是 u8 数组（每个三角形 3 个**字节**局部下标）。
//          `meshopt_Meshlet` = { vertex_offset（u32 元素下标）, triangle_offset（**字节**下标，
//          每簇按 4B 对齐）, vertex_count, triangle_count }。
//       —— 参数约束（`clusterizer.cpp:542-550` / `:520-523` 的 assert）：
//            `index_count % 3 == 0`；`vertex_positions_stride ∈ [12, 256]` 且是 4 的倍数；
//            `max_vertices ∈ [3, 255]`（**255，不是 256**）；`max_triangles ∈ [1, 512]`
//            且**必须是 4 的倍数**（原因："ensures the caller will compute output space
//            properly as index data is 4b aligned"）；`cone_weight ∈ [0, 1]`。
//          我们按设计传 128 / 64：64 是 4 的倍数、128 ≤ 255，两条都满足。
//       —— 缓冲容量（官方最坏情况公式）：`max_meshlets =
//            meshopt_buildMeshletsBound(index_count, max_vertices, max_triangles)`；
//            `meshlet_vertices` 需要 `max_meshlets × max_vertices` 个 u32；
//            `meshlet_triangles` 需要 `max_meshlets × max_triangles × 3` 个 u8。
//   · `void meshopt_optimizeMeshlet(unsigned int* meshlet_vertices, unsigned char* meshlet_triangles,
//        size_t triangle_count, size_t vertex_count);`（v0.22 标 EXPERIMENTAL）
//       —— 在**每个簇自己的子区间内**原地重排，提升光栅局部性；只重排、不增删，故
//          `vertex_count / triangle_count` 不变（这正是不破坏上面局部下标语义的前提）。
//   · `meshopt_Bounds meshopt_computeMeshletBounds(const unsigned int* meshlet_vertices,
//        const unsigned char* meshlet_triangles, size_t triangle_count, const float* vertex_positions,
//        size_t vertex_count, size_t vertex_positions_stride);`
//       —— 返回 { center[3], radius, cone_apex[3], cone_axis[3], cone_cutoff, cone_axis_s8[3],
//          cone_cutoff_s8 }。**关键语义**：`cone_cutoff` 就是剔除测试
//          `dot(dir, cone_axis) >= cone_cutoff` 里的那个 cos（内部值 = sqrt(1 - cos²(法线锥半角))），
//          它**正是**任务 7 `NaniteConeAxisAngle::cosHalfAngle`（"cos(锥半角)"）要的量 ⇒ 直接落盘，
//          不需要任何角度换算（只有"锥不可用"的哨兵编码需要映射，见下）。
//
// 【字段落盘口径：哪些填真值、哪些明确留 0】
//   · `boundsCenterRadius`  ← meshopt 的 center/radius（任务 8 填真值）
//   · `cone`                ← meshopt 的 cone_axis / cone_cutoff（任务 8 填真值）。
//       meshopt 对"锥不可用"返回**零向量轴**：① 半锥 ≥ ~168° 的 trivial-accept
//       （`cone_cutoff = 1`）、② 簇内三角形全是零面积的 trivial-reject（返回全零 bounds）。
//       这两种情形一律按任务 7 的**无锥哨兵**编码（axis = 0、cosHalfAngle = -1），
//       并在 `noConeClusterCount` 里单列 —— 不伪造一个单位轴。
//   · `triangleOffset` / `triangleCount` / `vertexOffset` ← 任务 8 填真值。语义与
//       `NanitePackedTriangle` 的"簇内局部下标 + vertexOffset"契约严格配套：
//       `triangleOffset` 是**三角形下标**（× 8B = 索引段字节偏移），
//       `vertexOffset` 是**顶点下标**（× 16B = 顶点段字节偏移）。
//   · `materialID`           ← **任务 10/12 填**（本任务写 0：bindless 材质 ID 由材质解析决定）
//   · `maxParentLODError`    ← **任务 9/10 填**（本任务写 0：LOD/DAG 尚未生成）
//   · `childClusterOffset` / `childCount` ← **任务 9 填**（本任务写 0 = 叶子）
// ============================================================

/// 簇切分的统计读数（验收判据：每网格簇数 / 每簇三角形上限 / 无退化簇）
struct NaniteClusterBuildStats {
    u32 clusterCount           = 0;  ///< 簇数
    u32 triangleCount          = 0;  ///< 三角形总数（= Σ 每簇 triangleCount，应等于输入三角形数）
    u32 vertexCount            = 0;  ///< 顶点表条目数（= Σ 每簇局部顶点数，**含**跨簇重复）
    u32 maxClusterTriangles    = 0;  ///< 每簇三角形数的最大值（硬上限 = kNaniteMaxClusterTriangles = 64）
    u32 maxClusterVertices     = 0;  ///< 每簇局部顶点数的最大值（硬上限 = kNaniteMaxClusterVertices = 128）
    u32 degenerateClusterCount = 0;  ///< **退化簇**：三角形数为 0 或顶点数为 0 的簇（必须为 0）
    u32 noConeClusterCount     = 0;  ///< meshopt 给不出可用锥（cone_axis == 0）的簇数（见落盘口径）
};

/// 一次簇切分的产物（CPU 侧中间表示；**尚未量化** —— 量化/打包是任务 10）
///
/// 【顶点表语义】`vertexIndices` 是"每簇局部顶点 → 网格顶点"的拼接表：第 c 簇的第 v 个局部
///   顶点对应网格顶点 `vertexIndices[clusters[c].vertexOffset + v]`。跨簇**允许重复**：
///   这是"簇内局部下标 + vertexOffset"契约的直接代价（去重/共享是任务 9 的 DAG 议题）。
/// 【为什么另存 `clusterVertexCount`】任务 7 的 `NaniteClusterRecord`（64B）里**没有**每簇
///   顶点数字段（GPU 侧只需要 `vertexOffset` + 局部下标），所以每簇局部顶点数放在这条平行数组里；
///   它与 `clusters` 一一对应，长度必须相等。
struct NaniteClusterBuild {
    std::vector<NaniteClusterRecord>  clusters;           ///< 可直接落盘的簇记录（64B/条）
    std::vector<u32>                  clusterVertexCount; ///< 与 clusters 一一对应的局部顶点数
    std::vector<u32>                  vertexIndices;      ///< 每簇局部顶点 → 网格顶点下标
    std::vector<NanitePackedTriangle> triangles;          ///< 3×u16 簇内局部下标（8B/三角形）
    NaniteClusterBuildStats           stats;              ///< 统计读数（见上）

    /// 产物是否为空（空网格 = 合法输入，簇数 0）
    [[nodiscard]] bool Empty() const { return clusters.empty(); }
};

/// 用 `meshopt_buildMeshlets` 把网格切成 ≤64 tri / ≤128 vert 的簇（§14.8 任务 8）
///
/// 【输入】`positions` = 每顶点 3 个 float（x, y, z；紧密排布，即 stride = 12B）；
///          `indices`   = 三角形列表（u32，长度必须是 3 的倍数）。
/// 【输出】成功时整体写满 `outResult`（含统计）；**失败时不改写出参**（与任务 7 的校验函数同口径）。
/// 【空网格】`indices` 为空 ⇒ 返回 true 且产物为空（簇数 0、统计全 0）—— 合法输入，不是失败。
/// 【失败（返回 false，不抛、不崩、不越界读）】索引个数不是 3 的倍数；`positions` 个数不是 3 的
///          倍数；存在索引 ≥ 顶点数；有三角形但顶点数为 0；非空输入却切不出任何簇；
///          或 meshopt 给出的簇内局部下标超出"≤128 顶点"契约（防御性，正常不可达）。
/// 【确定性】同一输入两次调用逐位一致：meshopt 的切分是纯确定算法，本函数不含随机数、
///          不读时间、不并行、不依赖容器地址序。
[[nodiscard]] bool BuildNaniteClusters(std::span<const float> positions,
                                       std::span<const u32>   indices,
                                       NaniteClusterBuild&    outResult);

// ============================================================
// §14.8 任务 9：LOD 链（边折叠逐级减半） + DAG 去重（哈希共享）
//
// 【目标】在任务 8 的"网格 → 簇"之上再叠两件事（§4.1 L238-248）：
//   ① LOD 链：把上一级的簇组**合并回一张索引表**，用 `meshopt_simplify` 把三角形数**逐级减半**，
//      再对简化后的网格重新走任务 8 的切簇，得到 0/1/2/… 级（"边折叠，每级 ~50%"）。
//   ② DAG：对每簇算**内容哈希**，哈希相同者**共享同一份数据**，并把任务 8 留 0 的
//      `childClusterOffset` / `childCount` 填成"细级簇 → 粗级父簇"的链接、把
//      `maxParentLODError` 填成"切到父级 LOD 的误差阈值"。
//
// 【meshoptimizer v0.22 的简化接口（真实签名，核实自 `src/meshoptimizer.h:375` 与
//   `src/simplifier.cpp:1810-1824 / 1943-2053`）】
//   size_t meshopt_simplify(unsigned int* destination, const unsigned int* indices, size_t index_count,
//                           const float* vertex_positions, size_t vertex_count, size_t vertex_positions_stride,
//                           size_t target_index_count, float target_error, unsigned int options,
//                           float* result_error);
//   · 该符号在 v0.22 是 `MESHOPTIMIZER_API`（**不是** EXPERIMENTAL）⇒ 无需定义 EXPERIMENTAL 宏。
//   · `destination` 必须装得下**最坏情况 `index_count` 个索引**（不是 `target_index_count`）——
//     本实现按 `index_count` 申请缓冲。
//   · 参数约束（`simplifier.cpp:1814-1819` 的 assert）：`index_count % 3 == 0`；
//     `vertex_positions_stride ∈ [12,256]` 且是 `sizeof(float)` 的倍数；`target_index_count <= index_count`；
//     `target_error >= 0`；`options` 只能是 `meshopt_SimplifyX` 的位或（0 是安全默认）。
//   · 返回**简化后的索引个数**（恒为 3 的倍数：`remapIndexBuffer` 每次只删整三角形）；
//     受拓扑/误差限制时**可能达不到目标**（返回值 > target）⇒ 本实现据此终止 LOD 链。
//   · `result_error`（可空）是**相对**网格范围的线性误差（`simplifier.cpp:2049-2051`）；
//     取绝对误差必须再乘 `meshopt_simplifyScale(positions, vertexCount, stride)`（头文件 :419-424
//     写明了这条换算）。
//   · 选项传 `options = 0`（安全默认）。特别**不**传 `meshopt_SimplifyLockBorder`：锁住边界
//     （开放面片的边）会让平面/薄壳网格往往无法减半，直接掐死 LOD 链 —— "逐级减半"优先。
//     目标误差传 1.0（相对范围的 100%，即允许最大形变以求达到目标三角形数）。
//
// 【LOD 终止条件（三条，任一命中即停）】
//   ① 下一级目标三角形数 `< kNaniteMinLODTriangles`（= 一个满簇 64）⇒ 下一级连一个簇都凑不满，停；
//   ② `meshopt_simplify` 没能减少索引数（返回 0、或 `>= 上一级索引数`、或不是 3 的倍数）⇒ 停（防死循环）；
//   ③ 已达 `kNaniteMaxLODLevels` = 6（含 LOD0）⇒ 停（与 §4.1 的 `max_levels = 6` 对齐）。
//   例：2048 tri → 1024 → 512 → 256 → 128 → 64（6 级、5 次简化），下一级目标 32 < 64 即停。
//
// 【DAG 哈希口径（为什么是"簇内局部"而不是"绝对坐标"）】
//   · 每簇的内容 = **相对该簇 AABB 中心的量化局部几何 + 簇内局部三角形**：
//     位置词 = `NaniteQuantizePositionAxis(v_axis, 簇心_axis, 网格最大范围)` 三轴打包成
//     `NanitePackPosition`（R10G10B10A2，与落盘 `NaniteVertex::packedPosition` 同一位域）；
//     三角形 = 任务 7 的 `NanitePackTriangle`（3×u16 局部下标）。**世界位置不在簇内容里** ——
//     它由 `NaniteClusterRecord::boundsCenterRadius` 提供（与真实 Nanite 的"簇 bounds + 簇内局部顶点"
//     一致）：于是"平铺的相同子网格"（同一份几何的平移副本）哈希相同 ⇒ 被 DAG 真正共享。
//   · **量化原点为什么不是 meshopt 的最小包围球心**：`meshopt_computeMeshletBounds` 的球心是
//     增量最小包围球的浮点结果，**随簇内顶点顺序漂移**（实测：同一份 8×4 子网格的平移副本，
//     球心在 (4.00, 2.00) 与 (3.95, 2.30) 之间跳），于是平移副本会算出不同的位置词、明明相同
//     的簇却去重不了。任务 9 因此把簇记录的包围球改写为**簇 AABB 中心 + 到最远顶点的距离**：
//     AABB 的 min/max 与顶点顺序无关，整数平移下逐位可交换 ⇒ 平移副本给出**逐位相同**的位置词；
//     半径仍是包含本簇全部顶点的合法包围球（只是不再最小 ⇒ 剔除略保守，属可接受代价）。
//     `boundsCenterRadius.xyz` 因此既是剔除用的球心、也是量化/解码的原点（自包含，无需外部表）。
//   · 若改用**绝对坐标**（量化原点换成网格 bboxMin），则只有"完全重合的重复簇"才命中，
//     平移副本一律不去重 —— 这就是本任务选"簇内局部口径"的原因（两类网格的实测数字见实施记录）。
//   · 量化尺度取**整网格最大范围**（`meshopt_simplifyScale` 的口径），不是每簇各自的半径：
//     这样"同一形状被放大/缩小"不会被误判成同一个簇（尺度差异保留在位置词里）。
//   · 顶点顺序无关：先对局部顶点的位置词**排序**，再把每个三角形按"三种循环旋转里字典序最小"
//     规范化后排序，拼成规范键流；因此 meshopt 给出的簇内顶点/三角形顺序不同也不影响去重
//     （历史 Python 草案同样是 `tuple(sorted(...))` 口径）。哈希用 FNV-1a 64（逐 u32，
//     不依赖平台字节序与标准库 `hash`）；哈希命中后**再做一次规范键流全量比对**，
//     故不存在"哈希碰撞导致错误共享"。
//   · 共享的是"内容"这一份：`uniqueVertexWords` / `uniqueTriangles` 每个唯一内容只存一份；
//     每次出现（一条 `NaniteClusterRecord`）用 `vertexOffset` / `triangleOffset` 指过去；
//     放置信息（`boundsCenterRadius`）与"局部顶点 → 网格顶点"映射按**出现**各存一份
//     （`clusterVertexIndices`）。
//   · 这条口径对任务 10（量化打包）有约束力：顶点段必须按"共享内容"只落一份，
//     解码端用该簇自己的 `boundsCenterRadius` + 全局最大范围还原绝对坐标；否则共享的
//     `vertexOffset` 会让不同位置的簇读到同一份坐标。
//
// 【maxParentLODError 的来源】
//   · 级 L 的簇切到父级（级 L+1）的误差阈值 = **L → L+1 那次 `meshopt_simplify` 的绝对误差** =
//     `result_error（相对） × meshopt_simplifyScale()`；同一级内所有簇共用该值（简化是整网格一次做完，
//     不追踪单簇折叠祖先 ⇒ 这是该级的**保守上界**，不是逐簇紧致误差）。
//   · 若 meshopt 报 0（只剩"没删掉任何三角形"时才可能），退回**确定性几何兜底**：
//     `max(|child.center - parent.center| + child.radius)`（该级所有"子 → 父"配对的最大值），
//     它是"用父簇球替代子簇球"的距离上界。
//   · 最高一级（根）没有父级 ⇒ 写 0；根的 `childCount` 仍可 > 0（根有更细的孩子）。
// ============================================================

/// LOD 链的最大级数（**含** LOD0）：与 §4.1 的 `max_levels = 6` 对齐
inline constexpr u32 kNaniteMaxLODLevels = 6u;
/// LOD 终止阈值：给下一级的目标三角形数不得少于**一个满簇**（= 任务 7 的 64）
inline constexpr u32 kNaniteMinLODTriangles = kNaniteMaxClusterTriangles;
/// "没有父簇"哨兵（根簇；与 `kInvalidObjectIndex` 一样是**有意义**的返回值，不是随手越界值）
inline constexpr u32 kNaniteNoParentCluster = 0xFFFFFFFFu;

/// LOD + DAG 构建的统计读数（验收判据：LOD 层级 > 0、DAG 去重率、每簇上限、无退化簇）
struct NaniteClusterDAGStats {
    u32   levelCount           = 0;     ///< LOD 级数（**含** LOD0；空网格为 0）
    u32   simplifiedLevelCount = 0;     ///< **简化出来的级数** = levelCount - 1（验收："LOD 层级 > 0"）
    u32   totalClusterCount    = 0;     ///< 去重前的簇出现总数（= Σ 每级簇数）
    u32   uniqueClusterCount   = 0;     ///< 去重后（共享）的内容份数
    float dedupRate            = 0.0f;  ///< **去重率** = 1 - unique / total（total == 0 时为 0）
    u32   maxClusterTriangles  = 0;     ///< 每簇三角形数最大值（硬上限 = 64）
    u32   maxClusterVertices   = 0;     ///< 每簇局部顶点数最大值（硬上限 = 128）
    u32   degenerateClusterCount = 0;   ///< 退化簇数（三角形数 0 或顶点数 0；必须为 0）
    u32   noConeClusterCount   = 0;     ///< meshopt 给不出可用锥的簇数（口径同任务 8）
    u32   leafClusterCount     = 0;     ///< 叶子簇数（= LOD0 的簇数；没有孩子）
    u32   rootClusterCount     = 0;     ///< 根簇数（= 最高一级的簇数；没有父级）
    float maxLODError          = 0.0f;  ///< 所有级简化误差（绝对单位）的最大值
};

/// 一次 LOD + DAG 构建的产物（CPU 侧中间表示；**仍未量化落盘** —— 打包是任务 10）
///
/// 【两条平行视角】`unique*` 是**去重后的共享内容**（每个唯一内容一份）；
/// `clusters` 及其平行数组是**每一次簇出现**（级 L 的第 k 个簇）。一条出现记录用
/// `vertexOffset` / `triangleOffset` 指向共享内容，用 `boundsCenterRadius` 携带自己的世界放置。
/// 【为什么出现记录不能整条共享】内容（簇内局部形状 + 拓扑）相同的两个簇可以处在不同世界位置，
/// 共享的只能是内容；`vertexOffset` 指共享顶点段（语义同任务 7：顶点下标）、
/// `triangleOffset` 指共享三角形段（三角形下标）。
struct NaniteClusterDAG {
    // ── 去重后的共享内容（每个唯一簇一份）──
    std::vector<u32>                  uniqueVertexWords;      ///< 局部顶点位置词（R10G10B10A2，相对簇心）
    std::vector<NanitePackedTriangle> uniqueTriangles;        ///< 共享三角形表（局部下标，引用上面的顶点词）
    std::vector<u32>                  uniqueVertexOffset;     ///< 每唯一内容在 uniqueVertexWords 的起始
    std::vector<u32>                  uniqueVertexCount;      ///< 每唯一内容的局部顶点数
    std::vector<u32>                  uniqueTriangleOffset;   ///< 每唯一内容在 uniqueTriangles 的起始
    std::vector<u32>                  uniqueTriangleCount;    ///< 每唯一内容的三角形数

    // ── 每一次"簇出现"（= 落盘的一条 NaniteClusterRecord）──
    std::vector<NaniteClusterRecord>  clusters;               ///< 64B/条；vertexOffset/triangleOffset 指向共享内容
    std::vector<u32>                  clusterVertexCount;     ///< 与 clusters 一一对应的局部顶点数
    std::vector<u32>                  clusterVertexIndexOffset;  ///< 每次出现的局部顶点在 clusterVertexIndices 的起始
    std::vector<u32>                  clusterVertexIndices;   ///< 每次出现的"局部顶点 → 网格顶点"（放置数据）
    std::vector<u32>                  clusterLevel;           ///< 每次出现所属 LOD 级（0 = 最细）
    std::vector<u32>                  clusterUnique;          ///< 每次出现 → unique* 的下标

    // ── 每级视图（长度 = levelCount；levelClusterOffset 多一个收尾元素）──
    std::vector<u32>                  levelClusterOffset;     ///< 级 L 的出现区间 [offset[L], offset[L+1])
    std::vector<u32>                  levelClusterCount;      ///< 级 L 的簇数（去重前）
    std::vector<u32>                  levelTriangleCount;     ///< 级 L 的三角形总数（去重前；逐级减半判据）
    std::vector<u32>                  levelUniqueCount;       ///< 级 L 引用到的不同唯一内容数

    // ── DAG 链接（细级 → 粗级父簇）──
    std::vector<u32>                  childClusterIndices;    ///< 扁平子簇表（元素是 clusters 下标）
    std::vector<u32>                  parentCluster;          ///< 每次出现 → 父簇下标；根为 kNaniteNoParentCluster

    NaniteClusterDAGStats             stats;                  ///< 统计读数（见上）

    /// 产物是否为空（空网格 = 合法输入，级数 0）
    [[nodiscard]] bool Empty() const { return clusters.empty(); }
};

/// 在任务 8 的切簇之上生成 LOD 链（逐级减半）并做 DAG 去重（§14.8 任务 9）
///
/// 【输入】与 `BuildNaniteClusters` 完全同口径（positions = 紧密 xyz；indices = 三角形表），
///   另外可带**每网格顶点**的法线（3 float）与 UV（2 float）。
/// 【输出】成功时整体写满 `outResult`；**失败时不改写出参**（与任务 7/8 同口径）。
/// 【空网格】`indices` 为空 ⇒ 返回 true 且 `levelCount == 0`、产物为空（合法输入）。
/// 【失败】与 `BuildNaniteClusters` 相同的输入非法情形（索引不是 3 的倍数、越界索引、
///         有三角形却无顶点、某级切不出簇等），以及**属性长度不是顶点数的 3 倍/2 倍**。
/// 【确定性】同一输入两次调用逐位一致：不含随机数、不读时间、不并行；唯一的哈希表只做**查**、
///         不参与产物顺序，所有数组都按"级序 + 级内簇序"追加。
///
/// 【§14.8 任务 18 / P0：属性词进规范键流（修掉 §14.20⑥ 的属性错配）】
///   规范键流现在是
///   `[顶点数][排序的位置词][三角形数][排序的三角形键][属性标记][顶点数][逐局部下标的法线词/UV 词]`。
///   前三段是**顺序无关**的（平移副本、簇内顶点重排都能命中）；第四段按**局部下标有序**逐位写出。
///   【为什么必须有序】共享的顶点段只有一份、按"首次出现的局部下标顺序"存放，着色器按
///   `vertexOffset + 局部下标` 取属性 —— 若两个位置/拓扑同构但局部顺序不同的簇共享内容，
///   就会读到**别的顶点**的法线/UV（任务 9/10 实测：3×3 平铺 19 个出现里 8 个顺序不同、
///   属性冲突 1）。把属性词按局部下标纳入键流 ⇒ "共享 ⇒ 逐下标属性逐位相同"由哈希保证，
///   打包器的 `attributeConflictCount` 因此恒为 0（不再需要"如实报告冲突"这条退路）。
///   【代价（如实）】去重率下降：只有"位置/拓扑同构 **且** 逐下标属性逐位相同"才共享；
///   不传属性的调用方（`normals`/`uvs` 为空）不受影响 —— 那时属性词段是常量，等价关系与
///   任务 9 当时逐位相同。实测数字见 §14.27。
/// 【重载】三参数版本 = 五参数版本传入空属性 span（保持既有调用方与单测的语义不变）。
[[nodiscard]] bool BuildNaniteClusterDAG(std::span<const float> positions,
                                         std::span<const u32>   indices,
                                         NaniteClusterDAG&      outResult);

/// 【§14.8 任务 18】带属性（法线/UV）的 DAG 构建：只有全部属性逐位相同的簇才共享内容。
/// @param normals 每网格顶点 3 个 float（**可空** ⇒ 全部按默认 `(0,0,1)` 参与键流）；
/// @param uvs     每网格顶点 2 个 float（**可空** ⇒ 全部按默认 `(0,0)` 参与键流）。
[[nodiscard]] bool BuildNaniteClusterDAG(std::span<const float> positions,
                                         std::span<const float> normals,
                                         std::span<const float> uvs,
                                         std::span<const u32>   indices,
                                         NaniteClusterDAG&      outResult);

// ============================================================
// §14.8 任务 10：量化与打包（CPU 侧，RHI-free）
//
// 【目标】把任务 9 的 DAG 产物打成**最终 GPU 侧字节布局**（= 一份完整的 `.nanite` 文件镜像）：
//
// ```text
// [0]                  NaniteFileHeader        96B
// [+96]                NaniteClusterRecord[]   clusterCount  × 64B
// [..]                 NaniteVertex[]          vertexCount   × 16B
// [..]                 NanitePackedTriangle[]  triangleCount × 8B
// [..]                 NaniteMaterialRecord[]  materialCount × 8B
// [..]                 u32[]                   lodLevelCount × 4B
// ```
// 段偏移**不落盘**，全部由头部计数推导（任务 7 的 `TryBuildNaniteFileLayout`）：每段起点
// 16B 对齐、每段长度向上取整到 16B，总字节数是"记录数 × 记录大小"的纯函数。
//
// 【三条必须守住的口径】
//   ① **位置 = 簇内局部量化 + 该簇的 `boundsCenterRadius` + 网格最大范围**（§14.19 硬约束①）。
//      顶点段的 `packedPosition` 直接**复用 DAG 的 `uniqueVertexWords`**（任务 9 的共享内容就是
//      这么算出来的）：共享内容的词必须逐位相同，否则一个 `vertexOffset` 指向的同一份数据
//      没法服务不同位置的簇。打包器同时**重算一遍**这些词并与 DAG 的词逐位比对
//      （`positionMismatchCount`，必须 0），把"两处口径一致"变成可测读数。
//   ② **顶点段只落共享内容一份**（`uniqueVertexWords.size()` 条），每次"出现"的记录用
//      `vertexOffset` / `triangleOffset` 指过去 —— 它们与任务 9 的 `uniqueVertexOffset` /
//      `uniqueTriangleOffset` **天然同位**（都是各自段的记录下标）⇒ 打包不需要重映射任何偏移。
//   ③ **索引 = 3×u16 进 `u32[2]`**（任务 7 定稿），落盘时 `header.indexCount = 三角形数 × 3`，
//      索引段字节数 = `三角形数 × 8` 向上取整到 16B。
//
// 【法线 / UV 从哪来（任务 18 / P0 起：**属性已进 DAG 的内容键流**）】
//   本函数额外收 `normals` / `uvs`（每**网格顶点**的属性），按每个唯一内容的**首次出现**取
//   它的"局部顶点 → 网格顶点"映射去编码法线/UV。任务 9/10 期间这里有两条**已实测的已知限制**
//   （属性按局部下标关联会错配），**任务 18 的 P0 已修掉**：
//     ① 任务 9 的内容哈希只覆盖"位置 + 拓扑" ⇒ 位置相同但 UV/法线不同的簇会被共享；
//     ② 任务 8 的 `meshopt_optimizeMeshlet` 会按拓扑**就地重排**簇内顶点，而内容哈希顺序无关
//        ⇒ 两个"内容相同"的簇可以有**不同的簇内顶点顺序**，按局部下标取属性会读到别的顶点。
//   修法（改动最小、语义最直白）：**把法线/UV 词按局部下标一并写进 DAG 的规范键流**
//   （`BuildNaniteClusterDAG` 的五参数版本），于是"共享内容"的前提变成
//   **位置/拓扑同构 且 逐局部下标属性逐位相同**，两条限制在结构上都不再可能发生。
//   【实测（单测 MESSAGE 原文）】修复前：3×3 平铺（每片 8×4=64 tri）19 个出现里 8 个簇内
//   顶点顺序与首份不一致、`attributeConflictCount = 1`；修复后：`attributeConflictCount = 0`
//   （对"每片 UV 平移"的网格由 `dedupRate = 0` 证明这些簇**不再**共享）。
//   去重率的代价与前后对照见 §14.27（本文档的实施记录）。
//   **`normals`/`uvs` 为空的调用方不受影响**：属性词段退化为常量段，等价关系与任务 9 当时一致。
//
// 【打包器的属性一致性检查（现在是硬门，不是"如实报告"）】
//   打包器仍遍历每个唯一内容的**全部出现**、重算它们的法线/UV 词并与写进段里的比，
//   但期望值从"可能非 0"变成**必须为 0**（`attributeConflictCount`）；非 0 时本函数**返回 false**
//   （调用方拿到"没有资产"，而不是一份静默错配的资产）。它因此同时是
//   "P0 修复真的生效"的可测断言与最后一道防线：绕过五参数重载手工拼出来的 DAG
//   （只在单测/分析代码里可能）会在这里被拒。
//
// 【打包器**不**做的事（明确的边界）】
//   · 不建 GPU 缓冲、不读回、不碰任何 RHI 类型（GPU 上传是任务 12）；
//   · 不解析 `MeshBatcher`（输入是"几何快照"式 span，见 §14.3 的依赖禁令）；
//   · 不做真实的材质解析与逐簇材质分配（`materialID` 一律写 0，归属任务 12/19）。
//
// 【三处一致的达成程度（如实说明）】本任务只能保证**同一套常量与公式**落在 C++
//   （`NaniteTypes.h`）与 Slang（`Engine/Shader/Shaders/Nanite/NaniteTypes.slang`）两侧，并用
//   单测把 C++ 侧的量化误差/段布局/DAG 衔接钉住。真正的"pack/upload/shader 三处一致"要等
//   任务 12（上传后 GPU 缓冲字节数与 CPU 侧逐字节一致）与任务 18（shader 解码出同样的几何）
//   才能端到端验证。
// ============================================================

/// 打包的量化/布局**实测读数**（每条验收判据的一个数字都在这里；单测用 MESSAGE 打印原值）
struct NanitePackStats {
    // ── 段表（计数 × 记录大小，见 NaniteFileLayout）──
    u32   clusterCount  = 0;   ///< 簇段条数（= 任务 9 的"出现"总数）
    u32   vertexCount   = 0;   ///< 顶点段条数（= 任务 9 的**共享内容**顶点数，不是出现总和）
    u32   triangleCount = 0;   ///< 索引段条数（= 共享三角形数 = header.indexCount / 3）
    u32   materialCount = 0;   ///< 材质段条数
    u32   lodLevelCount = 0;   ///< LOD 段条数（= LOD 级数）

    usize headerBytes   = 0;   ///< 96B（`kNaniteFileHeaderBytes`）
    usize clusterBytes  = 0;   ///< 簇段字节（含 16B 对齐填充）
    usize vertexBytes   = 0;   ///< 顶点段字节（含填充）
    usize indexBytes    = 0;   ///< 索引段字节（含填充）
    usize materialBytes = 0;   ///< 材质段字节（含填充）
    usize lodBytes      = 0;   ///< LOD 段字节（含填充）
    usize rawBytes      = 0;   ///< 五段"记录数 × 记录大小"之和（**未**含对齐填充），与 totalBytes 对账
    usize totalBytes    = 0;   ///< = 96 + Σ alignUp(段原始字节,16)，见 `TryBuildNaniteFileLayout`

    float meshMin[3]    = { 0.0f, 0.0f, 0.0f };  ///< 量化范围下界（写进头部 `bboxMin`）
    float meshMax[3]    = { 0.0f, 0.0f, 0.0f };  ///< 量化范围上界（写进头部 `bboxMax`）
    float meshMaxExtent = 0.0f;                  ///< 量化尺度 = max(每轴范围)，与任务 9 同口径

    // ── 位置：无 clamp 证明 + 往返误差实测 ──
    u32   positionClampCount    = 0;  ///< 位置编码被 10 位范围夹住的分量数（必须 0，见头文件证明）
    u32   positionMismatchCount = 0;  ///< 与 DAG 位置词不一致的顶点数（必须 0：两处口径一致）
    float maxPositionError      = 0.0f;  ///< 位置往返误差实测最大值（绝对单位）
    float positionErrorBound    = 0.0f;  ///< 位置误差上界 = meshMaxExtent / 2044（半个量化步）

    // ── 法线：八面体角误差实测 ──
    float maxNormalAngleErrorDegrees = 0.0f;  ///< 法线角度误差实测最大值（度）
    float normalAngleErrorBoundDegrees = 0.0f;///< 法线角度误差阈值（单测取 0.5°）

    // ── UV：unorm16 ──
    float maxUVError   = 0.0f;  ///< UV 往返误差实测最大值（对 `[0,1]` 内的分量；越界分量单独计）
    float uvErrorBound = 0.0f;  ///< UV 误差上界 = 1/65535（一个量化步，宽松版）
    u32   uvClampCount = 0;     ///< 越出 `[0,1]` 被 clamp 的分量数（如实报告，测试网格实测 0）

    // ── DAG 衔接的如实读数 ──
    u32   attributeConflictCount = 0;  ///< 共享内容的不同出现给出不同 normal/UV 的唯一内容数（**必须 0**；非 0 ⇒ 打包返回 false）

    // ── 【§14.8 任务 19】簇 → 源网格 → 材质的映射读数（口径见本文件"簇 → 源网格 → 材质"小节）──
    u32   multiMeshClusters   = 0;  ///< 三角形跨越 **≥2 个源网格**的簇数（按三角形多数票归属，见下）
    u32   unmappedClusters    = 0;  ///< 一个三角形都投不出源网格的簇数（**防御：正常必须 0**；
                                    ///<   §14.8 任务 25 修复前实测 Sponza = 4103，修复后 = 0）
    u32   distinctMaterialCount = 0;///< 材质段里**内容各不相同**的记录数（按 32B 记录逐位比较）
    u32   texturedMaterialCount = 0;///< 其中带 BaseColor 纹理（`textureMask` bit0）的记录数
};

/// 一次打包的产物：**最终 GPU 侧字节布局** + 分段强类型视图 + 实测读数
///
/// 【`bytes` 与强类型分段的关系】`bytes` 是给 GPU 上传用的连续镜像（按 `layout` 拼好）；
/// 五个强类型容器是同一批数据的"分段视图"，让单测/调用方不必手算偏移。两者必须逐字节一致
/// —— 打包器在返回前用 `ValidateNaniteFile()` 自校验一次，不一致直接返回 false。
struct NanitePackedAsset {
    NaniteFileHeader header;                       ///< 96B 文件头（计数/范围/误差/flags 都填满）
    NaniteFileLayout layout;                       ///< 段表（由 header 推导，不落盘）
    std::vector<NaniteClusterRecord>  clusters;    ///< 簇段（每次"出现"一条，64B）
    std::vector<NaniteVertex>         vertices;    ///< 顶点段（每个唯一内容一份，16B）
    std::vector<NanitePackedTriangle> triangles;   ///< 索引段（3×u16，8B/三角形）
    std::vector<NaniteMaterialRecord> materials;   ///< 材质段（32B；任务 19 由 8B 最小扩展）
    std::vector<u32>                  lodOffsets;  ///< LOD 段（每级一个 u32，见 `PackNaniteClusters`）
    std::vector<u8>                   bytes;       ///< 上面五段按 `layout` 拼成的字节镜像
    NanitePackStats                   stats;       ///< 实测读数（见上）

    /// 产物是否为空（空 DAG = 合法输入：只有 96B 头部，计数全 0）
    [[nodiscard]] bool Empty() const { return clusters.empty(); }
};

// ============================================================
// 【§14.8 任务 19】簇 → 源网格 → 材质 的映射（RHI-free，可单测）
//
// 【为什么需要它】任务 12 的资产是"**整份合并几何当一个资产**"：簇是在合并后的顶点/索引段上
//   切出来的，簇记录里没有"我来自哪个原始网格"的信息，而材质是**逐网格**的（每个 MeshComponent
//   一份 PBRMaterial）。要把真实材质接进软光栅，必须先把每个簇的三角形区间映射回源网格。
//
// 【映射规则（确定、可复现；§14.8 任务 25 修复后的口径）】
//   · 源网格区间来自 `MeshBatcher` 的**逐网格绘制区间**（`GetDrawCommands()` 的
//     `firstIndex/indexCount`；索引已加过 baseVertex，见 `MeshBatcher.cpp:53`）⇒ 三角形区间
//     就是 `[firstIndex/3, (firstIndex+indexCount)/3)`，且它们**首尾相接、按构建顺序升序**。
//   · ⚠ **投票必须落在"原始合并三角形空间"**。`meshes[]` 描述的是**原始合并索引段**，而
//     `NaniteClusterRecord::triangleOffset` 描述的是任务 9 **去重后的共享三角形段**（它的值来自
//     `NaniteClusterDAG::uniqueTriangleOffset`）—— **两者不是同一个坐标系**：
//       ① 任务 8 的 `meshopt_buildMeshlets` 会按"簇内局部性"重排三角形（`:375` 的
//          `meshopt_optimizeMeshlet` 只重排簇内，但簇与簇之间的拼接顺序由 meshopt 决定）
//          ⇒ 即使 LOD0 的共享段顺序也与原始顺序不同；
//       ② 级 ≥1 的簇来自 `meshopt_simplify` 产出的**新三角形**，它们的共享段偏移整体落在
//          原始三角形表**之后**（`uniqueTriangles` 按级序追加）。
//     **实测（Sponza 合并几何：262267 原始三角形 / 103 个源网格 / 8287 个簇）**：
//       - 4103 个簇的 `triangleOffset ≥ 262267` ⇒ 按它投票**必然全部落空**（这 4103 个全部是级 ≥1）；
//       - LOD0 的 4099 个簇虽然落得进 `[0, 262267)`，但其中 **3957 个会选出错的源网格**；
//       - ⇒ 旧口径下 `unmappedClusters = 4103`（49.5%）**不是**"资产合法地存在未覆盖三角形"，
//         而是两个坐标系混用（§14.33 ⑥ 的根因假设方向正确、机制描述需按本节更正）。
//   · 修复后的投票口径 = **按三角形顶点的源网格归属**（归属表由"原始合并索引 + `meshes[]`
//     区间"推出，与 `meshes[]` 天然同一空间，且对**所有 LOD 级**都有定义）：
//       - 逐三角形：取它 3 个顶点中占多数的那个源网格（3 个顶点分属 3 个网格时取下标更小者）；
//         一个顶点都查不到归属的三角形算"未映射三角形"，**不投票**；
//       - 逐簇：取票数最大的源网格；**票数相同时取下标更小的网格**（确定性，不依赖容器遍历序）；
//         跨 ≥2 个网格的簇计入 `multiMeshClusters`（如实计数，不隐藏）；
//       - 一个三角形都没投出的簇 ⇒ 归属 0 号网格并计入 `unmappedClusters`
//         （**正常必须 0**：合并几何是连续拼接的，每个原始三角形的 3 个顶点都落在同一个源网格里）。
//   · 【为什么不去改 `triangleOffset` 的语义】它同时是"共享三角形段下标"，被页划分
//     （`NanitePagePlan` 的 `triangleOffset × 3`，见 `NaniteTypes.h`）与 DAG 引用使用；
//     改它等于改资产格式与下游全部换算 ⇒ 本修复改为**局部增加一张"顶点 → 源网格"归属表**。
//
// 【为什么放在 `NaniteUpload.h`】与任务 9/10/14 的构建器同一层（"资产 → 派生数据"），
//   且本翻译单元已被单测直接编译（`Tests/CMakeLists.txt`），映射规则因此天然可测。
// ============================================================

/// 一个源网格在**合并索引段**里的三角形区间 + 它的材质段下标
struct NaniteSourceMeshRange {
    u32 firstTriangle = 0;    ///< 首个三角形的下标（= `MeshBatcher` 的 `firstIndex / 3`）
    u32 triangleCount = 0;    ///< 三角形数（= `indexCount / 3`）
    u32 materialIndex = 0;    ///< 该网格用的材质记录下标（调用方按"一网格一条记录"填 i 即可）
};

/// 映射的如实读数（写进 `NanitePackStats` 的同名字段）
struct NaniteClusterMaterialMapStats {
    u32 multiMeshClusters = 0;   ///< 三角形跨越 ≥2 个源网格的簇数（多数票归属）
    u32 unmappedClusters  = 0;   ///< 一个三角形都投不出源网格的簇数（**正常必须 0**；口径见上）
};

/// 【§14.8 任务 25 缺陷修复】按"**三角形顶点的源网格归属**"把每个簇映射到材质段下标。
///
/// 【为什么本函数**不读** `triangleOffset`（务必看清）】那个字段是**去重后共享三角形段**的下标，
///   与 `meshes[]` 的"原始合并三角形空间"不是同一个坐标系（见上：`meshopt_buildMeshlets` 会按簇内
///   局部性重排三角形，级 ≥1 的共享段又是整体追加在原始表**之后**）⇒ 拿它去查 `meshes[]` 会大面积
///   落空，还会让"落得进区间"的簇静默选错源网格。本函数改用
///   `dag` 里"每次出现 → 网格顶点"的映射（`clusterVertexIndexOffset` / `clusterVertexIndices`）
///   把簇的三角形还原成**原始合并顶点下标**，再用归属表定源网格。
///   ⇒ 因此它对**任何 LOD 级**、**任何去重状态**都成立，这正是资产构建器该用的口径。
///
/// @param indices      **原始合并**索引段（u32 三角形列表；长度必须是 3 的倍数）
/// @param vertexCount  **原始合并**顶点数（= 顶点归属表的长度）
/// @param meshes       **原始合并**三角形区间表（升序、首尾相接、覆盖 `[0, 三角形数)`）
/// @param dag          `BuildNaniteClusterDAG()` 的产物（本函数不修改它）
/// @param outClusterMaterialIndex 输出数组；长度必须 ≥ `dag.clusters.size()`
/// @return 映射读数（见上）
/// 【防御】输入自相矛盾（输出数组过短、簇的共享内容下标/顶点区间越界、`indices` 越界、
///   `meshes` 区间越出索引表）时**不越界读**：该簇按"未映射"处理（计入 `unmappedClusters`）。
/// 【确定性】不含哈希容器遍历序、不含随机数、不并行；平票一律取下标更小的网格 ⇒ 逐位可复现。
[[nodiscard]] NaniteClusterMaterialMapStats NaniteAssignClusterMaterialsByVertexOwner(
    std::span<const u32>                   indices,
    u32                                    vertexCount,
    std::span<const NaniteSourceMeshRange> meshes,
    const NaniteClusterDAG&                dag,
    std::span<u32>                         outClusterMaterialIndex);

// ============================================================
// 【§14.8 任务 25】Material Bin —— 只读的"按材质分组的簇下标"辅助数组
//
// 【它是什么、不是什么（先划清边界，因为本任务的验收就是"不动任何既有东西"）】
//   · 它是一份 **`u32[clusterCount]` 的排列**：元素是簇记录的下标，按该簇的 `materialID`
//     非降序排列（同材质的簇在数组里**连续**）。这就是 §5.4 说的 "按材质分组" 的数据形态。
//   · 它**不动**任何既有数据：资产本体（`clusters` / `vertices` / `triangles` / `materials` /
//     `lodOffsets` / `bytes`）、BVH、DAG 引用、页池/页表、`.nanite` 格式**一个字节都不改**；
//     本函数是**纯函数**（收 `span`、写调用方给的输出缓冲），不持有也不修改任何输入。
//   · 它**不**改变光栅的遍历顺序（本任务的"明确不做"，理由见 `NaniteSettings::materialBin`）：
//     光栅端仍然按可见簇列表的顺序处理簇，bin 只被**读数**用来算"若按 bin 顺序遍历会怎样"。
//
// 【为什么用计数排序（而不是 `std::sort` 一个比较器）】
//   ① 与仓库既有口径一致：`BuildNanitePagePlan` 等处也是"排序 + 线性扫描"，全程与容器遍历序无关；
//   ② O(n + 材质数) 且**稳定**：同材质的簇保持它们原本的相对顺序 ⇒ `bins[]` 在"同材质簇之间的
//      先后"这一层是完全确定的（两条独立运行逐位一致），可以直接被单测按字节比较；
//   ③ `materialID` 是"材质段下标"（值域小且在 `materials.size()` 以内），直方图分箱天然合适。
//
// 【材质序 = 材质段下标的升序】bin 只关心"同材质相邻"，材质之间本身没有顺序要求；取下标升序是
//   最直白的确定性选择（不引入任何"按名称/按指针/按哈希"这类不可复现的排序键）。
// ============================================================

/// 材质 bin 的构建读数（如实报告，不隐藏异常输入）
struct NaniteMaterialBinStats {
    u32 clusterCount  = 0u;  ///< 参与分组的簇数（= `clusters.size()`）
    u32 distinctCount = 0u;  ///< 实际出现的不同 `materialID` 个数（= 非空的直方图桶数）
    u32 materialCount = 0u;  ///< 材质段的条数（= `materialCount` 入参，直方图的桶数）
    /// `materialID` 超出 `[0, materialCount)` 的簇数（**防御：正常必须 0**）。
    /// 【为什么单列而不静默 clamp】越界 ID 一旦被悄悄夹到某个桶里，bin 就会"看起来正确"而实际
    ///   把簇分到了错的材质组；更糟的是直方图必须按 `materialCount` 分配 ⇒ 不检查就会越界写。
    ///   非 0 时本函数**返回 false**（调用方拿到"没有 bin"，而不是一份静默错分的 bin）。
    u32 outOfRangeCount = 0u;
};

/// 一次材质分组的产物（**只读取的辅助数组**；不是资产的一部分、不进 `.nanite`）
struct NaniteMaterialBin {
    /// 按材质非降序排列的簇下标（长度 == `clusterCount`；是 `0..clusterCount-1` 的一个排列）
    std::vector<u32> bins;
    NaniteMaterialBinStats stats;   ///< 构建读数（见上）

    /// 产物是否为空（没有簇 ⇒ 合法输入，数组为空）
    [[nodiscard]] bool Empty() const { return bins.empty(); }
};

/// 按"簇的 `materialID`"把簇下标分组排序（§14.8 任务 25；纯函数、RHI-free）
///
/// @param clusters      资产簇段（每次出现一条 64B 记录；只读 `materialID`）
/// @param materialCount 材质段条数（= `NanitePackedAsset::header.materialCount`；`materialID`
///                      的合法值域是 `[0, materialCount)`）
/// @param outBins       **可选**输出：按材质分组后的簇下标（长度必须 ≥ `clusters.size()`）。
///                      传空 span ⇒ 只算读数、不写任何东西（用于"只要统计"的调用方）。
/// @return 构建读数；`materialID` 越界 ⇒ 返回的 `bins` 为空且 `outOfRangeCount > 0`
///         （**不改写出参**：先算完、校验通过后才写，与任务 7/8/9/10 的失败口径一致）。
/// 【空输入】`clusters` 为空 ⇒ 返回空读数，合法（与其它构建器同口径）。
/// 【确定性】只有一次计数直方图 + 一次前缀和 + 一趟稳定放置；不排序比较器、不含哈希容器遍历序、
///   不含随机数、不并行 ⇒ 同一输入两次调用逐位一致。
[[nodiscard]] NaniteMaterialBinStats BuildNaniteMaterialBin(
    std::span<const NaniteClusterRecord> clusters,
    u32                                  materialCount,
    std::span<u32>                       outBins);

/// 覆盖式重载：直接把 bin 建进 `outResult`（`outResult.bins` 被整体重写）
[[nodiscard]] NaniteMaterialBinStats BuildNaniteMaterialBin(
    std::span<const NaniteClusterRecord> clusters,
    u32                                  materialCount,
    NaniteMaterialBin&                   outResult);

/// 把任务 9 的 DAG 产物量化/打包成最终 GPU 侧字节布局（§14.8 任务 10）
///
/// 【输入】
///   · `positions`：每顶点 3 个 float（与 `BuildNaniteClusterDAG` **同一份**数据；用来重算
///     网格 AABB/最大范围并核验 DAG 的位置词——打包器不复用调用方的原始浮点，只复用词）；
///   · `normals`：每顶点 3 个 float；**可为空** ⇒ 全部按默认 `(0,0,1)` 编码；
///   · `uvs`：每顶点 2 个 float；**可为空** ⇒ 全部按 `(0,0)` 编码；
///   · `materials`：材质记录（可空 ⇒ `materialCount = 0`）；本函数**原样搬运**，不解析 ID；
///   · `dag`：`BuildNaniteClusterDAG()` 的产物（本函数不修改它）。
/// 【输出】成功时整体写满 `outResult`（含 `bytes` 字节镜像与统计）；**失败时不改写出参**
///   （与任务 7/8/9 同口径：先在本地构建，成功后再整体交出）。
/// 【空 DAG】`dag.clusters` 为空 ⇒ 返回 true，产出一份"只有 96B 头部、计数全 0"的合法资产。
/// 【失败（返回 false，不抛、不崩、不越界读）】`positions` 不是 3 的倍数或与 DAG 顶点数不符；
///   `normals`/`uvs` 非空但长度不是顶点数的 3 倍 / 2 倍；DAG 内部不一致（出现记录的偏移/计数
///   越出共享表、每簇局部顶点数为 0、`clusterUnique` 越界）；索引段里出现 ≥128 的簇内局部
///   下标（违反"每簇 ≤128 顶点"的 u16 打包前提）；或最后自校验不过。
/// 【确定性】同一输入两次调用逐位一致：不含随机数、不读时间、不并行、不用哈希容器遍历序；
///   "首次出现"一律取最小的出现下标。
[[nodiscard]] bool PackNaniteClusters(std::span<const float>                  positions,
                                      std::span<const float>                  normals,
                                      std::span<const float>                  uvs,
                                      std::span<const NaniteMaterialRecord>   materials,
                                      const NaniteClusterDAG&                 dag,
                                      NanitePackedAsset&                      outResult);

// ============================================================
// §14.8 任务 12：资产加载（合并几何快照 → `.nanite` 字节镜像，RHI-free）
//
// 【本函数存在的理由】任务 12 的输入是 `MeshBatcher` 的**合并几何**（§14.8 任务 12、
//   §14.3 的依赖禁令：只当**一次性输入**）。把"快照 → 字节镜像"这一整步收在一个
//   RHI-free 的函数里，是为了：
//     ① 让 `NaniteRenderer`（唯一持有设备的一侧）只做"取几何快照 + 转 SoA + 交给 Scene 上传"
//        三件事，不在门面里散落三段调用顺序；
//     ② 这一步能被单测直接覆盖（`HugEngineTests` 直接编译本翻译单元，不需要 GPU）；
//     ③ 保持 `MeshBatcher` 只出现在调用方的 `.cpp` 里（本文件不 include 它的头）。
//
// 【输入口径】与 `BuildNaniteClusterDAG` + `PackNaniteClusters` 完全一致：
//   · `positions` / `normals` / `uvs` 是**扁平 SoA**（每顶点 3 / 3 / 2 个 float），
//     调用方负责把 `StaticVertex[]` 转成这三条数组（合批索引已是绝对索引，不需要再加偏移）；
//   · `indices` 是三角形列表（u32，长度是 3 的倍数）；
//   · `materials` 可为空（⇒ `materialCount = 0`）：本函数**原样搬运**记录，不解释字段；
//     【任务 19】新增重载额外接收 `meshes`（逐源网格的三角形区间 + 材质下标），
//     它会把每个簇的 `materialID` 按"三角形多数票"写成**材质段下标**（映射规则见上），
//     于是软光栅能从资产材质段取到真实材质（旧重载保持"materialID 一律 0"的语义不变）。
//     【任务 25 修复】投票走 `NaniteAssignClusterMaterialsByVertexOwner`（顶点归属空间，
//     与 `meshes[]` 同一坐标系）—— **不能**拿簇记录的 `triangleOffset` 去查 `meshes[]`：
//     那是去重后共享三角形段的下标，实测 Sponza 有 4103 个簇落空、另有 3957 个 LOD0 簇选错网格。
// 【失败】与两个被调函数同口径：输入非法（索引不是 3 的倍数、越界索引、位置/属性长度不符、
//   DAG 内部不一致、切不出簇等）⇒ 返回 false 且**不改写** `outResult`。
// 【空几何】`indices` 为空 ⇒ 返回 true，产出一份"只有 96B 头部、计数全 0"的合法资产
//   （上层据此跳过 GPU 上传，见 `NaniteRenderer::EnsureAssetUploaded`）。
// 【确定性】同一输入两次调用逐位一致（两个被调函数各自都是确定性的，本函数不加任何状态）。
// ============================================================
[[nodiscard]] bool BuildNaniteAssetFromGeometry(std::span<const float>                positions,
                                                std::span<const float>                normals,
                                                std::span<const float>                uvs,
                                                std::span<const u32>                  indices,
                                                std::span<const NaniteMaterialRecord> materials,
                                                NanitePackedAsset&                    outResult);

/// 【§14.8 任务 19】带"簇 → 源网格 → 材质"映射的资产构建重载。
///
/// 【与上一个重载的唯一差别】额外接收 `meshes`：构建完成后按
///   `NaniteAssignClusterMaterialsByVertexOwner`（§14.8 任务 25 的修复口径）把每个簇记录的
///   `materialID` 写成材质段下标，并把结果（`multiMeshClusters` / `unmappedClusters` /
///   `distinctMaterialCount` / `texturedMaterialCount`）写进 `outResult.stats`；随后
///   **重新序列化簇段并再跑一次 `ValidateNaniteFile`** —— 字节镜像与强类型分段视图必须仍然
///   逐字节一致（这条自校验是"改记录就得改镜像"的硬门）。
/// 【`meshes` 为空】等价于旧重载（`materialID` 保持打包器写入的默认值 0）。
[[nodiscard]] bool BuildNaniteAssetFromGeometry(std::span<const float>                  positions,
                                                std::span<const float>                  normals,
                                                std::span<const float>                  uvs,
                                                std::span<const u32>                    indices,
                                                std::span<const NaniteMaterialRecord>   materials,
                                                std::span<const NaniteSourceMeshRange>  meshes,
                                                NanitePackedAsset&                      outResult);

// ============================================================
// §14.8 任务 14：per-instance cluster BVH 的**构建**（CPU 侧，RHI-free）
//
// 【放在这里的理由（两处位置的取舍，见 `NaniteTypes.h` 任务 14 小节的对称说明）】
//   · 它消费的是 `.nanite` 的**簇记录**（任务 9/10 的产物 `NaniteClusterRecord`），属于
//     "资产 → 加速结构"的构建阶段，与 `PackNaniteClusters` 同一层；
//   · `NaniteUpload.{h,cpp}` 已被 `Tests/TestNaniteBuilder.cpp` 直接编译进单测目标
//     （`Tests/CMakeLists.txt:50-54` 的纪律钉子），构建器因此天然可单测；
//   · POD 布局（`NaniteBVHNode` / `NaniteClusterSphere`）与 **CPU 参考遍历**
//     （`NaniteTraverseClusterBVHCPU`）留在 `NaniteTypes.h`：前者要与 Slang 共享、
//     后者是"GPU 与 CPU 逐项一致"的参考实现，与任务 13 的 `NaniteCullInstancesCPU` 同构。
//
// 【分裂策略：最长轴中点分裂 + 数量中位数回退，叶子容量 4】
//   · **轴向**：取该结点内全部簇球的**质心**在 x/y/z 上的跨度（max-min），沿最大的一轴分裂。
//     这是标准 BVH 启发式：沿最长轴分裂最可能把体积真正分开。
//   · **切点**：取该轴上质心范围的**中点**（空间中点），把质心 < 中点的簇放左边。
//     为什么不用 SAH：SAH 要对每个候选分裂算面积代价（或做分桶），既有浮点分箱又有
//     "桶边界 vs 精确坐标"的对比，而本任务的验收是**可复现**与 CPU/GPU 逐项一致；
//     中点分裂只有"一次排序 + 一次扫描"，确定性与可解释性都更强，且沿分裂轴产生
//     **互不重叠**的孩子体积 —— 对"节点不可见 ⇒ 整棵子树跳过"的早退最有利。
//   · **回退（保证终止 + 保证平衡）**：一侧为空（质心全相同/极密集/NaN）**或**中点分裂过偏
//     （任一侧不足 n/3）时，退回**按数量中位数**（前半 n/2）分裂。护栏是必需的：Sponza 这类
//     "少量离群簇 + 一大团"的分布会让纯中点一次只切掉 1~2 个簇，树深退化（实测未加护栏时
//     8287 簇的树深恰好顶到上限 24、叶子数 3103 ⇒ 大量 1~2 簇的叶子、节点数 6205）。
//     加 n/3 护栏后每次分裂都把规模压到 ≤ 2n/3 ⇒ 深度 ≤ 1 + log_{1.5}(n / 叶子容量)，
//     对 n ≤ `kNaniteMaxBVHClusters`(16384) 恒 ≤ 22 < 24 ⇒ 深度上限只是**安全网**。
//   · **叶子容量 4**：簇球很小（每簇 ≤64 三角形），4 个簇的叶子球仍然紧；遍历到叶子后至多
//     4 次球测试。树高 ≈ log(n/4)（实测 8287 簇 ⇒ 19 级），对 32 深的显式栈留有充分余量。
//   · **深度硬上限**（`kNaniteBVHMaxDepth` = 24）：超上限即停止分裂（该结点变成更大的叶子），
//     把"GPU 显式栈会不会溢出"从运行期风险变成构建期不变量（单测直接断言）。
//
// 【节点包围球的口径（确定性）】叶子 = 该叶子全部簇球的 AABB 包围球；内部节点 = 两个孩子
//   包围球的 AABB 包围球（`center` = 两者 AABB 的中心，`radius` = 到任一孩子球边界的最远距离）。
//   两者都只用 min/max/max 这类**与顺序无关**的运算，故逐位可复现；且父球恒包含所有后代簇球
//   ⇒ "父不可见 ⇒ 后代全不可见"这条早退是正确的保守判据。
//
// 【确定性】不含随机数、不读时间、不并行；排序的比较器带**下标兜底**（坐标相同时按下标），
//   因此即使坐标大量重复，顺序仍然唯一。同一输入两次构建逐位一致。
// ============================================================

/// 一次 cluster BVH 构建的产物（CPU 侧镜像；GPU 缓冲由 `NaniteCull` 从这个镜像上传）
struct NaniteClusterBVH {
    std::vector<NaniteBVHNode>       nodes;              ///< 节点表（[0] 恒为根；DFS 布局）
    std::vector<u32>                 leafClusterIndices; ///< 叶子簇表（扁平；叶子用 [left, left+count)）
    std::vector<NaniteClusterSphere> clusterSpheres;     ///< 每簇包围球（网格空间；与输入簇记录同序）

    u32 clusterCount        = 0u;  ///< 参与构建的簇数（= clusterSpheres.size()）
    u32 leafCount           = 0u;  ///< 叶子数（= nodes 里 leaf 位为 1 的个数）
    u32 depth               = 0u;  ///< 最大深度（**节点数**；根 = 1；空 BVH = 0）
    u32 maxLeafClusterCount = 0u;  ///< 实际最大叶子簇数（≤ 叶子容量；仅在深度上限触发时会更大）
    u32 maxStackDepthUpperBound = 0u;  ///< DFS 显式栈占用的上界（= 树高；单测用它核对 ≤ 栈容量）

    /// 产物是否为空（0 个簇 ⇒ 合法输入，节点表为空）
    [[nodiscard]] bool Empty() const { return nodes.empty(); }

    /// 只读视图（交给 `NaniteTraverseClusterBVHCPU` 用）
    [[nodiscard]] NaniteClusterBVHView View() const {
        NaniteClusterBVHView view;
        view.nodes              = nodes.empty() ? nullptr : nodes.data();
        view.nodeCount          = (u32)nodes.size();
        view.leafClusterIndices = leafClusterIndices.empty() ? nullptr : leafClusterIndices.data();
        view.clusterSpheres     = clusterSpheres.empty() ? nullptr : clusterSpheres.data();
        view.clusterCount       = clusterCount;
        return view;
    }
};

/// 按任务 9/10 的簇记录构建 per-instance cluster BVH（§14.8 任务 14）
///
/// 【输入】`clusters`：`NanitePackedAsset::clusters`（或 DAG 的出现记录）——只消费
///   `boundsCenterRadius`（center.xyz + radius）。**调用方负责钳制簇数上限**
///   （`NaniteCull::SetClusterBVH` 按 `kNaniteMaxBVHClusters` 截断并告警）。
/// 【输出】成功时整体写满 `outResult`；失败时不改写出参。
/// 【空输入】`clusters` 为空 ⇒ 返回 true 且产物为空（与任务 7/8/9/10/12 的"空网格是合法输入"同口径）。
/// 【半径防御】半径取 `max(radius, 0)` 并丢弃 NaN ⇒ 与遍历判据（`radius < 0` 归零）一致，
///   不会出现"构建期算出的球比遍历期判据更大/更小"的分歧。
[[nodiscard]] bool BuildNaniteClusterBVH(std::span<const NaniteClusterRecord> clusters,
                                         NaniteClusterBVH&                    outResult);

// ============================================================
// 【§14.8 任务 26】由 BVH 镜像推出**每簇的节点深度**（调试可视化模式 4 的数据源）
//
// 【为什么放在这里】它是"资产 → 加速结构"派生出来的只读量，与 `BuildNaniteClusterBVH`
//   同一层（RHI-free、纯函数、可单测），也天然被 `Tests/TestNaniteBuilder.cpp` 覆盖。
//
// 【口径（必须与 GPU 遍历逐字同源）】
//   · 深度 = **节点数**，根（`nodes[0]`）= 1（与 `NaniteClusterBVH::depth` 同一口径）；
//   · 每个叶子把它的簇（`leafClusterIndices[left .. left+count)`）记成"该叶子的深度"；
//     BVH 的构建保证每个簇恰好落在一个叶子里 ⇒ 输出是一张**满射且无冲突**的表；
//   · 输出长度恒 == `bvh.clusterCount`（未出现在任何叶子里的簇保持 0 ⇒ 读的人能看出
//     "这个簇不在 BVH 里"，而不是被一个编造的深度骗过）。
//
// 【为什么用显式栈而不是递归】深度可达 `kNaniteBVHMaxDepth`（24）；递归写法在
//   `-Werror` 的构建里没有优势，而显式栈与 GPU/shader 的遍历写法同构，读起来更容易对照。
// 【复杂度】O(节点数 + 簇数)，一次性（只在调试可视化开启时算一次）。
// 【失败】`bvh.nodes` 为空 ⇒ 输出被清空并返回 false（调用方按"无可视化数据"处理）；
//   节点/叶子下标越界（损坏镜像）⇒ 跳过该孩子并继续（不越界读，也不编造深度）。
// ============================================================
[[nodiscard]] bool ComputeNaniteClusterBVHDepths(const NaniteClusterBVH& bvh,
                                                 std::vector<u32>&        outDepths);

// ============================================================
// §14.8 任务 15：每簇 LOD 元数据（Phase 3 的 DAG 割判据的输入）
//
// 【为什么要这张表（而不是让 shader 现读 64B 簇记录）】
//   · Phase 3 需要两个数：`ownError`（用本簇替代其孩子渲染的误差 = 孩子的 `maxParentLODError`）
//     与 `parentError`（= 本簇自己的 `maxParentLODError`）。前者要么在 shader 里按
//     `childClusterOffset` 再读一条 64B 记录（访存放大 4 倍、还多一次分支），要么在这里预计算。
//   · 另外还需要 `lodLevel`（"选中级别分布"直方图）与"是不是根簇"（**必须显式**：根的
//     `parentError` 也是 0，只靠数值判断会把根永远筛掉 ⇒ 一个簇都选不出来，见
//     `NaniteClusterLODInfo` 的注释）。这两件都来自本任务的元数据表。
//   · 预计算还让 **CPU 参考与 GPU 读同一份比特**（任务 13/14 一以贯之的口径）。
//
// 【LOD 级从哪来】用 `.nanite` 的 LOD 段 `lodOffsets`（任务 10 定义为"该级第一个出现簇的下标"，
//   §14.20③ 明确写了"任务 15 可直接用"）：簇表按级升序排列 ⇒ `lodLevel(i)` = 满足
//   `lodOffsets[L] <= i` 的**最大** L。空 `lodOffsets` ⇒ 全部记为 0 级（退化为"只有一级"）。
//
// 【根簇的判定】扫描全部 `childClusterOffset/childCount`，被任何簇引为孩子的簇**不是**根；
//   其余都是根（DAG 里每个非根簇恰有一个父，任务 9 保证）。扫描与顺序无关 ⇒ 确定性。
//
// 【确定性】纯函数、无随机、无并行；同一输入两次调用逐位一致（单测直接比字节）。
// 【失败】簇记录与元数据长度不一致、`childClusterOffset + childCount` 越界、`lodOffsets` 非单调
//   ⇒ 返回 false 且**不改写出参**。
// ============================================================

/// 由簇记录 + LOD 段构建每簇 LOD 元数据（输出长度恒 == `clusters.size()`）
[[nodiscard]] bool BuildNaniteClusterLODInfo(std::span<const NaniteClusterRecord> clusters,
                                             std::span<const u32>                 lodOffsets,
                                             std::vector<NaniteClusterLODInfo>&   outResult);

// ============================================================
// §14.8 任务 24：页划分（CPU 侧，RHI-free；纯函数 ⇒ 可单测）
//
// 【权威设计】§14.32 的 ②（含 6 处修正）与「页边界从资产本身即可推出」那一段。
//   本函数把那份设计变成一条**由资产自身推出**的划分，不改 `.nanite` 格式、不依赖构建期
//   临时数组：唯一输入就是落盘后的 `clusters` 段与两段的记录数。
//
// 【为什么页边界推得出来（设计的关键依据）】
//   · `UniqueVertexOffset[]` / `uniqueTriangleOffset[]` 是构建期**单调追加**的（每份共享内容
//     一份），但**没有落盘**；而 `clusters[i].vertexOffset` 就是它的取值。
//   · 取 `clusters[i].vertexOffset` 的**去重升序集合** ⇒ 恰好等于那两套构建期数组
//     （内容是"因簇的需要"才被创建的 ⇒ 每份至少被一个簇引用 ⇒ 集合完整）。
//   · 相邻两个不同值之间就是一份共享内容的范围（最后一份到 `vertices.size()`）。
//   ⇒ 一份共享内容的三段区间全部由资产自身给出，**不需要改文件格式**。
//
// 【页的定义（实现口径，逐条对应 §14.32）】
//   · 份数分页：页 p 覆盖共享内容下标 `[p×K, min(C, (p+1)×K))`（K = `kNanitePageContentsPerPage`）；
//   · **顶点段 / 三角形段** = 该内容区间的**连续**记录区间
//     `[starts[p×K], starts[min(C,(p+1)×K)])`（端点都在页内 ⇒ 任何一份内容不跨页）；
//   · **簇段** = 引用了这些内容的那些"簇出现"记录，按**出现下标升序密集打包**
//     （去重路径下 `vertexOffset` 不单调 ⇒ 簇下标区间不可能同时是区间，见 `NaniteTypes.h`
//     的任务 24 小节）；每簇在页内的下标写在 `clusterPage[].local` 里；
//   · **顺序无关性**：顶点段与三角形段的秩必须**逐一对应**（同一份内容的两个秩相等），
//     不等就返回 false —— 这条守卫把"两张共享表不同源"这种损坏资产挡在门外。
//
// 【失败（返回 false，不抛、不崩、不改写出参）】簇段为空以外的任何输入异常：
//   `vertexCount/triangleCount` 为 0 却有簇；首份内容不从记录 0 开始（有前缀空洞）；
//   某份内容的区间为空或越出段尾；顶点秩与三角形秩不一致；`contentsPerPage == 0`。
// 【空资产】`clusters` 为空 ⇒ 返回 true 且产物为空（页数 0），与任务 7/8/9/10/12 同口径。
// 【确定性】只有排序与线性扫描，不含随机/时间/并行/哈希容器遍历序 ⇒ 同输入逐位一致。
// ============================================================

/// 页划分的**实测读数**（每一条都是验收 (b)/(c) 要核对的量）
struct NanitePagePlanStats {
    /// 【这三个与 `NanitePagePlan` 的同名字段**故意冗余**】两者是同一次划分的同一批量的两份
    ///   投影：plan 层是"结构"，这里的一份是"读数出口"（与 `pageStraddleCount` / 三个 `*Stride` /
    ///   `slot*Bytes` 同一处取用）。实现里在成功路径末尾一次写完两者，单测显式断言逐位相等。
    ///   【为什么必须写】留着恒 0 的同名字段会让"从 stats 读读数"的人拿到 0 ——
    ///   判据 (b) 的 `resident + 非驻留 == pages_total` 就会变成 `0 == 0 + 0` 的**空洞通过**。
    u32 clusterCount = 0;    ///< 簇出现总数（= `clusters.size()` = `NanitePagePlan::clusterCount`）
    u32 contentCount = 0;    ///< 共享内容份数（= 去重升序的 `vertexOffset` 个数）
    u32 pageCount    = 0;    ///< 页数（= `ceil(contentCount / K)`）

    /// 池槽步长（**条数**）：每页在三段里的最大条数。池缓冲的大小 = 槽数 × 步长 × 记录大小。
    /// 【为什么取"每页最大"而不是平均】池是**定长槽**的物理数组：任何一个槽都必须装得下
    ///   最坏的那一页，否则上传就会越界。
    u32 clusterStride  = 0;  ///< 一个槽最多装多少条簇记录（64B/条）
    u32 vertexStride   = 0;  ///< 一个槽最多装多少条量化顶点（16B/条）
    u32 triangleStride = 0;  ///< 一个槽最多装多少条打包三角形（8B/条）

    u32 maxClustersPerPage  = 0;  ///< 实测每页最大簇出现数（= `clusterStride`）
    u32 maxVerticesPerPage  = 0;  ///< 实测每页最大顶点条数（= `vertexStride`）
    u32 maxTrianglesPerPage = 0;  ///< 实测每页最大三角形条数（= `triangleStride`）

    /// **每份共享内容不跨页**的违反数（必须 0；非 0 ⇒ 调用方按 `page_straddle` 退化并报读数）
    u32 pageStraddleCount = 0;
    /// 顶点段与三角形段的秩不一致的簇数（正常 0）。**失败路径不通过出参外传**（本函数返回 false
    /// 且不改写出参）⇒ 它只是内部诊断，真正可见的证据是失败时那一条 `HE_CORE_WARN` 日志
    ///（带上第一个出问题的簇下标与两个秩）。成功路径下它恒为 0，调用方可以放心断言。
    u32 rankMismatchCount = 0;

    /// 三个段的"池总字节"上界（= 槽数 × 步长 × 记录大小；由调用方按池槽数算出，见 `NanitePagePlan::PoolBytes`）
    usize slotClusterBytes  = 0;  ///< 一个槽的簇段字节（= clusterStride × 64）
    usize slotVertexBytes   = 0;  ///< 一个槽的顶点段字节（= vertexStride × 16）
    usize slotTriangleBytes = 0;  ///< 一个槽的三角形段字节（= triangleStride × 8）
};

/// 一次页划分的产物（CPU 侧镜像；GPU 侧的页表由 `NaniteStream` 动态维护）
struct NanitePagePlan {
    u32 contentsPerPage = kNanitePageContentsPerPage;  ///< 每页共享内容份数（K）
    u32 clusterCount    = 0;   ///< 簇出现总数
    u32 contentCount    = 0;   ///< 共享内容份数
    u32 pageCount       = 0;   ///< 页数

    /// 每簇的（页号，页内簇序号）；长度 == clusterCount。**上传给 GPU 的正是这一份**。
    std::vector<NaniteClusterPageRef> clusterPage;
    /// 每页在三段里的区间（长度都 == pageCount）
    std::vector<u32> pageVertexBegin;     ///< 顶点段起始记录下标（共享顶点数组内）
    std::vector<u32> pageVertexCount;     ///< 顶点段条数
    std::vector<u32> pageTriangleBegin;   ///< 三角形段起始记录下标
    std::vector<u32> pageTriangleCount;   ///< 三角形段条数
    std::vector<u32> pageClusterBegin;    ///< 该页在"收集后的簇记录序"里的起点（= 前缀和）
    std::vector<u32> pageClusterCount;    ///< 该页收集到多少条簇出现记录

    NanitePagePlanStats stats;  ///< 实测读数

    /// 产物是否为空（空资产 = 合法输入：页数 0）
    [[nodiscard]] bool Empty() const { return pageCount == 0u; }

    /// 池在给定槽数下的三段总字节（越界防护与容量报告都用它；不进 shader）
    [[nodiscard]] usize PoolBytes(u32 slots) const {
        return (usize)slots * (stats.slotClusterBytes + stats.slotVertexBytes + stats.slotTriangleBytes);
    }
};

/// 由落盘后的簇记录 + 两段记录数构建页划分（§14.8 任务 24，纯函数）
/// @param clusters        资产簇段（每次出现一条 64B 记录）
/// @param vertexCount     顶点段记录数（= `header.vertexCount` = `asset.vertices.size()`）
/// @param triangleCount   三角形段记录数（= `asset.triangles.size()`）
/// @param contentsPerPage 每页共享内容份数（0 ⇒ 返回 false；调用方一般传 `kNanitePageContentsPerPage`）
/// @return 成功写满 `outResult`；失败**不改写出参**
[[nodiscard]] bool BuildNanitePagePlan(std::span<const NaniteClusterRecord> clusters,
                                       u32                                  vertexCount,
                                       u32                                  triangleCount,
                                       u32                                  contentsPerPage,
                                       NanitePagePlan&                      outResult);

// ============================================================
// 上传类（任务 1 骨架；任务 12 的 GPU 侧落在 `NaniteScene`）
//
// 任务 8/9/10/12 与它的关系：`BuildNaniteClusters()` / `BuildNaniteClusterDAG()` /
// `PackNaniteClusters()` / `BuildNaniteAssetFromGeometry()` 都在本文件，负责产出
// **CPU 侧字节镜像**；把这份镜像搬上 GPU 的 device 调用落在 `NaniteScene`（它是 GPU 资源
// 宿主）。**为什么不在本类里做**：本翻译单元被 `HugEngineTests` 直接编译，必须保持
// RHI-free（`Tests/CMakeLists.txt:50-54` 是一条纪律钉子），任何 RHI include 都会让单测
// 目标编译失败。本类因此仍只持有 `rhi::IRHIDevice*` 的指针（不前向调用它的成员）。
// ============================================================
class NaniteUpload {
public:
    NaniteUpload() = default;
    ~NaniteUpload() = default;

    NaniteUpload(const NaniteUpload&) = delete;
    NaniteUpload& operator=(const NaniteUpload&) = delete;

    /// 骨架就绪：只记住设备与视口尺寸。任务 12 起在这里建上传用的暂存/目标缓冲
    bool Initialize(rhi::IRHIDevice* device, u32 width, u32 height);

    void Shutdown();
    void OnResize(u32 width, u32 height);

    [[nodiscard]] bool IsReady() const { return m_Device != nullptr; }

private:
    rhi::IRHIDevice* m_Device = nullptr;
    u32 m_Width  = 0;
    u32 m_Height = 0;
};

} // namespace he::render
