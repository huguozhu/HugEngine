// ============================================================
// Nanite/NaniteUpload.cpp — 上传段（任务 1 生命周期桩）+ 离线簇切分（任务 8，CPU 侧）
//
// 【本文件由 §14.8 任务 1 建立骨架，任务 8/9/10 依次加入 CPU 侧构建，任务 12 加入资产入口】
//   · 任务 1：只有"记住设备与尺寸 / 清空"这几个动作，**没有任何 GPU 资源**。
//   · 任务 8：`BuildNaniteClusters()` —— `meshopt_buildMeshlets` 的落地（见头文件的接口说明）。
//   · 任务 9/10：`BuildNaniteClusterDAG()` / `PackNaniteClusters()` —— LOD+DAG 去重与量化打包。
//   · 任务 12：`BuildNaniteAssetFromGeometry()` —— 上面两者的顺序组合，即"资产加载"的 CPU 侧；
//     **GPU 上传与读回校验不在本文件**（device 调用会破坏本翻译单元的 RHI-free 纪律，
//     见 `Tests/CMakeLists.txt:50-54`），落在 `NaniteScene`（GPU 资源宿主）。
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
#include <cmath>           // std::sqrt / std::acos（量化误差实测）
#include <cstring>         // std::memcpy（任务 10：把各段铺进字节镜像）
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

/// 网格 AABB + **位置量化尺度**（任务 9 的 DAG 哈希与任务 10 的打包**共用这一个函数**）
///
/// 【为什么抽成共用函数】位置量化必须逐位可复现："DAG 的 `uniqueVertexWords`"与"打包出来的
///   `packedPosition`"是同一批数（打包器直接复用词），而它们的前提是**同一个量化尺度**。
///   把 AABB/尺度的算法放在一处，就不必靠人工同步两份内联代码（任务 9 原来内联在
///   `BuildNaniteClusterDAG` 里，任务 10 把它提出来）。
/// 【口径】`meshExtent = max(每轴范围)`（与 `meshopt_simplifyScale` 同口径，见头文件）；
///   退化网格（0 顶点 / 单点 / 全重合）⇒ 返回 **1.0f** 兜底，避免除以 0 的退化量化。
/// 【出参】`outMin` / `outMax` 各 3 个 float（**可空**：只关心尺度时传 nullptr）；空网格写 0。
[[nodiscard]] float ComputeMeshBounds(std::span<const float> positions,
                                      const usize            vertexCount,
                                      float* const           outMin,
                                      float* const           outMax) {
    float minValue[3] = { 0.0f, 0.0f, 0.0f };
    float maxValue[3] = { 0.0f, 0.0f, 0.0f };
    if (vertexCount > 0u) {
        minValue[0] = positions[0];
        minValue[1] = positions[1];
        minValue[2] = positions[2];
        maxValue[0] = minValue[0];
        maxValue[1] = minValue[1];
        maxValue[2] = minValue[2];
        for (usize v = 1; v < vertexCount; ++v) {
            for (u32 axis = 0; axis < 3u; ++axis) {
                const float value = positions[v * 3u + axis];
                if (value < minValue[axis]) minValue[axis] = value;
                if (value > maxValue[axis]) maxValue[axis] = value;
            }
        }
    }
    float extent = std::max(maxValue[0] - minValue[0],
                            std::max(maxValue[1] - minValue[1], maxValue[2] - minValue[2]));
    if (!(extent > 0.0f)) extent = 1.0f;   // 退化：给量化一个非退化尺度（与任务 9 的兜底一致）
    if (outMin != nullptr) { outMin[0] = minValue[0]; outMin[1] = minValue[1]; outMin[2] = minValue[2]; }
    if (outMax != nullptr) { outMax[0] = maxValue[0]; outMax[1] = maxValue[1]; outMax[2] = maxValue[2]; }
    return extent;
}

/// 【任务 18 / P0 修复】规范键流里"属性词段"的分隔标记
///
/// 取值是 ASCII 的 `'NAAT'`（Nanite Attribute Tag），与位置词（R10G10B10A2）和三角形键
/// （u16 三连）的值域完全不搭界 —— 它的作用只是让键流**自描述**：读到这个标记就知道
/// 后面是"顶点数 + 按局部下标顺序的法线词/UV 词"，人工排查时不必靠数偏移。
inline constexpr u32 kClusterCanonicalAttributeTag = 0x4E414154u;

/// 把一个簇的局部几何/拓扑/属性规范化成"顺序无关"的键流（用于哈希与命中后的精确比对）
///
/// 【键流布局】
/// ```text
/// [顶点数 N][排序后的 N 个位置词][三角形数 T][排序后的 T 个三角形键（各 3 个位置词）]
/// [属性标记 'NAAT'][顶点数 N][局部下标 0..N-1 的法线词、UV 词（各 1 个 u32）]
/// ```
/// 位置词 = 任务 7 的量化位置打包（R10G10B10A2），量化原点 = `record` 的簇 AABB 中心、
/// 尺度 = 整网格最大范围（口径说明见头文件"为什么是簇内局部"）。
/// 【三角形键】三种**循环旋转**里取字典序最小（保绕序，不做反转），再对全部键排序。
/// 【前两段顺序无关、第三段按下标有序（任务 18 的 P0 修复）】
///   · 位置词与三角形键按规范（排序/旋转归一）比较 ⇒ 顶点/三角形顺序无关，平移副本仍然命中；
///   · 属性词段**按局部下标逐位**写出 —— 这是刻意的：共享内容的顶点段只有**一份**、且按
///     "首次出现的局部下标顺序"存放，所以"共享"必须保证**每个局部下标上的属性都逐位相同**，
///     否则另一个顺序不同的出现会按同一份共享顶点读到**别的顶点的法线/UV**（§14.20⑥ 的缺陷）。
///   纳入属性词段的直接后果：只有"位置/拓扑同构 **且** 逐下标属性逐位相同"的簇才共享内容
///   （代价是去重率会下降，实测值见 §14.27；这是"改动最小、语义最直白"的修法）。
/// 【输出】`outWords` 是**原始顺序**的位置词（共享内容按原始顺序存放）；
///         `outKeys` 是规范键流（哈希与比对都用它）。
/// 【失败】局部下标越出本簇顶点数（防御性；正常不可达）⇒ 返回 false。
[[nodiscard]] bool BuildClusterCanonicalKeys(std::span<const float>     positions,
                                             std::span<const float>     normals,
                                             std::span<const float>     uvs,
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

    // ④ 【任务 18 / P0 修复】属性词段（法线词 + UV 词，按**局部下标顺序**逐位写出）
    //    【为什么顺序敏感】共享顶点段只有一份、按首次出现的局部下标顺序存放；着色器按
    //      `cluster.vertexOffset + 局部下标` 取属性。若两个"位置/拓扑同构"的簇的局部下标顺序
    //      不同却共享内容，就会出现"第 v 个局部下标读到别的顶点的法线/UV"——正是 §14.20⑥
    //      记录的缺陷。把属性词按局部下标写进键流 ⇒ 共享的前提变成"逐下标属性逐位相同"，
    //      缺陷结构上不可能再出现（打包器的 `attributeConflictCount` 因此必须恒为 0）。
    //    【口径与打包器逐字一致】法线 = `NanitePackNormal`（八面体 10+10 位）、
    //      UV = `NanitePackUV(NaniteQuantizeUV(...), ...)`（unorm16）——与 `PackNaniteClusters`
    //      的第 ④⑤ 步用的是同一组函数；`normals`/`uvs` 为空时用与打包器相同的默认值
    //      （法线 +Z、UV (0,0)），因此"不带属性"的调用方与今天的去重率**逐位不变**。
    outKeys.push_back(kClusterCanonicalAttributeTag);
    outKeys.push_back(localVertexCount);
    for (u32 v = 0; v < localVertexCount; ++v) {
        const u32 meshVertex = clusterVertexIndices[v];
        float nx = 0.0f;
        float ny = 0.0f;
        float nz = 1.0f;
        if (!normals.empty()) {
            const float* normal = normals.data() + (usize)meshVertex * 3u;
            nx = normal[0];
            ny = normal[1];
            nz = normal[2];
        }
        float u = 0.0f;
        float vv = 0.0f;
        if (!uvs.empty()) {
            const float* uv = uvs.data() + (usize)meshVertex * 2u;
            u  = uv[0];
            vv = uv[1];
        }
        outKeys.push_back(NanitePackNormal(nx, ny, nz));
        outKeys.push_back(NanitePackUV(NaniteQuantizeUV(u), NaniteQuantizeUV(vv)));
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
                           std::span<const float> normals,
                           std::span<const float> uvs,
                           std::span<const u32>   indices,
                           NaniteClusterDAG&      outResult) {
    // ── 输入校验：与任务 8 同口径；失败一律返回 false 且**不改写出参** ──
    if ((indices.size() % kIndicesPerTriangle) != 0u) return false;
    if ((positions.size() % 3u) != 0u)                return false;

    const usize vertexCount = positions.size() / 3u;
    // 【任务 18 / P0】属性必须是"每网格顶点"的完整记录（与 `PackNaniteClusters` 同一口径）；
    //   长度不对就拒绝，绝不按截断/越界读 —— 属性词段拿它算键流，错读会静默改变去重语义。
    if (!normals.empty() && normals.size() != vertexCount * 3u) return false;
    if (!uvs.empty()     && uvs.size()     != vertexCount * 2u) return false;
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
    //    算法与任务 10 的打包器**共用** `ComputeMeshBounds`（口径只此一处，见其注释）
    const float meshExtent = ComputeMeshBounds(positions, vertexCount, nullptr, nullptr);

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

            if (!BuildClusterCanonicalKeys(positions, normals, uvs, record, clusterVertexIndices,
                                           clusterTriangles, localVertexCount, localTriangleCount,
                                           meshExtent, words, keys)) {
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

/// 【兼容重载】不带属性的 DAG 构建（等价于 `normals`/`uvs` 都为空）
///
/// 【为什么保留】任务 9/10 的既有调用方与单测只关心"位置 + 拓扑"的去重；空属性 ⇒ 属性词段
///   是一段常量（法线 +Z、UV (0,0)）⇒ 去重语义与任务 9 当时**逐位相同**（键流长度变了、
///   哈希值变了，但等价关系不变）。这样旧调用方的去重率读数不受本次修复影响，
///   "属性一致性"只在真的传了属性的路径上生效（`BuildNaniteAssetFromGeometry` 恒传）。
bool BuildNaniteClusterDAG(std::span<const float> positions,
                           std::span<const u32>   indices,
                           NaniteClusterDAG&      outResult) {
    return BuildNaniteClusterDAG(positions, std::span<const float>{}, std::span<const float>{},
                                 indices, outResult);
}

// ============================================================
// §14.8 任务 10：量化与打包（最终 GPU 侧字节布局）
//
// 口径、边界、失败条件都写在 `NaniteUpload.h` 的同名小节里；这里只留与代码逐句对应的短注释。
// 一句话概括三段职责：① 位置**复用** DAG 的词并核验口径；② 法线/UV 由属性 span 现编；
// ③ 段表/字节镜像交给任务 7 的 `TryBuildNaniteFileLayout` + `ValidateNaniteFile` 兜底。
// ============================================================

//
// 口径、边界、失败条件都写在 `NaniteUpload.h` 的同名小节里；这里只留与代码逐句对应的短注释。
// 一句话概括三段职责：① 位置**复用** DAG 的词并核验口径；② 法线/UV 由属性 span 现编；
// ③ 段表/字节镜像交给任务 7 的 `TryBuildNaniteFileLayout` + `ValidateNaniteFile` 兜底。
// ============================================================
bool PackNaniteClusters(std::span<const float>                positions,
                        std::span<const float>                normals,
                        std::span<const float>                uvs,
                        std::span<const NaniteMaterialRecord> materials,
                        const NaniteClusterDAG&               dag,
                        NanitePackedAsset&                    outResult) {
    // ── 输入校验：失败一律返回 false 且**不改写出参**（与任务 7/8/9 同口径）──
    if ((positions.size() % 3u) != 0u) return false;                 // 位置必须是完整的 xyz
    const usize meshVertexCount = positions.size() / 3u;
    if (!normals.empty() && normals.size() != meshVertexCount * 3u) return false;
    if (!uvs.empty()     && uvs.size()     != meshVertexCount * 2u) return false;

    const usize uniqueCount     = dag.uniqueVertexCount.size();
    const usize occurrenceCount = dag.clusters.size();

    // DAG 自身的平行数组必须一一对应（防御性；正常由 `BuildNaniteClusterDAG` 保证）
    if (dag.uniqueVertexOffset.size()   != uniqueCount)     return false;
    if (dag.uniqueTriangleOffset.size() != uniqueCount)     return false;
    if (dag.uniqueTriangleCount.size()  != uniqueCount)     return false;
    if (occurrenceCount != dag.clusterVertexCount.size())        return false;
    if (occurrenceCount != dag.clusterVertexIndexOffset.size())  return false;
    if (occurrenceCount != dag.clusterUnique.size())             return false;
    // `levelClusterOffset` 比 `levelClusterCount` 多一个收尾元素（区间右端）；空 DAG 两者都为空
    if (occurrenceCount > 0u && dag.levelClusterOffset.size() != dag.levelClusterCount.size() + 1u) {
        return false;
    }

    // 共享内容表：每个唯一内容的顶点/三角形区间必须落在各自表内
    for (usize u = 0; u < uniqueCount; ++u) {
        if ((usize)dag.uniqueVertexOffset[u] + (usize)dag.uniqueVertexCount[u] >
            dag.uniqueVertexWords.size()) return false;
        if ((usize)dag.uniqueTriangleOffset[u] + (usize)dag.uniqueTriangleCount[u] >
            dag.uniqueTriangles.size()) return false;
    }

    // 出现记录：偏移/计数落在共享表内、局部顶点数在 (0,128]、网格顶点下标合法。
    // 这一段校验是"绝不越界读"的前提（下面所有循环都建立在它之上）。
    for (usize c = 0; c < occurrenceCount; ++c) {
        const u32 localVertices  = dag.clusterVertexCount[c];
        const u32 localTriangles = dag.clusters[c].triangleCount;
        if (localVertices == 0u || localVertices > kNaniteMaxClusterVertices) return false;
        if (localTriangles > kNaniteMaxClusterTriangles) return false;
        if ((usize)dag.clusterVertexIndexOffset[c] + (usize)localVertices >
            dag.clusterVertexIndices.size()) return false;
        if ((usize)dag.clusters[c].vertexOffset + (usize)localVertices >
            dag.uniqueVertexWords.size()) return false;
        if ((usize)dag.clusters[c].triangleOffset + (usize)localTriangles >
            dag.uniqueTriangles.size()) return false;
        const u32 unique = dag.clusterUnique[c];
        if ((usize)unique >= uniqueCount) return false;
        // 出现记录与它指向的共享内容必须同形（"共享内容"口径的守卫）
        if (localVertices  != dag.uniqueVertexCount[unique])   return false;
        if (localTriangles != dag.uniqueTriangleCount[unique]) return false;
        for (u32 v = 0; v < localVertices; ++v) {
            if ((usize)dag.clusterVertexIndices[dag.clusterVertexIndexOffset[c] + v] >=
                meshVertexCount) return false;
        }
    }

    NanitePackedAsset result;
    NanitePackStats&  stats = result.stats;

    // ── ① 网格量化范围（与任务 9 共用 `ComputeMeshBounds` ⇒ 位置词口径逐位一致）──
    stats.meshMaxExtent = ComputeMeshBounds(positions, meshVertexCount,
                                            stats.meshMin, stats.meshMax);
    // 位置误差上界 = 半个量化步 = (meshMaxExtent / 1022) / 2
    stats.positionErrorBound = stats.meshMaxExtent / (2.0f * (float)kNaniteVertexQuantFullScale);
    stats.normalAngleErrorBoundDegrees = kNaniteNormalAngleErrorBoundDegrees;
    stats.uvErrorBound = 1.0f / (float)kNaniteUVQuantMax;

    // ── ② 每个唯一内容的"**首次出现**"（内容哈希保证同形；取最小出现下标 ⇒ 确定）──
    //     量化原点与该份的属性来源都取首次出现：
    //     原点 = 该出现的簇心（`boundsCenterRadius.xyz`，任务 9 的哈希用的就是它）；
    //     属性 = 该出现的"局部顶点 → 网格顶点"映射。
    std::vector<u32> uniqueFirstCluster(uniqueCount, kNaniteNoParentCluster);
    for (u32 c = 0; c < (u32)occurrenceCount; ++c) {
        const u32 unique = dag.clusterUnique[c];
        if (uniqueFirstCluster[unique] == kNaniteNoParentCluster) uniqueFirstCluster[unique] = c;
    }
    for (usize u = 0; u < uniqueCount; ++u) {
        if (uniqueFirstCluster[u] == kNaniteNoParentCluster) return false;   // 无人引用的唯一内容
    }

    // ── ③ 簇段：拷贝出现记录；`materialID` 本任务统一写 0（归属任务 12/19 的材质解析）──
    //     `vertexOffset` / `triangleOffset` **原样保留**：它们与共享内容表的下标同位，
    //     在最终文件里就是"顶点段记录下标 / 索引段三角形下标"，不需要重映射。
    result.clusters = dag.clusters;
    for (NaniteClusterRecord& record : result.clusters) {
        record.materialID = 0u;   // 逐簇材质解析属任务 12/19；此处不伪造 ID
    }

    // ── ④ 顶点段：位置词**直接复用** DAG 的共享词（共享内容必须逐位相同），
    //      法线/UV 按首次出现的网格顶点现编；顺带核验口径与实测误差 ──
    result.vertices.resize(dag.uniqueVertexWords.size());
    for (usize u = 0; u < uniqueCount; ++u) {
        const u32 firstCluster  = uniqueFirstCluster[u];
        const NaniteClusterRecord& first = result.clusters[firstCluster];
        const float originX = first.boundsCenterRadius[0];
        const float originY = first.boundsCenterRadius[1];
        const float originZ = first.boundsCenterRadius[2];
        const u32   wordBase  = dag.uniqueVertexOffset[u];
        const u32   count     = dag.uniqueVertexCount[u];
        const u32   indexBase = dag.clusterVertexIndexOffset[firstCluster];

        for (u32 v = 0; v < count; ++v) {
            const u32    meshVertex = dag.clusterVertexIndices[indexBase + v];
            const float* position   = positions.data() + (usize)meshVertex * 3u;

            NaniteVertex vertex;
            vertex.packedPosition = dag.uniqueVertexWords[wordBase + v];   // 共享词（逐位一致）
            vertex.quantBias      = kNaniteVertexQuantBias;                // §8.4 定稿的 +512

            // 位置核验：用**本任务的口径**重算一遍，必须与 DAG 的词逐位相同
            const u32 rawX = NaniteQuantizePositionAxis(position[0], originX, stats.meshMaxExtent);
            const u32 rawY = NaniteQuantizePositionAxis(position[1], originY, stats.meshMaxExtent);
            const u32 rawZ = NaniteQuantizePositionAxis(position[2], originZ, stats.meshMaxExtent);
            if (NanitePackPosition(rawX, rawY, rawZ) != vertex.packedPosition) {
                ++stats.positionMismatchCount;   // 必须 0：否则说明两处量化口径已经漂了
            }
            // "无 clamp"的可测读数（按分量计）：簇心 + 网格最大范围口径下恒为 0
            if (NanitePositionQuantizeClamps(position[0], originX, stats.meshMaxExtent)) ++stats.positionClampCount;
            if (NanitePositionQuantizeClamps(position[1], originY, stats.meshMaxExtent)) ++stats.positionClampCount;
            if (NanitePositionQuantizeClamps(position[2], originZ, stats.meshMaxExtent)) ++stats.positionClampCount;

            // 位置往返误差实测：**从落盘的词解码**（GPU 侧将来读到的就是这个词）
            const float back[3] = {
                NaniteDequantizePositionAxis(NaniteUnpackR10G10B10A2(vertex.packedPosition, 0u),
                                             originX, stats.meshMaxExtent),
                NaniteDequantizePositionAxis(NaniteUnpackR10G10B10A2(vertex.packedPosition, 1u),
                                             originY, stats.meshMaxExtent),
                NaniteDequantizePositionAxis(NaniteUnpackR10G10B10A2(vertex.packedPosition, 2u),
                                             originZ, stats.meshMaxExtent),
            };
            for (u32 axis = 0; axis < 3u; ++axis) {
                const float error = std::fabs(back[axis] - position[axis]);
                if (error > stats.maxPositionError) stats.maxPositionError = error;
            }

            // 法线：八面体 10+10 位（`normals` 为空 ⇒ 默认 +Z，仍参与误差统计）
            float inputNx = 0.0f;
            float inputNy = 0.0f;
            float inputNz = 1.0f;
            if (!normals.empty()) {
                const float* normal = normals.data() + (usize)meshVertex * 3u;
                inputNx = normal[0];
                inputNy = normal[1];
                inputNz = normal[2];
            }
            vertex.packedNormal = NanitePackNormal(inputNx, inputNy, inputNz);

            // 法线角度误差实测：输入先归一化（解码结果已是单位向量），再取 acos(dot)
            const float normalLengthSquared = inputNx * inputNx + inputNy * inputNy + inputNz * inputNz;
            if (normalLengthSquared > 0.0f) {
                const float invLength = 1.0f / std::sqrt(normalLengthSquared);
                float decodedX = 0.0f;
                float decodedY = 0.0f;
                float decodedZ = 1.0f;
                NaniteUnpackNormal(vertex.packedNormal, decodedX, decodedY, decodedZ);
                const float radians = NaniteNormalAngleErrorRadians(inputNx * invLength,
                                                                   inputNy * invLength,
                                                                   inputNz * invLength,
                                                                   decodedX, decodedY, decodedZ);
                const float degrees = radians * (180.0f / 3.14159265358979323846f);
                if (degrees > stats.maxNormalAngleErrorDegrees) stats.maxNormalAngleErrorDegrees = degrees;
            }

            // UV：unorm16（`uvs` 为空 ⇒ 默认 (0,0)）；越界分量如实计数
            float inputU = 0.0f;
            float inputV = 0.0f;
            if (!uvs.empty()) {
                const float* uv = uvs.data() + (usize)meshVertex * 2u;
                inputU = uv[0];
                inputV = uv[1];
            }
            if (NaniteUVNeedsClamp(inputU)) ++stats.uvClampCount;
            if (NaniteUVNeedsClamp(inputV)) ++stats.uvClampCount;
            vertex.packedUV = NanitePackUV(NaniteQuantizeUV(inputU), NaniteQuantizeUV(inputV));

            // UV 误差实测：参考值取"clamp 到 [0,1] 后的输入"（在域内时它就是输入本身）
            const float referenceU = inputU < 0.0f ? 0.0f : (inputU > 1.0f ? 1.0f : inputU);
            const float referenceV = inputV < 0.0f ? 0.0f : (inputV > 1.0f ? 1.0f : inputV);
            const float uvErrorU = std::fabs(NaniteDequantizeUV(NaniteUnpackUVU(vertex.packedUV)) - referenceU);
            const float uvErrorV = std::fabs(NaniteDequantizeUV(NaniteUnpackUVV(vertex.packedUV)) - referenceV);
            if (uvErrorU > stats.maxUVError) stats.maxUVError = uvErrorU;
            if (uvErrorV > stats.maxUVError) stats.maxUVError = uvErrorV;

            result.vertices[wordBase + v] = vertex;
        }
    }

    // ── ⑤ 属性一致性检查：同一共享内容的**其它出现**是否给出同样的法线/UV 词 ──
    //     【任务 18 / P0 起这条检查是"硬门"而不是"如实报告"】属性词（法线 + UV，按局部下标）
    //     已经进了 DAG 的内容键流 ⇒ 共享的前提就是"逐局部下标的属性词逐位相同"，
    //     因此这里的冲突计数**必须为 0**；非 0 说明调用方绕过五参数重载手工拼了一份 DAG
    //     （只在单测/分析代码里可能），此时**拒绝产出**而不是静默写出一份属性错配的资产。
    std::vector<u8> uniqueConflict(uniqueCount, 0u);
    for (u32 c = 0; c < (u32)occurrenceCount; ++c) {
        const u32 unique    = dag.clusterUnique[c];
        const u32 count     = dag.clusterVertexCount[c];
        const u32 indexBase = dag.clusterVertexIndexOffset[c];
        const u32 wordBase  = dag.uniqueVertexOffset[unique];
        for (u32 v = 0; v < count; ++v) {
            const u32 meshVertex = dag.clusterVertexIndices[indexBase + v];
            float nx = 0.0f;
            float ny = 0.0f;
            float nz = 1.0f;
            if (!normals.empty()) {
                const float* normal = normals.data() + (usize)meshVertex * 3u;
                nx = normal[0];
                ny = normal[1];
                nz = normal[2];
            }
            float u = 0.0f;
            float vv = 0.0f;
            if (!uvs.empty()) {
                const float* uv = uvs.data() + (usize)meshVertex * 2u;
                u  = uv[0];
                vv = uv[1];
            }
            const u32 normalWord = NanitePackNormal(nx, ny, nz);
            const u32 uvWord     = NanitePackUV(NaniteQuantizeUV(u), NaniteQuantizeUV(vv));
            const NaniteVertex& written = result.vertices[wordBase + v];
            if (written.packedNormal != normalWord || written.packedUV != uvWord) {
                if (uniqueConflict[unique] == 0u) {
                    uniqueConflict[unique] = 1u;
                    ++stats.attributeConflictCount;   // 每个唯一内容只计一次
                }
            }
        }
    }
    if (stats.attributeConflictCount != 0u) {
        // 非 0 = "共享内容的不同出现给出不同属性" ⇒ 写进段里的属性必然对某些出现是错的。
        // 返回 false（调用方拿到的是"没有资产"，而不是一份静默错配的资产）。
        return false;
    }

    // ── ⑥ 索引段：直接复用共享三角形表，并逐条核验"簇内局部下标"的合法区间 ──
    //     合法前提是 `[0, min(127, 本唯一内容顶点数-1)]`：≥128 就不是 u16 打包语义下的
    //     合法簇内下标（任务 7 的 `IsValidClusterLocalVertexIndex`），越出本簇顶点数更是坏数据。
    result.triangles = dag.uniqueTriangles;
    for (usize u = 0; u < uniqueCount; ++u) {
        const u32 localVertexCount = dag.uniqueVertexCount[u];
        for (u32 t = 0; t < dag.uniqueTriangleCount[u]; ++t) {
            const NanitePackedTriangle& triangle =
                result.triangles[dag.uniqueTriangleOffset[u] + t];
            const u32 i0 = NaniteTriangleIndex0(triangle);
            const u32 i1 = NaniteTriangleIndex1(triangle);
            const u32 i2 = NaniteTriangleIndex2(triangle);
            if (!IsValidClusterLocalVertexIndex(i0) || !IsValidClusterLocalVertexIndex(i1) ||
                !IsValidClusterLocalVertexIndex(i2)) return false;
            if (i0 >= localVertexCount || i1 >= localVertexCount || i2 >= localVertexCount) return false;
        }
    }

    // ── ⑦ 材质段：原样搬运（不解析 ID、不做逐簇分配）──
    result.materials.assign(materials.begin(), materials.end());

    // ── ⑧ LOD 段：每级一个 u32 = **该级第一个出现簇的下标**（`levelClusterOffset[L]`）──
    //     任务 7 只定了"每个 LOD 一个 u32 偏移"的步长与条数，语义由任务 10 定为
    //     "该级出现簇区间的左端"：右端 = 下一条（末级用 `header.clusterCount`）⇒ 不需要哨兵元素。
    //     任务 15 的 LOD 选择可按它把"级"映射回簇下标区间。
    result.lodOffsets.reserve(dag.levelClusterCount.size());
    for (usize level = 0; level < dag.levelClusterCount.size(); ++level) {
        result.lodOffsets.push_back(dag.levelClusterOffset[level]);
    }

    // ── ⑨ 头部（计数/范围/误差/flags 全部有真值来源）──
    NaniteFileHeader header{};   // magic / version 已是任务 7 的定稿值
    header.clusterCount  = (u32)result.clusters.size();
    header.vertexCount   = (u32)result.vertices.size();
    header.indexCount    = (u32)result.triangles.size() * kNaniteIndicesPerTriangle;
    header.materialCount = (u32)result.materials.size();
    header.lodLevelCount = (u32)result.lodOffsets.size();
    header.flags         = kNaniteFileFlagHasDAG;   // 打包器只吃 DAG 产物 ⇒ 恒带簇图
    for (u32 axis = 0; axis < 3u; ++axis) {
        header.bboxMin[axis] = stats.meshMin[axis];
        header.bboxMax[axis] = stats.meshMax[axis];
    }
    header.maxLODError   = dag.stats.maxLODError;   // 任务 9 的绝对误差（保守上界）

    // ── ⑩ 段表：任务 7 的推导函数（起点 16B 对齐、长度向上取整到 16B）──
    NaniteFileLayout layout{};
    if (TryBuildNaniteFileLayout(header, layout) != NaniteFileError::None) return false;

    // 段字节读数 + "记录数 × 记录大小"的显式对账（公式写出来，任何一处步长改动都会在这里炸）
    const u64 rawClusterBytes  = (u64)result.clusters.size()  * (u64)kNaniteClusterRecordBytes;
    const u64 rawVertexBytes   = (u64)result.vertices.size()  * (u64)kNaniteVertexRecordBytes;
    const u64 rawIndexBytes    = (u64)result.triangles.size() * (u64)kNaniteIndexBytesPerTriangle;
    const u64 rawMaterialBytes = (u64)result.materials.size() * (u64)kNaniteMaterialRecordBytes;
    const u64 rawLodBytes      = (u64)result.lodOffsets.size() * (u64)kNaniteLodOffsetBytes;
    const u64 expectedTotal    = (u64)kNaniteFileHeaderBytes
                               + NaniteAlignUpFile(rawClusterBytes)
                               + NaniteAlignUpFile(rawVertexBytes)
                               + NaniteAlignUpFile(rawIndexBytes)
                               + NaniteAlignUpFile(rawMaterialBytes)
                               + NaniteAlignUpFile(rawLodBytes);
    if ((u64)layout.totalBytes != expectedTotal) return false;

    stats.clusterCount  = header.clusterCount;
    stats.vertexCount   = header.vertexCount;
    stats.triangleCount = (u32)result.triangles.size();
    stats.materialCount = header.materialCount;
    stats.lodLevelCount = header.lodLevelCount;
    stats.headerBytes   = layout.headerBytes;
    stats.clusterBytes  = layout.clusterBytes;
    stats.vertexBytes   = layout.vertexBytes;
    stats.indexBytes    = layout.indexBytes;
    stats.materialBytes = layout.materialBytes;
    stats.lodBytes      = layout.lodBytes;
    stats.rawBytes      = (usize)(rawClusterBytes + rawVertexBytes + rawIndexBytes +
                                  rawMaterialBytes + rawLodBytes);
    stats.totalBytes    = layout.totalBytes;

    // ── ⑪ 字节镜像（GPU 上传用的连续缓冲）+ 自校验 ──
    //     `std::vector<u8>` 的分配在本平台按 `__STDCPP_DEFAULT_NEW_ALIGNMENT__`（x64 = 16B）对齐，
    //     而各段偏移都是 16B 的整数倍 ⇒ 段内记录的对齐与"整块上传到 GPU"的要求一致。
    std::vector<u8> bytes(layout.totalBytes, 0u);
    std::memcpy(bytes.data(), &header, sizeof(header));
    if (!result.clusters.empty()) {
        std::memcpy(bytes.data() + layout.clusterOffset, result.clusters.data(),
                    result.clusters.size() * sizeof(NaniteClusterRecord));
    }
    if (!result.vertices.empty()) {
        std::memcpy(bytes.data() + layout.vertexOffset, result.vertices.data(),
                    result.vertices.size() * sizeof(NaniteVertex));
    }
    if (!result.triangles.empty()) {
        std::memcpy(bytes.data() + layout.indexOffset, result.triangles.data(),
                    result.triangles.size() * sizeof(NanitePackedTriangle));
    }
    if (!result.materials.empty()) {
        std::memcpy(bytes.data() + layout.materialOffset, result.materials.data(),
                    result.materials.size() * sizeof(NaniteMaterialRecord));
    }
    if (!result.lodOffsets.empty()) {
        std::memcpy(bytes.data() + layout.lodOffset, result.lodOffsets.data(),
                    result.lodOffsets.size() * sizeof(u32));
    }

    // 自校验：镜像必须是一份**合法且完整**的 `.nanite`（魔数/版本/3 的倍数/段对齐/不越界）
    NaniteFileLayout checked{};
    if (ValidateNaniteFile(bytes.data(), bytes.size(), &checked) != NaniteFileError::None) return false;
    if (checked.totalBytes != layout.totalBytes) return false;

    result.header = header;
    result.layout = layout;
    result.bytes  = std::move(bytes);

    outResult = std::move(result);   // 只有走到这里才动调用方的对象
    return true;
}

// ============================================================
// §14.8 任务 12：资产加载 —— 合并几何快照 → `.nanite` 字节镜像
//
// 只是任务 9 + 任务 10 的**顺序组合**（口径说明见 `NaniteUpload.h` 的同名小节）：
//   DAG（LOD 链 + 去重） → Pack（量化 + 打包 + 自校验）⇒ 一份可直接上传的字节镜像。
// 刻意不在这里加任何缓存/状态：本模块要的"只读一次合并几何"由调用方
// （`NaniteRenderer::EnsureAssetUploaded` 的一次性门闩）保证，而不是靠这里记住什么。
// ============================================================
bool BuildNaniteAssetFromGeometry(std::span<const float>                positions,
                                  std::span<const float>                normals,
                                  std::span<const float>                uvs,
                                  std::span<const u32>                  indices,
                                  std::span<const NaniteMaterialRecord> materials,
                                  NanitePackedAsset&                    outResult) {
    // ① LOD 链 + DAG 去重（任务 9）。失败时不改写出参，直接返回 false。
    //    【任务 18 / P0 修复】法线/UV 一并交给 DAG：内容哈希现在包含"按局部下标逐位的属性词"
    //    ⇒ 只有**全部属性逐位相同**的簇才会共享顶点/索引段，"共享内容属性错配"结构上不可能再出现。
    NaniteClusterDAG dag;
    if (!BuildNaniteClusterDAG(positions, normals, uvs, indices, dag)) return false;

    // ② 量化 + 打包 + 段表自校验（任务 10）。同样"先本地构建、成功才交出"。
    return PackNaniteClusters(positions, normals, uvs, materials, dag, outResult);
}

// ============================================================
// §14.8 任务 14：per-instance cluster BVH 的构建（CPU 侧）
//
// 口径（分裂策略 / 叶子容量 / 深度上限 / 包围球规则 / 确定性）全部写在
// `NaniteUpload.h` 的 "§14.8 任务 14" 小节，这里只留与代码逐句对应的短注释。
//
// 递归深度 ≤ kNaniteBVHMaxDepth（24），故用普通递归而不是显式栈 —— 调用栈吃不满 24 层；
// GPU 侧才必须用显式栈（shader 没有递归）。
// ============================================================
namespace {

/// 半径防御：NaN / 负值一律取 0
///
/// 与遍历判据（`NaniteSphereVisibleInFrustum` 把 `radius < 0` 归零）同口径 ⇒ 构建期算出的球
/// 不会比遍历期的判据"更大或更小"，不会出现两处口径不一致导致的边界翻转。
[[nodiscard]] inline float SanitizeClusterBVHRadius(float radius) {
    return (radius > 0.0f) ? radius : 0.0f;
}

/// 一组簇球的 AABB 包围球（`count == 0` ⇒ 退化为原点、半径 0）
///
/// 【为什么父球这样算】`center` = 全部球 AABB 的中心、`radius` = 到任一球边界的**最远**距离。
/// 父球因此恒包含所有子球（以及后代的簇球）⇒ "父球不可见 ⇒ 后代全不可见"这条早退是正确的。
/// 【确定性】只用 min/max/max，与遍历顺序无关（同类运算在 IEEE 下可交换/可结合地进行比较），
/// 故逐位可复现。
void ComputeSphereUnion(const u32* order, u32 begin, u32 end,
                        const std::vector<NaniteClusterSphere>& spheres,
                        float outCenter[3], float& outRadius) {
    outCenter[0] = outCenter[1] = outCenter[2] = 0.0f;
    outRadius = 0.0f;
    if (order == nullptr || end <= begin) return;

    float lo[3] = { 0.0f, 0.0f, 0.0f };
    float hi[3] = { 0.0f, 0.0f, 0.0f };
    for (u32 k = begin; k < end; ++k) {
        const NaniteClusterSphere& s = spheres[order[k]];
        for (u32 axis = 0; axis < 3u; ++axis) {
            const float a = s.center[axis] - s.radius;
            const float b = s.center[axis] + s.radius;
            if (k == begin) { lo[axis] = a; hi[axis] = b; }
            else { lo[axis] = (a < lo[axis]) ? a : lo[axis]; hi[axis] = (b > hi[axis]) ? b : hi[axis]; }
        }
    }
    for (u32 axis = 0; axis < 3u; ++axis) outCenter[axis] = (lo[axis] + hi[axis]) * 0.5f;

    float maxDistance = 0.0f;
    for (u32 k = begin; k < end; ++k) {
        const NaniteClusterSphere& s = spheres[order[k]];
        const float dx = s.center[0] - outCenter[0];
        const float dy = s.center[1] - outCenter[1];
        const float dz = s.center[2] - outCenter[2];
        // 不开方：比较平方距离后再开一次方，既少一次开方又不改变"取最远"的选择
        const float distanceSquared = dx * dx + dy * dy + dz * dz;
        const float reach = std::sqrt(distanceSquared) + s.radius;
        if (reach > maxDistance) maxDistance = reach;
    }
    outRadius = maxDistance;
}

/// 在一个 [begin, end) 的簇区间上递归构建 BVH，返回新建结点的下标
///
/// @param order          簇下标的工作数组（区间内会被本函数按分裂轴就地排序）
/// @param depth          该结点的深度（根 = 1）
/// @param maxDepth       出参：整棵树的最大深度
/// @param leafCount      出参：叶子数
/// @param maxLeafClusters 出参：实际最大叶子簇数
[[nodiscard]] u32 BuildClusterBVHNode(NaniteClusterBVH& bvh, u32* order, u32 begin, u32 end,
                                      u32 depth, u32& maxDepth, u32& leafCount,
                                      u32& maxLeafClusters) {
    const u32 clusterCount = end - begin;
    const u32 nodeIndex = (u32)bvh.nodes.size();
    bvh.nodes.emplace_back(NaniteBVHNode{});
    if (depth > maxDepth) maxDepth = depth;

    // ── 叶子判据：数量已够小，或已到深度硬上限（超上限就停止分裂 ⇒ 栈溢出变成构建期不变量）──
    if (clusterCount <= kNaniteBVHLeafCapacity || depth >= kNaniteBVHMaxDepth) {
        float center[3] = { 0.0f, 0.0f, 0.0f };
        float radius = 0.0f;
        ComputeSphereUnion(order, begin, end, bvh.clusterSpheres, center, radius);

        NaniteBVHNode& node = bvh.nodes[nodeIndex];
        node.center[0] = center[0];
        node.center[1] = center[1];
        node.center[2] = center[2];
        node.radius    = radius;
        node.left      = (u32)bvh.leafClusterIndices.size();   // 叶子簇表首下标
        node.right     = kNaniteBVHNoChild;
        node.count     = clusterCount;
        node.flags     = kNaniteBVHNodeFlagLeaf;
        // 叶子簇表按当前 order 顺序追加：`order` 的排列是确定性的，故产物逐位可复现。
        for (u32 k = begin; k < end; ++k) bvh.leafClusterIndices.push_back(order[k]);
        ++leafCount;
        if (clusterCount > maxLeafClusters) maxLeafClusters = clusterCount;
        return nodeIndex;
    }

    // ── 分裂轴：质心 AABB 上跨度最大的轴（标准 BVH 启发式）──
    float cmin[3] = { 0.0f, 0.0f, 0.0f };
    float cmax[3] = { 0.0f, 0.0f, 0.0f };
    for (u32 k = begin; k < end; ++k) {
        const NaniteClusterSphere& s = bvh.clusterSpheres[order[k]];
        for (u32 axis = 0; axis < 3u; ++axis) {
            if (k == begin) { cmin[axis] = s.center[axis]; cmax[axis] = s.center[axis]; }
            else {
                if (s.center[axis] < cmin[axis]) cmin[axis] = s.center[axis];
                if (s.center[axis] > cmax[axis]) cmax[axis] = s.center[axis];
            }
        }
    }
    u32   axis        = 0u;
    float bestExtent  = -1.0f;
    for (u32 a = 0; a < 3u; ++a) {
        const float extent = cmax[a] - cmin[a];
        // NaN 的 extent 与任何值比较都为 false ⇒ 不会成为 bestExtent，轴退化为 x
        if (extent > bestExtent) { bestExtent = extent; axis = a; }
    }
    const float splitPosition = (cmin[axis] + cmax[axis]) * 0.5f;

    // 排序的比较器带**下标兜底**：坐标相同时按下标，保证全序 ⇒ 排序结果唯一（可复现）
    std::sort(order + begin, order + end,
              [axis, &bvh](u32 lhs, u32 rhs) {
                  const float a = bvh.clusterSpheres[lhs].center[axis];
                  const float b = bvh.clusterSpheres[rhs].center[axis];
                  if (a < b) return true;
                  if (a > b) return false;
                  return lhs < rhs;
              });

    // 切点 = 第一个质心坐标 ≥ 中点的位置（质心 < 中点的全在左侧）
    u32 split = begin;
    while (split < end && bvh.clusterSpheres[order[split]].center[axis] < splitPosition) ++split;
    // 【回退：保证终止 + 保证平衡】两种情形都退回"按数量中位数"（前半 n/2）：
    //   ① 一侧为空（质心全相同 / 极密集 / NaN）—— 中点分裂永远要有进展；
    //   ② 中点分裂**过偏**（任一侧不足 n/3）—— 这是中点分裂的真实风险：Sponza 这类
    //      "少量离群簇 + 一大团"的分布会让中点一次只切掉 1~2 个簇，树深退化到深度上限
    //      （实测未加护栏时 8287 簇的树深恰好顶到 24、叶子数 3103 ⇒ 大量 1~2 簇的叶子）。
    //      加了 n/3 护栏后每次分裂都把规模压到 ≤ 2n/3 ⇒ 深度 ≤ 1 + log_{1.5}(n / 叶子容量)，
    //      对 n ≤ 16384（`kNaniteMaxBVHClusters`）恒 ≤ 22 < 24 ⇒ **深度上限重新变成安全网**，
    //      而不是形状的决定因素。单测直接断言这条上界。
    const u32 kMinSideCount = (clusterCount + 2u) / 3u;   // ceil(n/3)
    if (split == begin || split == end ||
        (split - begin) < kMinSideCount || (end - split) < kMinSideCount) {
        split = begin + clusterCount / 2u;
    }

    // ── 递归两个孩子，然后用两个孩子的球重算本结点的球（父球包含孩子球 ⇒ 早退正确）──
    const u32 left  = BuildClusterBVHNode(bvh, order, begin, split, depth + 1u,
                                          maxDepth, leafCount, maxLeafClusters);
    const u32 right = BuildClusterBVHNode(bvh, order, split, end, depth + 1u,
                                          maxDepth, leafCount, maxLeafClusters);

    // 两个孩子的球（顺序固定为 左、右 ⇒ 并集与逐位结果确定）
    const float* childCenter[2] = { bvh.nodes[left].center, bvh.nodes[right].center };
    const float  childRadius[2] = { bvh.nodes[left].radius, bvh.nodes[right].radius };
    float parentCenter[3];
    float parentRadius = 0.0f;
    {
        for (u32 a = 0; a < 3u; ++a) {
            const float lo = (childCenter[0][a] - childRadius[0] < childCenter[1][a] - childRadius[1])
                           ? childCenter[0][a] - childRadius[0] : childCenter[1][a] - childRadius[1];
            const float hi = (childCenter[0][a] + childRadius[0] > childCenter[1][a] + childRadius[1])
                           ? childCenter[0][a] + childRadius[0] : childCenter[1][a] + childRadius[1];
            parentCenter[a] = (lo + hi) * 0.5f;
        }
        float maxReach = 0.0f;
        for (u32 c = 0; c < 2u; ++c) {
            const float dx = childCenter[c][0] - parentCenter[0];
            const float dy = childCenter[c][1] - parentCenter[1];
            const float dz = childCenter[c][2] - parentCenter[2];
            const float reach = std::sqrt(dx * dx + dy * dy + dz * dz) + childRadius[c];
            if (reach > maxReach) maxReach = reach;
        }
        parentRadius = maxReach;
    }

    NaniteBVHNode& node = bvh.nodes[nodeIndex];
    node.center[0] = parentCenter[0];
    node.center[1] = parentCenter[1];
    node.center[2] = parentCenter[2];
    node.radius    = parentRadius;
    node.left      = left;
    node.right     = right;
    node.count     = 2u;   // 内部结点恒有 2 个孩子（分裂保证两侧非空）
    node.flags     = 0u;
    return nodeIndex;
}

} // namespace

bool BuildNaniteClusterBVH(std::span<const NaniteClusterRecord> clusters,
                           NaniteClusterBVH&                    outResult) {
    NaniteClusterBVH result;
    const u32 clusterCount = (u32)clusters.size();
    result.clusterCount = clusterCount;

    if (clusterCount > 0u) {
        // ① 簇球表：从任务 9/10 的 `boundsCenterRadius` **逐位搬运**（CPU 参考遍历与 GPU 读同一份比特）
        result.clusterSpheres.resize(clusterCount);
        for (u32 i = 0; i < clusterCount; ++i) {
            const NaniteClusterRecord& record = clusters[i];
            result.clusterSpheres[i].center[0] = record.boundsCenterRadius[0];
            result.clusterSpheres[i].center[1] = record.boundsCenterRadius[1];
            result.clusterSpheres[i].center[2] = record.boundsCenterRadius[2];
            result.clusterSpheres[i].radius    = SanitizeClusterBVHRadius(record.boundsCenterRadius[3]);
        }

        // ② 递归构建。节点数上界 = 2 × 叶子数 - 1 ≤ 2 × 簇数 - 1（每个叶子至少 1 个簇）
        result.nodes.reserve((usize)clusterCount * 2u);
        result.leafClusterIndices.reserve(clusterCount);

        std::vector<u32> order(clusterCount);
        for (u32 i = 0; i < clusterCount; ++i) order[i] = i;

        u32 maxDepth = 0u;
        u32 leafCount = 0u;
        u32 maxLeafClusters = 0u;
        BuildClusterBVHNode(result, order.data(), 0u, clusterCount, 1u,
                            maxDepth, leafCount, maxLeafClusters);

        result.leafCount            = leafCount;
        result.depth                = maxDepth;
        result.maxLeafClusterCount  = maxLeafClusters;
        // DFS 显式栈的占用上界 = 树高（每层至多压入一个"待访问的右兄弟"）
        result.maxStackDepthUpperBound = maxDepth;

        if (result.nodes.size() != (usize)leafCount * 2u - 1u) return false;   // 自校验：满二叉树
        if (maxDepth > kNaniteBVHMaxDepth) return false;                        // 自校验：深度上界
    }

    outResult = std::move(result);   // 只有走到这里才动调用方的对象
    return true;
}

// ============================================================
// §14.8 任务 15：每簇 LOD 元数据（Phase 3 的 DAG 割判据的输入）
// 口径与失败条件写在 `NaniteUpload.h` 的同名小节里；这里只留与代码逐句对应的短注释。
// ============================================================
bool BuildNaniteClusterLODInfo(std::span<const NaniteClusterRecord> clusters,
                               std::span<const u32>                 lodOffsets,
                               std::vector<NaniteClusterLODInfo>&   outResult) {
    const u32 clusterCount = (u32)clusters.size();

    // ── ① LOD 段自校验（单调不减、首元素为 0、元素都 < 簇数）──
    // 段本身由任务 10 的 `PackNaniteClusters` 从 DAG 的 `levelClusterOffset` 拷来；这里再查一次
    // 是为了让"元数据错位"这类问题在**构建点**就暴露，而不是变成运行期的错误 LOD 选择。
    for (usize i = 0; i < lodOffsets.size(); ++i) {
        if (lodOffsets[i] > clusterCount) return false;
        if (i > 0u && lodOffsets[i] < lodOffsets[i - 1u]) return false;
    }

    std::vector<NaniteClusterLODInfo> result(clusterCount);
    if (clusterCount == 0u) {
        outResult = std::move(result);   // 空输入是合法输入（与任务 7/9/10 同口径）
        return true;
    }

    // ── ② 根簇判定：被任何簇引为孩子的簇不是根 ──
    std::vector<u8> hasParent(clusterCount, 0u);
    for (u32 i = 0; i < clusterCount; ++i) {
        const NaniteClusterRecord& record = clusters[i];
        if (record.childCount == 0u) continue;
        // 越界防御：`childClusterOffset + childCount` 必须落在簇表内（否则返回 false，不改写出参）
        if (record.childClusterOffset > clusterCount
            || record.childCount > clusterCount - record.childClusterOffset) {
            return false;
        }
        for (u32 k = 0u; k < record.childCount; ++k) {
            hasParent[record.childClusterOffset + k] = 1u;
        }
    }

    // ── ③ 逐簇填元数据 ──
    u32 levelCursor = 0u;   // `lodOffsets` 的游标（簇表按级升序 ⇒ 单次线性扫描即可）
    for (u32 i = 0; i < clusterCount; ++i) {
        const NaniteClusterRecord& record = clusters[i];
        NaniteClusterLODInfo& info = result[i];

        // LOD 级：满足 lodOffsets[L] <= i 的最大 L（空段 ⇒ 全部算 0 级）
        if (!lodOffsets.empty()) {
            while (levelCursor + 1u < (u32)lodOffsets.size() && lodOffsets[levelCursor + 1u] <= i) {
                ++levelCursor;
            }
        }
        info.lodLevel = levelCursor;

        // ownError = 用本簇替代其孩子渲染的误差 = 孩子的 `maxParentLODError`（叶子 = 0）
        info.ownError = (record.childCount > 0u)
            ? clusters[record.childClusterOffset].maxParentLODError : 0.0f;
        if (!(info.ownError > 0.0f)) info.ownError = 0.0f;   // NaN / 负值一律归 0（防御）

        // parentError = 用父簇替代本簇渲染的误差 = 本簇自己的 `maxParentLODError`（根 = 0）
        info.parentError = (record.maxParentLODError > 0.0f) ? record.maxParentLODError : 0.0f;

        info.flags = (hasParent[i] == 0u) ? kNaniteLODInfoFlagRoot : 0u;
    }

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
