#pragma once

// ============================================================
// Nanite/NaniteUpload.h — 离线簇切分（CPU 侧） + `.nanite` 资产 → GPU 缓冲
//
// 【本文件的两次填充】
//   · 任务 1（骨架）：上传类的生命周期桩（Initialize / Shutdown / OnResize / IsReady），
//     **不加载任何资产、不创建任何 GPU 缓冲**。
//   · 任务 8（本任务）：`BuildNaniteClusters()` —— 用 meshoptimizer v0.22 的
//     `meshopt_buildMeshlets` 把"位置 + 索引"切成 ≤64 三角形 / ≤128 顶点的簇
//     （§14.8 任务 8、设计 §4.1 L246-247），并**直接填进任务 7 定稿的 `NaniteClusterRecord`**。
//     这一段是**纯 CPU 的"资产准备侧"代码**，放进 `NaniteUpload.{h,cpp}` 正是 §14.3 的模块
//     边界（本任务不新增模块文件）。
//   · 任务 12：`.nanite` 资产读取 + 从 `MeshBatcher` 的合并几何**读一次** → 上传 GPU 缓冲。
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
/// 【输入】与 `BuildNaniteClusters` 完全同口径（positions = 紧密 xyz；indices = 三角形表）。
/// 【输出】成功时整体写满 `outResult`；**失败时不改写出参**（与任务 7/8 同口径）。
/// 【空网格】`indices` 为空 ⇒ 返回 true 且 `levelCount == 0`、产物为空（合法输入）。
/// 【失败】与 `BuildNaniteClusters` 相同的输入非法情形（索引不是 3 的倍数、越界索引、
///         有三角形却无顶点、某级切不出簇等）。
/// 【确定性】同一输入两次调用逐位一致：不含随机数、不读时间、不并行；唯一的哈希表只做**查**、
///         不参与产物顺序，所有数组都按"级序 + 级内簇序"追加。
[[nodiscard]] bool BuildNaniteClusterDAG(std::span<const float> positions,
                                         std::span<const u32>   indices,
                                         NaniteClusterDAG&      outResult);

// ============================================================
// 上传类（任务 1 骨架；任务 12 填 GPU 侧）
//
// 任务 8 与它的关系：任务 12 会先调用 `BuildNaniteClusters()`（CPU 侧准备），再做
// 量化/打包与 GPU 上传；本任务**不碰任何 GPU/渲染路径**，只交付这个纯函数。
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
