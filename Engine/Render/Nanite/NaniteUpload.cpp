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

#include <meshoptimizer.h>   // meshopt_buildMeshlets / optimizeMeshlet / computeMeshletBounds / meshopt_simplify（v0.22）

#include <algorithm>       // std::max / std::sort / std::equal
#include <array>           // std::array（三角形规范键）
#include <cmath>           // std::sqrt
#include <unordered_map>   // 任务 9 的去重桶（只查不改产物顺序，见头文件的确定性说明）

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

// ============================================================
// §14.8 任务 9：LOD 链 + DAG 去重的内部辅助
// ============================================================

// 任务 9 的两条参数约束也钉在编译期（口径见头文件）：
static_assert(kNaniteMaxLODLevels >= 2u,
              "LOD 链至少要能产出 1 级以上（levelCount > 0 的验收前提）");
static_assert(kNaniteMinLODTriangles == kNaniteMaxClusterTriangles,
              "LOD 终止阈值必须与任务 7 的每簇上限同源（一个满簇）");

/// FNV-1a 64 的初始值与质数：逐 u32 混入（自己维护，不依赖平台字节序与标准库 `hash`）
inline constexpr u64 kFnv1aOffsetBasis = 14695981039346656037ull;
inline constexpr u64 kFnv1aPrime       = 1099511628211ull;

/// 把一个 u32 的 4 个字节依次混进 FNV-1a（大端序写死，保证跨平台哈希一致）
[[nodiscard]] inline u64 HashU32(u64 hash, u32 value) {
    for (u32 b = 0; b < 4u; ++b) {
        hash ^= (u64)((value >> (b * 8u)) & 0xFFu);
        hash *= kFnv1aPrime;
    }
    return hash;
}

/// 两个簇的包围球心距的平方（父/子就近配对用；避免开方、也不用 std::sqrt 的近似差异）
[[nodiscard]] inline float SquaredCenterDistance(const NaniteClusterRecord& a,
                                                 const NaniteClusterRecord& b) {
    const float dx = a.boundsCenterRadius[0] - b.boundsCenterRadius[0];
    const float dy = a.boundsCenterRadius[1] - b.boundsCenterRadius[1];
    const float dz = a.boundsCenterRadius[2] - b.boundsCenterRadius[2];
    return dx * dx + dy * dy + dz * dz;
}

/// 把一个簇的局部几何/拓扑规范化成"顺序无关"的键流（用于哈希与命中后的精确比对）
///
/// 【键流布局】`[顶点数 N][排序后的 N 个位置词][三角形数 T][排序后的 T 个三角形键（各 3 个位置词）]`
/// 位置词 = 任务 7 的量化位置打包（R10G10B10A2），量化原点 = `record` 的簇 AABB 中心、
/// 尺度 = 整网格最大范围（口径说明见头文件"为什么是簇内局部"）。
/// 【三角形键】三种**循环旋转**里取字典序最小（保绕序，不做反转），再对全部键排序。
/// 【输出】`outWords` 是**原始顺序**的位置词（共享内容按原始顺序存放）；
///         `outKeys` 是规范键流（哈希与比对都用它）。
/// 【失败】局部下标越出本簇顶点数（防御性；正常不可达）⇒ 返回 false。
[[nodiscard]] bool BuildClusterCanonicalKeys(std::span<const float>     positions,
                                             const NaniteClusterRecord& record,
                                             const u32*                 clusterVertexIndices,
                                             const NanitePackedTriangle* clusterTriangles,
                                             u32                        localVertexCount,
                                             u32                        localTriangleCount,
                                             float                      meshExtent,
                                             std::vector<u32>&          outWords,
                                             std::vector<u32>&          outKeys) {
    outWords.clear();
    outKeys.clear();
    outWords.resize(localVertexCount);

    const float centerX = record.boundsCenterRadius[0];
    const float centerY = record.boundsCenterRadius[1];
    const float centerZ = record.boundsCenterRadius[2];

    // ① 局部顶点的量化位置词（相对簇心；平移副本 ⇒ 词相同）
    for (u32 v = 0; v < localVertexCount; ++v) {
        const float* position = positions.data() + (usize)clusterVertexIndices[v] * 3u;
        const u32 rawX = NaniteQuantizePositionAxis(position[0], centerX, meshExtent);
        const u32 rawY = NaniteQuantizePositionAxis(position[1], centerY, meshExtent);
        const u32 rawZ = NaniteQuantizePositionAxis(position[2], centerZ, meshExtent);
        outWords[v] = NanitePackPosition(rawX, rawY, rawZ);
    }

    outKeys.reserve(2u + (usize)localVertexCount + (usize)localTriangleCount * 3u);

    // ② 顶点数 + 排序后的位置词（顶点顺序无关）
    outKeys.push_back(localVertexCount);
    {
        const std::ptrdiff_t sortBegin = (std::ptrdiff_t)outKeys.size();
        outKeys.insert(outKeys.end(), outWords.begin(), outWords.end());
        std::sort(outKeys.begin() + sortBegin, outKeys.end());
    }

    // ③ 三角形键：循环旋转规范化 → 排序（三角形顺序无关）
    outKeys.push_back(localTriangleCount);
    if (localTriangleCount > 0u) {
        std::vector<std::array<u32, 3>> triangleKeys;
        triangleKeys.reserve(localTriangleCount);
        for (u32 t = 0; t < localTriangleCount; ++t) {
            const NanitePackedTriangle& packed = clusterTriangles[t];
            const u32 i0 = NaniteTriangleIndex0(packed);
            const u32 i1 = NaniteTriangleIndex1(packed);
            const u32 i2 = NaniteTriangleIndex2(packed);
            if (i0 >= localVertexCount || i1 >= localVertexCount || i2 >= localVertexCount) {
                return false;   // 防御性：绝不越界读局部顶点表
            }
            const u32 words[3] = { outWords[i0], outWords[i1], outWords[i2] };

            // 三种循环旋转 (0,1,2)/(1,2,0)/(2,0,1) 里取字典序最小（保绕序）
            u32 best = 0u;
            for (u32 rotation = 1u; rotation < 3u; ++rotation) {
                for (u32 k = 0; k < 3u; ++k) {
                    const u32 current = words[(best + k) % 3u];
                    const u32 other   = words[(rotation + k) % 3u];
                    if (current != other) {
                        if (other < current) best = rotation;
                        break;
                    }
                }
            }
            triangleKeys.push_back(std::array<u32, 3>{
                words[best], words[(best + 1u) % 3u], words[(best + 2u) % 3u] });
        }
        std::sort(triangleKeys.begin(), triangleKeys.end());
        for (const std::array<u32, 3>& key : triangleKeys) {
            outKeys.push_back(key[0]);
            outKeys.push_back(key[1]);
            outKeys.push_back(key[2]);
        }
    }

    return true;
}

/// 把一级的簇组"合并"回一张索引表（局部下标经该级顶点表还原成网格顶点下标）
///
/// 这正是"下一级的简化输入 = 上一级的簇组"（§4.1 的边折叠链）：合并结果的三角形多重集
/// 与上一级完全一致（`BuildNaniteClusters` 只重排、不增删），但顺序按簇内局部性重排过。
[[nodiscard]] std::vector<u32> MergeLevelClusters(const NaniteClusterBuild& build) {
    std::vector<u32> merged;
    merged.reserve(build.triangles.size() * kIndicesPerTriangle);
    for (usize c = 0; c < build.clusters.size(); ++c) {
        const NaniteClusterRecord& record = build.clusters[c];
        for (u32 t = 0; t < record.triangleCount; ++t) {
            const NanitePackedTriangle& packed = build.triangles[record.triangleOffset + t];
            merged.push_back(build.vertexIndices[record.vertexOffset + NaniteTriangleIndex0(packed)]);
            merged.push_back(build.vertexIndices[record.vertexOffset + NaniteTriangleIndex1(packed)]);
            merged.push_back(build.vertexIndices[record.vertexOffset + NaniteTriangleIndex2(packed)]);
        }
    }
    return merged;
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

// ============================================================
// §14.8 任务 9：LOD 链（逐级减半） + DAG 去重
//
// 口径、终止条件、误差来源、哈希规范化都写在 `NaniteUpload.h` 的同名小节里，
// 这里只留与代码逐句对应的短注释。
// ============================================================
bool BuildNaniteClusterDAG(std::span<const float> positions,
                           std::span<const u32>   indices,
                           NaniteClusterDAG&      outResult) {
    // ── 输入校验：与任务 8 同口径；失败一律返回 false 且**不改写出参** ──
    if ((indices.size() % kIndicesPerTriangle) != 0u) return false;
    if ((positions.size() % 3u) != 0u)                return false;

    const usize vertexCount = positions.size() / 3u;
    for (const u32 index : indices) {
        if ((usize)index >= vertexCount) return false;
    }
    if (!indices.empty() && vertexCount == 0u) return false;

    NaniteClusterDAG result;
    if (indices.empty()) {
        // 空网格：合法输入，级数 0、产物为空（不是失败）
        outResult = std::move(result);
        return true;
    }

    // ── 网格量化尺度：整网格包围盒的最大轴长（与 meshopt_simplifyScale 同口径）──
    float bboxMin[3] = { positions[0], positions[1], positions[2] };
    float bboxMax[3] = { bboxMin[0], bboxMin[1], bboxMin[2] };
    for (usize v = 1; v < vertexCount; ++v) {
        for (u32 axis = 0; axis < 3u; ++axis) {
            const float value = positions[v * 3u + axis];
            if (value < bboxMin[axis]) bboxMin[axis] = value;
            if (value > bboxMax[axis]) bboxMax[axis] = value;
        }
    }
    float meshExtent = std::max(bboxMax[0] - bboxMin[0],
                                std::max(bboxMax[1] - bboxMin[1], bboxMax[2] - bboxMin[2]));
    if (!(meshExtent > 0.0f)) meshExtent = 1.0f;   // 单点/全重合网格：给量化一个非退化尺度

    // ── 去重表（哈希 → 候选唯一内容下标；命中后还要做规范键流全量比对）──
    std::unordered_map<u64, std::vector<u32>> dedupBuckets;
    std::vector<u32> uniqueCanonicalOffset;   // 每唯一内容在 uniqueCanonicalKeys 的起始
    std::vector<usize> uniqueCanonicalCount;  // 每唯一内容的规范键流长度
    std::vector<u32> uniqueCanonicalKeys;     // 全部唯一内容的规范键流（扁平存放）

    // ── LOD 链：级 0 = 原始网格；每级结束前尝试"合并本级簇组 → meshopt_simplify 减半" ──
    std::vector<u32>   levelIndices(indices.begin(), indices.end());
    std::vector<float> stepErrors;            // stepErrors[L] = 级 L → L+1 的**绝对**简化误差
    std::vector<u32>   words;                 // 复用缓冲：某簇的局部顶点位置词
    std::vector<u32>   keys;                  // 复用缓冲：某簇的规范键流

    for (;;) {
        const u32 level = (u32)result.levelClusterCount.size();

        NaniteClusterBuild levelBuild;
        if (!BuildNaniteClusters(positions, levelIndices, levelBuild)) return false;
        if (levelBuild.Empty()) return false;   // 非空输入却切不出簇 ⇒ 失败（任务 8 同口径）

        result.levelClusterOffset.push_back((u32)result.clusters.size());
        result.levelClusterCount.push_back(levelBuild.stats.clusterCount);
        result.levelTriangleCount.push_back(levelBuild.stats.triangleCount);

        // ── 逐簇：规范化 → 去重 → 写一条"出现"记录 ──
        for (usize c = 0; c < levelBuild.clusters.size(); ++c) {
            const NaniteClusterRecord& levelRecord = levelBuild.clusters[c];
            const u32  localVertexCount   = levelBuild.clusterVertexCount[c];
            const u32  localTriangleCount = levelRecord.triangleCount;
            const u32* clusterVertexIndices = levelBuild.vertexIndices.data() + levelRecord.vertexOffset;
            const NanitePackedTriangle* clusterTriangles =
                levelBuild.triangles.data() + levelRecord.triangleOffset;

            // ① 用**簇 AABB 中心 + 最大顶点距离**重算包围球，替换 meshopt 的最小包围球：
            //    量化原点必须是**顶点顺序无关**的量 —— meshopt 的最小包围球中心随簇内顶点顺序漂移
            //    （平移副本会算出不同的中心 ⇒ 位置词不同 ⇒ 明明相同的簇去重不了）。
            //    AABB 的 min/max 与顺序无关，整数平移下逐位可交换，故平移副本给出**逐位相同**的位置词。
            //    半径仍是"包含本簇全部顶点"的合法包围球（只是不再最小 ⇒ 剔除略保守，属可接受的代价）。
            const float* first = positions.data() + (usize)clusterVertexIndices[0] * 3u;
            float aabbMin[3] = { first[0], first[1], first[2] };
            float aabbMax[3] = { first[0], first[1], first[2] };
            for (u32 v = 1; v < localVertexCount; ++v) {
                const float* position = positions.data() + (usize)clusterVertexIndices[v] * 3u;
                for (u32 axis = 0; axis < 3u; ++axis) {
                    if (position[axis] < aabbMin[axis]) aabbMin[axis] = position[axis];
                    if (position[axis] > aabbMax[axis]) aabbMax[axis] = position[axis];
                }
            }
            const float center[3] = { (aabbMin[0] + aabbMax[0]) * 0.5f,
                                      (aabbMin[1] + aabbMax[1]) * 0.5f,
                                      (aabbMin[2] + aabbMax[2]) * 0.5f };
            float radiusSquared = 0.0f;
            for (u32 v = 0; v < localVertexCount; ++v) {
                const float* position = positions.data() + (usize)clusterVertexIndices[v] * 3u;
                const float dx = position[0] - center[0];
                const float dy = position[1] - center[1];
                const float dz = position[2] - center[2];
                const float distanceSquared = dx * dx + dy * dy + dz * dz;
                if (distanceSquared > radiusSquared) radiusSquared = distanceSquared;
            }

            NaniteClusterRecord record = levelRecord;   // cone / 计数从任务 8 的产物继承
            record.boundsCenterRadius[0] = center[0];
            record.boundsCenterRadius[1] = center[1];
            record.boundsCenterRadius[2] = center[2];
            record.boundsCenterRadius[3] = std::sqrt(radiusSquared);
            record.maxParentLODError  = 0.0f;           // 稍后按级填
            record.childClusterOffset = 0u;
            record.childCount         = 0u;

            if (!BuildClusterCanonicalKeys(positions, record, clusterVertexIndices, clusterTriangles,
                                           localVertexCount, localTriangleCount, meshExtent,
                                           words, keys)) {
                return false;
            }

            // 内容哈希：规范键流逐 u32 混入 FNV-1a 64
            u64 hash = kFnv1aOffsetBasis;
            for (const u32 word : keys) hash = HashU32(hash, word);

            // 命中的候选还要**全量比对规范键流**（杜绝哈希碰撞导致的错误共享）
            u32  uniqueIndex = 0u;
            bool found = false;
            const auto bucketIt = dedupBuckets.find(hash);
            if (bucketIt != dedupBuckets.end()) {
                for (const u32 candidate : bucketIt->second) {
                    if (uniqueCanonicalCount[candidate] != keys.size()) continue;
                    if (std::equal(keys.begin(), keys.end(),
                                   uniqueCanonicalKeys.begin() + uniqueCanonicalOffset[candidate])) {
                        uniqueIndex = candidate;
                        found = true;
                        break;
                    }
                }
            }

            if (!found) {
                // 新内容：把这一份"簇内局部几何 + 拓扑"存进共享表（只此一份）
                uniqueIndex = (u32)result.uniqueVertexCount.size();
                result.uniqueVertexOffset.push_back((u32)result.uniqueVertexWords.size());
                result.uniqueVertexCount.push_back(localVertexCount);
                result.uniqueVertexWords.insert(result.uniqueVertexWords.end(),
                                                words.begin(), words.end());
                result.uniqueTriangleOffset.push_back((u32)result.uniqueTriangles.size());
                result.uniqueTriangleCount.push_back(localTriangleCount);
                result.uniqueTriangles.insert(result.uniqueTriangles.end(),
                                              clusterTriangles, clusterTriangles + localTriangleCount);

                dedupBuckets[hash].push_back(uniqueIndex);
                uniqueCanonicalOffset.push_back((u32)uniqueCanonicalKeys.size());
                uniqueCanonicalCount.push_back(keys.size());
                uniqueCanonicalKeys.insert(uniqueCanonicalKeys.end(), keys.begin(), keys.end());
            }

            // 出现记录：偏移改指共享内容（链接/误差字段稍后统一填）
            record.vertexOffset   = result.uniqueVertexOffset[uniqueIndex];
            record.triangleOffset = result.uniqueTriangleOffset[uniqueIndex];

            result.clusters.push_back(record);
            result.clusterVertexCount.push_back(localVertexCount);
            result.clusterVertexIndexOffset.push_back((u32)result.clusterVertexIndices.size());
            result.clusterVertexIndices.insert(result.clusterVertexIndices.end(),
                                               clusterVertexIndices,
                                               clusterVertexIndices + localVertexCount);
            result.clusterLevel.push_back(level);
            result.clusterUnique.push_back(uniqueIndex);

            // 每簇上限 / 退化簇统计（与任务 8 同口径）
            result.stats.maxClusterTriangles = std::max(result.stats.maxClusterTriangles, localTriangleCount);
            result.stats.maxClusterVertices  = std::max(result.stats.maxClusterVertices, localVertexCount);
            if (localTriangleCount == 0u || localVertexCount == 0u) ++result.stats.degenerateClusterCount;
            if (record.cone.cosHalfAngle == kNaniteConeNoCullCos)   ++result.stats.noConeClusterCount;
        }

        // ── 终止条件①/③：下一级目标不足一个满簇、或已达最大级数 ──
        const u32 levelTriangleCount = levelBuild.stats.triangleCount;
        if (result.levelClusterCount.size() >= (usize)kNaniteMaxLODLevels) break;
        if (levelTriangleCount < 2u * kNaniteMinLODTriangles) break;

        // ── 合并本级簇组 → 索引表，再简化到"一半" ──
        std::vector<u32> merged = MergeLevelClusters(levelBuild);
        const u32   targetTriangles  = levelTriangleCount / 2u;
        const usize targetIndexCount = (usize)targetTriangles * kIndicesPerTriangle;

        // 目标索引数必须 ≤ 输入索引数（meshopt 的 assert；targetTriangles 取整除法保证成立）
        std::vector<u32> simplified(merged.size());   // 最坏情况要装下 index_count 个索引
        float relativeError = 0.0f;
        const usize simplifiedCount = meshopt_simplify(
            simplified.data(), merged.data(), merged.size(),
            positions.data(), vertexCount, kPositionStrideBytes,
            targetIndexCount, /*target_error=*/1.0f, /*options=*/0u, &relativeError);

        // ── 终止条件②：没能减少（或返回非法个数）⇒ 停，绝不空转 ──
        if (simplifiedCount == 0u || (simplifiedCount % kIndicesPerTriangle) != 0u ||
            simplifiedCount >= merged.size()) {
            break;
        }
        simplified.resize(simplifiedCount);

        // 绝对误差 = 相对误差 × 网格范围缩放（头文件 :419-424 的换算）
        const float absoluteError =
            relativeError * meshopt_simplifyScale(positions.data(), vertexCount, kPositionStrideBytes);
        stepErrors.push_back(absoluteError > 0.0f ? absoluteError : 0.0f);

        levelIndices = std::move(simplified);
    }

    const usize levelCount    = result.levelClusterCount.size();
    const usize totalClusters = result.clusters.size();
    result.levelClusterOffset.push_back((u32)totalClusters);   // 级视图的收尾元素（区间右端）

    // ── DAG 链接：级 L 的每个出现 → 级 L+1 的**恰好一个**父出现（父级一定更粗 ⇒ 无环）──
    result.parentCluster.assign(totalClusters, kNaniteNoParentCluster);
    std::vector<u8>  hasParent(totalClusters, 0u);
    std::vector<std::vector<u32>> childrenOf(totalClusters);
    std::vector<float> levelFallbackError(levelCount, 0.0f);   // 几何兜底误差（meshopt 报 0 时用）

    for (usize level = 0; level + 1u < levelCount; ++level) {
        const u32 childBegin  = result.levelClusterOffset[level];
        const u32 childEnd    = result.levelClusterOffset[level + 1u];
        const u32 parentBegin = result.levelClusterOffset[level + 1u];
        const u32 parentEnd   = result.levelClusterOffset[level + 2u];

        // 认父（写回父下标 + 子表 + 兜底误差）
        const auto assign = [&](u32 parent, u32 child, usize linkLevel) {
            hasParent[child] = 1u;
            result.parentCluster[child] = parent;
            childrenOf[parent].push_back(child);
            const float centerDistance = std::sqrt(SquaredCenterDistance(result.clusters[child],
                                                                        result.clusters[parent]));
            const float bound = centerDistance + result.clusters[child].boundsCenterRadius[3];
            if (bound > levelFallbackError[linkLevel]) levelFallbackError[linkLevel] = bound;
        };

        // ① 每个父簇先抢一个最近的、尚未认父的子簇（父簇数 ≤ 子簇数 ⇒ 一定抢得到）
        for (u32 parent = parentBegin; parent < parentEnd; ++parent) {
            u32   best = kNaniteNoParentCluster;
            float bestDistance = 0.0f;
            for (u32 child = childBegin; child < childEnd; ++child) {
                if (hasParent[child] != 0u) continue;
                const float distance = SquaredCenterDistance(result.clusters[child],
                                                             result.clusters[parent]);
                if (best == kNaniteNoParentCluster || distance < bestDistance) {
                    best = child;
                    bestDistance = distance;
                }
            }
            if (best != kNaniteNoParentCluster) assign(parent, best, level);
        }

        // ② 剩下的子簇各自认最近的父簇（每个子簇恰有一个父）
        for (u32 child = childBegin; child < childEnd; ++child) {
            if (hasParent[child] != 0u) continue;
            u32   best = parentBegin;
            float bestDistance = SquaredCenterDistance(result.clusters[child],
                                                       result.clusters[parentBegin]);
            for (u32 parent = parentBegin + 1u; parent < parentEnd; ++parent) {
                const float distance = SquaredCenterDistance(result.clusters[child],
                                                             result.clusters[parent]);
                if (distance < bestDistance) {
                    best = parent;
                    bestDistance = distance;
                }
            }
            assign(best, child, level);
        }
    }

    // 扁平化子簇表：按出现顺序写 childClusterOffset / childCount（子簇下标升序 ⇒ 确定）
    for (u32 cluster = 0; cluster < (u32)totalClusters; ++cluster) {
        std::vector<u32>& kids = childrenOf[cluster];
        std::sort(kids.begin(), kids.end());
        result.clusters[cluster].childClusterOffset = (u32)result.childClusterIndices.size();
        result.clusters[cluster].childCount         = (u32)kids.size();
        result.childClusterIndices.insert(result.childClusterIndices.end(), kids.begin(), kids.end());
    }

    // ── maxParentLODError：级 L → L+1 的简化绝对误差；报 0 时退几何兜底；根写 0 ──
    for (usize cluster = 0; cluster < totalClusters; ++cluster) {
        const u32 level = result.clusterLevel[cluster];
        float error = 0.0f;
        if ((usize)level + 1u < levelCount) {
            error = (level < stepErrors.size()) ? stepErrors[level] : 0.0f;
            if (!(error > 0.0f)) error = levelFallbackError[level];
        }
        result.clusters[cluster].maxParentLODError = error;
        result.stats.maxLODError = std::max(result.stats.maxLODError, error);
    }

    // ── 每级引用到的不同唯一内容数（级视图的最后一个读数）──
    for (usize level = 0; level < levelCount; ++level) {
        std::vector<u8> seen(result.uniqueVertexCount.size(), 0u);
        u32 distinct = 0u;
        for (u32 cluster = result.levelClusterOffset[level];
             cluster < result.levelClusterOffset[level + 1u]; ++cluster) {
            const u32 unique = result.clusterUnique[cluster];
            if (seen[unique] == 0u) {
                seen[unique] = 1u;
                ++distinct;
            }
        }
        result.levelUniqueCount.push_back(distinct);
    }

    // ── 统计收口 ──
    result.stats.levelCount           = (u32)levelCount;
    result.stats.simplifiedLevelCount = levelCount > 0u ? (u32)(levelCount - 1u) : 0u;
    result.stats.totalClusterCount    = (u32)totalClusters;
    result.stats.uniqueClusterCount   = (u32)result.uniqueVertexCount.size();
    result.stats.dedupRate = (totalClusters > 0u)
        ? 1.0f - (float)result.stats.uniqueClusterCount / (float)totalClusters
        : 0.0f;
    result.stats.leafClusterCount = levelCount > 0u ? result.levelClusterCount.front() : 0u;
    result.stats.rootClusterCount = levelCount > 0u ? result.levelClusterCount.back()  : 0u;

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
