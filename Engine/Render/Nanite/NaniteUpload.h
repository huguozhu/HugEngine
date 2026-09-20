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
