// ============================================================
// Nanite/NaniteUpload.cpp — 上传段（任务 1 生命周期桩）+ 离线簇切分（任务 8，CPU 侧）
//
// 【本文件由 §14.8 任务 1 建立骨架，任务 8 加入 CPU 侧簇切分，其余内容由任务 12 填充】
//   · 任务 1：只有"记住设备与尺寸 / 清空"这几个动作，**没有任何 GPU 资源**。
//   · 任务 8：`BuildNaniteClusters()` —— `meshopt_buildMeshlets` 的落地（见头文件的接口说明）。
//     本翻译单元**不 include 任何 RHI 头**（上传类只把 `rhi::IRHIDevice*` 存下来/判空），
//     因此它可被单测目标 `HugEngineTests` 直接编译（`Tests/CMakeLists.txt` 里登记了本文件）。
//
// 【§14.3 依赖禁令】模块内不得引用 `GI_*` / `Lumen*` / `GPUCulling` 的内部结构
//   （可借其 Hi-Z 纹理句柄与描述符写法）；不得依赖 `MeshBatcher` 的运行时状态
//   —— 它只当**一次性输入**。故本文件（以及未来的实现）不得出现 `MeshBatcher*` 成员。
//
// 【§14.8 任务 8 的边界】本任务只做"网格 → 簇表"的纯 CPU 变换：不含量化、不生成 LOD/DAG、
//   不建 GPU 缓冲、不改任何渲染路径。那些分别是任务 10、任务 9、任务 12。
// ============================================================

#include "Nanite/NaniteUpload.h"

#include <meshoptimizer.h>   // meshopt_buildMeshlets / optimizeMeshlet / computeMeshletBounds（v0.22）

#include <algorithm>   // std::max

namespace he::render {
namespace {

// ── 任务 8 的两条硬上限：直接取任务 7 的契约常量，不在本文件另立数字 ──
inline constexpr usize kClusterMaxTriangles = kNaniteMaxClusterTriangles;   // 64
inline constexpr usize kClusterMaxVertices  = kNaniteMaxClusterVertices;    // 128
/// 位置数组的步长：每顶点 3 个紧密排布的 float ⇒ 12B
/// （meshopt 要求 stride ∈ [12, 256] 且是 4 的倍数；12 恰好同时满足）
inline constexpr usize kPositionStrideBytes = sizeof(float) * 3u;
/// 每个三角形的索引个数（与任务 7 的 `kNaniteIndicesPerTriangle` 同源）
inline constexpr usize kIndicesPerTriangle = kNaniteIndicesPerTriangle;     // 3

// 把 meshopt 的参数约束钉在编译期：任何一处改错都会直接编译失败，而不是等到运行期的 assert
static_assert(kClusterMaxVertices <= 255u,
              "meshopt_buildMeshlets 的 max_vertices 必须 ≤ 255（是 255，不是 256）");
static_assert(kClusterMaxTriangles % 4u == 0u,
              "meshopt_buildMeshlets 的 max_triangles 必须是 4 的倍数（索引数据 4B 对齐）");
static_assert(kClusterMaxTriangles >= 1u && kClusterMaxTriangles <= 512u,
              "meshopt_buildMeshlets 的 max_triangles 必须在 [1, 512] 内");
static_assert(kPositionStrideBytes >= 12u && kPositionStrideBytes <= 256u &&
              (kPositionStrideBytes % 4u) == 0u,
              "meshopt 要求 vertex_positions_stride ∈ [12,256] 且是 4 的倍数");

/// 把 meshopt 的法线锥落到任务 7 的 `NaniteConeAxisAngle`（含"无锥"哨兵映射）
///
/// meshopt 的 `cone_cutoff` 与任务 7 的 `cosHalfAngle` 是同一个量（都是剔除测试
/// `dot(dir, axis) >= cos` 里的 cos），故有真锥时**直接**落盘，不做角度换算。
/// meshopt 对"锥不可用"返回**零向量轴**（半锥 ≥ ~168° 的 trivial-accept，或簇内全是零面积
/// 三角形的 trivial-reject）—— 此时按任务 7 的哨兵编码（axis = 0、cos = -1 = 半角 180°，
/// 即"该簇不参与锥剔除"），而不是伪造一个单位轴。调用方用 `noConeClusterCount` 统计它。
[[nodiscard]] NaniteConeAxisAngle MakeConeAxisAngle(const meshopt_Bounds& bounds) {
    NaniteConeAxisAngle cone;
    const float axisLengthSquared = bounds.cone_axis[0] * bounds.cone_axis[0] +
                                    bounds.cone_axis[1] * bounds.cone_axis[1] +
                                    bounds.cone_axis[2] * bounds.cone_axis[2];
    if (!(axisLengthSquared > 0.0f)) {
        // 无可用锥 ⇒ 任务 7 的"无锥"哨兵（默认构造已经是这个值，这里显式写出以免读者误解）
        cone.axis[0] = 0.0f;
        cone.axis[1] = 0.0f;
        cone.axis[2] = 0.0f;
        cone.cosHalfAngle = kNaniteConeNoCullCos;   // = -1（半角 180°：恒不被锥剔除）
        return cone;
    }
    cone.axis[0] = bounds.cone_axis[0];
    cone.axis[1] = bounds.cone_axis[1];
    cone.axis[2] = bounds.cone_axis[2];
    // meshopt 的 cutoff 理论上已在 [-1,1]，这里夹一次是防御性（NaN 也会被夹成 -1 之外的边界）
    cone.cosHalfAngle = bounds.cone_cutoff < -1.0f ? -1.0f
                      : (bounds.cone_cutoff >  1.0f ?  1.0f : bounds.cone_cutoff);
    return cone;
}

} // namespace

bool BuildNaniteClusters(std::span<const float> positions,
                         std::span<const u32>   indices,
                         NaniteClusterBuild&    outResult) {
    // ── 输入校验：失败一律返回 false 且**不改写出参**（先在本地构建，成功后再整体交出）──
    if ((indices.size() % kIndicesPerTriangle) != 0u) return false;   // 索引个数必须是 3 的倍数
    if ((positions.size() % 3u) != 0u)                return false;   // 位置必须是完整的 xyz

    const usize vertexCount = positions.size() / 3u;
    for (const u32 index : indices) {
        if ((usize)index >= vertexCount) return false;   // 越界索引：绝不把它交给 meshopt（其内部只在 debug 下 assert）
    }
    if (!indices.empty() && vertexCount == 0u) return false;

    NaniteClusterBuild result;

    const usize sourceTriangleCount = indices.size() / kIndicesPerTriangle;
    if (sourceTriangleCount == 0u) {
        // 空网格：合法输入，产物为空（簇数 0、统计全 0），不是失败
        outResult = std::move(result);
        return true;
    }

    // ── meshopt 输出缓冲：容量按官方最坏情况公式算（`meshopt_buildMeshletsBound` 的注释）──
    const usize maxMeshlets = meshopt_buildMeshletsBound(indices.size(), kClusterMaxVertices,
                                                         kClusterMaxTriangles);
    std::vector<meshopt_Meshlet> meshlets(maxMeshlets);
    std::vector<u32> meshletVertices(maxMeshlets * kClusterMaxVertices);            // 局部下标 → 网格顶点
    std::vector<u8>  meshletTriangles(maxMeshlets * kClusterMaxTriangles * kIndicesPerTriangle);  // 每三角形 3 字节

    // cone_weight = 0：本任务只要求"簇内空间紧凑 + 顶点/三角形不超限"；cone_weight 影响簇形状与
    // 锥质量的取舍（meshopt 文档建议需要锥剔除时取 0~1），那属于任务 15（锥剔除）的调参，
    // 任务 8 取 0 以保证结果是"纯空间划分"的确定性基线。
    const usize meshletCount = meshopt_buildMeshlets(
        meshlets.data(), meshletVertices.data(), meshletTriangles.data(),
        indices.data(), indices.size(),
        positions.data(), vertexCount, kPositionStrideBytes,
        kClusterMaxVertices, kClusterMaxTriangles, /*cone_weight=*/0.0f);

    // 非空输入却切不出簇：视为失败（不静默产出空资产，否则调用方会拿到"看起来成功"的空文件）
    if (meshletCount == 0u || meshletCount > maxMeshlets) return false;

    result.clusters.reserve(meshletCount);
    result.clusterVertexCount.reserve(meshletCount);
    result.triangles.reserve(sourceTriangleCount);
    result.vertexIndices.reserve(meshletCount * kClusterMaxVertices);

    for (usize m = 0; m < meshletCount; ++m) {
        const meshopt_Meshlet& meshlet = meshlets[m];
        // `vertex_offset` 是 u32 元素下标；`triangle_offset` 是**字节**下标（每簇按 4B 对齐）
        u32* const clusterVertices = meshletVertices.data() + meshlet.vertex_offset;
        u8*  const clusterTriangles = meshletTriangles.data() + meshlet.triangle_offset;

        // ① 簇内重排：只在本簇的子区间内原地重排（提升后续光栅的顶点局部性）。
        //    它不增删顶点/三角形，故下面沿用的 vertex_count / triangle_count 仍然成立。
        meshopt_optimizeMeshlet(clusterVertices, clusterTriangles,
                                meshlet.triangle_count, meshlet.vertex_count);

        // ② 包围球 + 法线锥：meshopt 的伴生接口，直接吃"簇内局部下标"寻址的那两个数组
        const meshopt_Bounds bounds = meshopt_computeMeshletBounds(
            clusterVertices, clusterTriangles, meshlet.triangle_count,
            positions.data(), vertexCount, kPositionStrideBytes);

        // ③ 填任务 7 定稿的簇记录（64B）
        NaniteClusterRecord record{};
        record.boundsCenterRadius[0] = bounds.center[0];
        record.boundsCenterRadius[1] = bounds.center[1];
        record.boundsCenterRadius[2] = bounds.center[2];
        record.boundsCenterRadius[3] = bounds.radius;
        record.cone = MakeConeAxisAngle(bounds);
        // 三角形**下标**（× 8B = 索引段字节偏移）；顶点**下标**（× 16B = 顶点段字节偏移）
        record.triangleOffset = (u32)result.triangles.size();
        record.triangleCount  = meshlet.triangle_count;
        record.vertexOffset   = (u32)result.vertexIndices.size();
        // 下列字段任务 8 填不出来，**明确留 0**，不伪造语义：
        record.materialID         = 0u;     // 任务 10/12 填（bindless 材质 ID 由材质解析决定）
        record.maxParentLODError  = 0.0f;   // 任务 9/10 填（LOD/DAG 尚未生成）
        record.childClusterOffset = 0u;     // 任务 9 填（0 = 叶子）
        record.childCount         = 0u;     // 任务 9 填

        // ④ 顶点表：把本簇的"簇内局部下标 → 网格顶点"拼到全局顶点表尾部（跨簇允许重复）
        for (u32 v = 0; v < meshlet.vertex_count; ++v) {
            result.vertexIndices.push_back(clusterVertices[v]);
        }

        // ⑤ 三角形表：u8 局部下标 → 任务 7 的 3×u16 打包
        //    "≤128 顶点"（= 局部下标 ≤127）正是 u16 打包的前提，这里做一次防御性检查：
        //    一旦 meshopt 违反了上限，宁可整个失败，也不写出会解码错的簇记录。
        for (u32 t = 0; t < meshlet.triangle_count; ++t) {
            const u32 i0 = clusterTriangles[t * 3u + 0u];
            const u32 i1 = clusterTriangles[t * 3u + 1u];
            const u32 i2 = clusterTriangles[t * 3u + 2u];
            if (!IsValidClusterLocalVertexIndex(i0) ||
                !IsValidClusterLocalVertexIndex(i1) ||
                !IsValidClusterLocalVertexIndex(i2)) {
                return false;
            }
            result.triangles.push_back(NanitePackTriangle(i0, i1, i2));
        }

        result.clusterVertexCount.push_back(meshlet.vertex_count);
        result.clusters.push_back(record);
    }

    // ── ⑥ 统计（验收读数：簇数 / 每簇三角形数（含最大值）/ 每簇顶点数 / 退化簇数）──
    NaniteClusterBuildStats stats;
    stats.clusterCount  = (u32)result.clusters.size();
    stats.triangleCount = (u32)result.triangles.size();
    stats.vertexCount   = (u32)result.vertexIndices.size();
    for (usize c = 0; c < result.clusters.size(); ++c) {
        const u32 localTriangles = result.clusters[c].triangleCount;
        const u32 localVertices  = result.clusterVertexCount[c];
        stats.maxClusterTriangles = std::max(stats.maxClusterTriangles, localTriangles);
        stats.maxClusterVertices  = std::max(stats.maxClusterVertices, localVertices);
        // 退化簇 = 三角形数为 0 或顶点数为 0（meshopt 正常不会产出，故这条是"契约守卫"读数）
        if (localTriangles == 0u || localVertices == 0u) ++stats.degenerateClusterCount;
        if (result.clusters[c].cone.cosHalfAngle == kNaniteConeNoCullCos) ++stats.noConeClusterCount;
    }
    result.stats = stats;

    outResult = std::move(result);   // 只有走到这里才动调用方的对象
    return true;
}

bool NaniteUpload::Initialize(rhi::IRHIDevice* device, u32 width, u32 height) {
    // 任务 1：骨架就绪 = 拿到设备。任务 12 起在这里建暂存缓冲与目标缓冲，
    // 并把"缓冲是否真的建成"纳入这个返回值。
    m_Device = device;
    m_Width  = width;
    m_Height = height;
    return m_Device != nullptr;
}

void NaniteUpload::Shutdown() {
    // 任务 1 没有自持资源；任务 12 起在这里释放暂存/目标缓冲。
    m_Device = nullptr;
    m_Width  = 0;
    m_Height = 0;
}

void NaniteUpload::OnResize(u32 width, u32 height) {
    // 上传段的资源与世界空间挂钩，不随视口变化；这里只记录尺寸以便后续诊断。
    m_Width  = width;
    m_Height = height;
}

} // namespace he::render
