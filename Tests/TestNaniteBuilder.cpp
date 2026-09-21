// ============================================================
// Tests/TestNaniteBuilder.cpp — 离线 cluster 切分单测（§14.8 任务 8）
//
// 【为什么单独建这个文件】`Tests/TestNaniteTypes.cpp` 覆盖的是"任务 7 的纯数据格式"（RHI-free、
//   不编译任何 .cpp）；任务 8 的簇切分需要**真的调用** `BuildNaniteClusters()`，因此本文件
//   与被测实现 `Engine/Render/Nanite/NaniteUpload.cpp` 一起编进 `HugEngineTests`
//   （登记见 `Tests/CMakeLists.txt`），并链接 meshoptimizer v0.22。
//   被测实现是 RHI-free 的（`NaniteUpload.h` 只前置声明 `rhi::IRHIDevice`），所以本文件
//   **仍然不需要链接 HugEngineRender**。
//
// 覆盖范围（验收判据 = 每网格簇数 / 每簇三角形上限 / 无退化簇，另加覆盖完整性与可复现性）：
//   1. 规则网格 6×6 四边形（72 三角形）：簇数 > 0、≤64 tri / ≤128 vert、退化簇 = 0
//   2. 稠密网格 32×32 四边形（2048 三角形）：同上，并打印簇数统计
//   3. 三角形覆盖完整性：所有输入三角形恰好出现在一个簇里（不丢不重、绕序保留）
//   4. 可复现性：同一输入两次切分逐位一致（簇数、记录、顶点表、三角形表、统计）
//   5. 边界：空网格（0 三角形）⇒ 成功且产物为空
//   6. 边界：不足一个整簇的小网格（1 个三角形 / 18 个三角形）⇒ 恰好 1 簇
//   7. 边界：恰好一个整簇（8×4 四边形 = 64 三角形）⇒ 恰好 1 簇且打满 64
//   8. 非法输入 ⇒ 返回 false 且**不改写**出参
//   9. 包围球 / 锥字段：字段合法（`IsValidConeAxisAngle`）、顶点都落在包围球内、无锥哨兵映射
//  10. 与任务 7 的 `.nanite` 布局兼容：计数 → 段表 → `ValidateNaniteFile` 全通过、偏移不越界
//
// §14.8 任务 9（本文件后半段）追加 LOD 链 + DAG 去重的用例：
//  11. LOD 链：`levelCount > 0`、逐级减半的单调性、终止阈值；每簇仍 ≤64 tri / ≤128 vert
//  12. DAG 去重率：**(a) 平铺的相同子网格**（期望 > 10%）与 **(b) 一般网格**（如实报告）两类数字
//  13. DAG 链接自洽：`childClusterOffset/childCount` 不越界、每个子簇恰有一个父、无孤儿、无环
//  14. 共享内容一致：出现记录 → 唯一内容的偏移/计数自洽，`maxParentLODError` 非负、根为 0
//  15. 可复现性 + 边界（空网格 / 非法输入 / 凑不出一个满簇的单三角形）
//
// §14.8 任务 10（量化与打包）追加的用例：
//  16. 段布局：偏移/长度/对齐/总字节数（记录数 × 记录大小 + 16B 对齐）、字节镜像逐字节一致、
//      `ValidateNaniteFile` 通过、LOD 段语义、材质段原样搬运
//  17. 量化误差实测：位置（≤ meshMaxExtent/2044，实测最大值见 MESSAGE）、法线（≤ 0.5°，实测）、
//      UV（≤ 1/65535，实测）；**无 clamp**（`positionClampCount == 0`）与"与 DAG 位置词一致"
//  18. 与 DAG 的衔接：平铺网格（有共享内容）下逐簇解码回输入几何、同一 `vertexOffset` 被多次引用
//  19. 【任务 18 / P0】共享簇必须属性逐位一致：属性词进内容键流 ⇒ `attributeConflictCount == 0`、
//      逐"出现 × 局部下标"重算属性词逐位相同；每片 UV 平移的网格不再共享、逐片真副本仍 > 10% 去重
//  20. 可复现性 + 失败路径（空 DAG / 位置或属性长度非法 / DAG 内部不一致 / 属性 span 可空）
//
// §14.8 任务 12（资产加载的 CPU 侧入口）追加的用例：
//  21. `BuildNaniteAssetFromGeometry`：与"手工 DAG + Pack"逐字节一致、产物自洽可校验、
//      确定性、空几何 ⇒ 96B 空资产、失败不改写出参
//
// §14.8 任务 14（per-instance cluster BVH 的构建）追加的用例：
//  22. `NaniteBVH:` 节点数/深度上界、叶子容量、结构自洽（叶子簇表是排列 / 父球包含孩子球）、
//      三种网格规模、两次构建逐位可复现、空表与单簇边界、DFS 遍历访问数（全部在内/全部在外/
//      部分相交/多实例/实例域钳制/容量截断）
//
// §14.8 任务 19 / 25（簇 → 源网格 → 材质）追加的用例：
//  23. `NaniteMaterialMap:` 低层规则（三角形空间的多数票 / 跨网格计数 / 平票取小下标 / 防御）
//  24. `NaniteMaterialMap:` 任务 25 回归 —— 平移副本（去重命中）的多源网格资产上，
//      `BuildNaniteAssetFromGeometry` 的带材质重载必须 `unmapped == 0`、且逐三角形手工期望的
//      材质零错；真实 Sponza 资产的同类回归在 `Tests/TestNaniteMaterialMap.cpp`。
// ============================================================

#include "doctest.h"

#include "Nanite/NaniteUpload.h"   // BuildNaniteClusters（任务 8）/ BuildNaniteClusterDAG（任务 9）

#include <algorithm>   // std::sort
#include <array>       // std::array（三角形键）
#include <cmath>       // std::fabs / std::sqrt
#include <cstring>     // std::memcmp / std::memcpy
#include <string>      // std::to_string（统计行）
#include <vector>

using namespace he;
using namespace he::render;

namespace {

// ============================================================
// 【§14.8 任务 19】测试用材质记录构造（32B：因子 + 纹理掩码 + bindless 基索引）
//   与 `NaniteMakeMaterialRecord` 同义，只是把 4 个字段塞进两个参数，便于老用例迁移。
// ============================================================
inline NaniteMaterialRecord NaniteMakeTestMaterial(u32 bindlessTextureBase, u32 textureMask) {
    const float factor[4] = { 0.5f, 0.6f, 0.7f, 1.0f };
    return NaniteMakeMaterialRecord(factor, 0.25f, 0.75f, textureMask, bindlessTextureBase);
}

// ============================================================
// 测试用网格生成（确定性；不依赖任何资产文件）
// ============================================================

/// 规则网格：`quadsX × quadsY` 个四边形（XY 平面，z = 0），顶点共享
struct GridMesh {
    std::vector<float> positions;   ///< 每顶点 3 个 float
    std::vector<u32>   indices;     ///< 三角形列表（长度 = 三角形数 × 3）
};

GridMesh MakeGridRect(u32 quadsX, u32 quadsY) {
    GridMesh mesh;
    const u32 side = quadsX + 1u;   // 每行顶点数
    mesh.positions.reserve((usize)side * (quadsY + 1u) * 3u);
    for (u32 y = 0; y <= quadsY; ++y) {
        for (u32 x = 0; x < side; ++x) {
            mesh.positions.push_back((float)x);
            mesh.positions.push_back((float)y);
            mesh.positions.push_back(0.0f);
        }
    }
    mesh.indices.reserve((usize)quadsX * quadsY * 6u);
    for (u32 y = 0; y < quadsY; ++y) {
        for (u32 x = 0; x < quadsX; ++x) {
            const u32 i0 = y * side + x;          // 左下
            const u32 i1 = i0 + 1u;               // 右下
            const u32 i2 = i0 + side;             // 左上
            const u32 i3 = i2 + 1u;               // 右上
            // 两个三角形，绕序一致（+Z 法线）
            mesh.indices.push_back(i0); mesh.indices.push_back(i1); mesh.indices.push_back(i3);
            mesh.indices.push_back(i0); mesh.indices.push_back(i3); mesh.indices.push_back(i2);
        }
    }
    return mesh;
}

/// 正方形网格：`quadsPerSide²` 个四边形 = `2 × quadsPerSide²` 个三角形
GridMesh MakeGrid(u32 quadsPerSide) { return MakeGridRect(quadsPerSide, quadsPerSide); }

/// 单位立方体（8 个共享顶点、12 个三角形）：六个面的法线铺满 ±x/±y/±z，
/// 使得 meshopt 的"法线锥"无可用的单位轴 ⇒ **确定性地**覆盖"无锥哨兵"这条映射分支。
GridMesh MakeCube() {
    GridMesh mesh;
    const float p[8][3] = {
        { -1.0f, -1.0f, -1.0f }, {  1.0f, -1.0f, -1.0f },
        {  1.0f,  1.0f, -1.0f }, { -1.0f,  1.0f, -1.0f },
        { -1.0f, -1.0f,  1.0f }, {  1.0f, -1.0f,  1.0f },
        {  1.0f,  1.0f,  1.0f }, { -1.0f,  1.0f,  1.0f },
    };
    for (const auto& v : p) {
        mesh.positions.push_back(v[0]);
        mesh.positions.push_back(v[1]);
        mesh.positions.push_back(v[2]);
    }
    const u32 faces[12][3] = {
        { 4, 5, 6 }, { 4, 6, 7 },   // +Z
        { 1, 0, 3 }, { 1, 3, 2 },   // -Z
        { 0, 4, 7 }, { 0, 7, 3 },   // -X
        { 5, 1, 2 }, { 5, 2, 6 },   // +X
        { 0, 1, 5 }, { 0, 5, 4 },   // -Y
        { 7, 6, 2 }, { 7, 2, 3 },   // +Y
    };
    for (const auto& f : faces) {
        mesh.indices.push_back(f[0]);
        mesh.indices.push_back(f[1]);
        mesh.indices.push_back(f[2]);
    }
    return mesh;
}

// ============================================================
// 检查辅助
// ============================================================

/// 统计行（验收要的读数；用 MESSAGE 打印到测试输出里）
std::string StatsLine(const char* label, const NaniteClusterBuild& build) {
    const NaniteClusterBuildStats& s = build.stats;
    return std::string(label) +
           " 簇数=" + std::to_string(s.clusterCount) +
           " 三角形总数=" + std::to_string(s.triangleCount) +
           " 顶点表条目=" + std::to_string(s.vertexCount) +
           " 最大每簇三角形=" + std::to_string(s.maxClusterTriangles) +
           " 最大每簇顶点=" + std::to_string(s.maxClusterVertices) +
           " 退化簇=" + std::to_string(s.degenerateClusterCount) +
           " 无锥簇=" + std::to_string(s.noConeClusterCount);
}

/// 每条记录的"硬上限 + 退化"检查（所有正向用例共用）
bool AllClustersRespectCaps(const NaniteClusterBuild& build) {
    if (build.clusters.size() != build.clusterVertexCount.size()) return false;
    if (build.stats.clusterCount != (u32)build.clusters.size()) return false;
    for (usize c = 0; c < build.clusters.size(); ++c) {
        const u32 localTriangles = build.clusters[c].triangleCount;
        const u32 localVertices  = build.clusterVertexCount[c];
        if (localTriangles == 0u || localTriangles > kNaniteMaxClusterTriangles) return false;
        if (localVertices  == 0u || localVertices  > kNaniteMaxClusterVertices)  return false;
        // 三角形/顶点偏移必须落在各自的表内，且不能越界
        if (build.clusters[c].triangleOffset + localTriangles >
            (u32)build.triangles.size()) return false;
        if (build.clusters[c].vertexOffset + localVertices >
            (u32)build.vertexIndices.size()) return false;
    }
    return true;
}

/// 把产物里的打包三角形还原成"网格顶点三元组"（有序 ⇒ 同时校验绕序保留）
using TriangleKey = std::array<u32, 3>;

std::vector<TriangleKey> CollectMeshTriangles(const NaniteClusterBuild& build) {
    std::vector<TriangleKey> keys;
    keys.reserve(build.triangles.size());
    for (usize c = 0; c < build.clusters.size(); ++c) {
        const NaniteClusterRecord& record = build.clusters[c];
        for (u32 t = 0; t < record.triangleCount; ++t) {
            const NanitePackedTriangle& packed = build.triangles[record.triangleOffset + t];
            const u32 i0 = NaniteTriangleIndex0(packed);
            const u32 i1 = NaniteTriangleIndex1(packed);
            const u32 i2 = NaniteTriangleIndex2(packed);
            // 簇内局部下标必须先落在本簇的顶点范围内，才能经 vertexOffset 换成网格顶点
            REQUIRE(i0 < build.clusterVertexCount[c]);
            REQUIRE(i1 < build.clusterVertexCount[c]);
            REQUIRE(i2 < build.clusterVertexCount[c]);
            keys.push_back(TriangleKey{
                build.vertexIndices[record.vertexOffset + i0],
                build.vertexIndices[record.vertexOffset + i1],
                build.vertexIndices[record.vertexOffset + i2],
            });
        }
    }
    return keys;
}

/// 输入索引表 → 网格顶点三元组
std::vector<TriangleKey> CollectInputTriangles(std::span<const u32> indices) {
    std::vector<TriangleKey> keys;
    keys.reserve(indices.size() / 3u);
    for (usize i = 0; i + 2u < indices.size(); i += 3u) {
        keys.push_back(TriangleKey{ indices[i], indices[i + 1u], indices[i + 2u] });
    }
    return keys;
}

/// 两次切分是否逐位一致（簇记录按字节比，因为它们是纯 POD 且无未初始化填充）
bool SameBuild(const NaniteClusterBuild& a, const NaniteClusterBuild& b) {
    if (a.clusters.size() != b.clusters.size()) return false;
    if (a.clusterVertexCount != b.clusterVertexCount) return false;
    if (a.vertexIndices != b.vertexIndices) return false;
    if (a.triangles.size() != b.triangles.size()) return false;
    if (!a.clusters.empty() &&
        std::memcmp(a.clusters.data(), b.clusters.data(),
                    a.clusters.size() * sizeof(NaniteClusterRecord)) != 0) return false;
    if (!a.triangles.empty() &&
        std::memcmp(a.triangles.data(), b.triangles.data(),
                    a.triangles.size() * sizeof(NanitePackedTriangle)) != 0) return false;
    return std::memcmp(&a.stats, &b.stats, sizeof(NaniteClusterBuildStats)) == 0;
}

} // namespace

// ============================================================
// 1. 规则网格 6×6 四边形（72 三角形）
// ============================================================
TEST_CASE("NaniteBuilder: 6×6 网格（72 三角形）簇数与上限") {
    const GridMesh mesh = MakeGrid(6);
    REQUIRE(mesh.indices.size() == 216u);   // 72 个三角形

    NaniteClusterBuild build;
    REQUIRE(BuildNaniteClusters(mesh.positions, mesh.indices, build));

    MESSAGE(StatsLine("6x6 网格（72 tri / 49 vert）:", build).c_str());

    CHECK(build.stats.triangleCount == 72u);                                  // 不丢三角形
    CHECK(build.stats.vertexCount == (u32)build.vertexIndices.size());
    CHECK(build.stats.clusterCount > 0u);
    CHECK(build.stats.clusterCount <= 72u);                                   // 每簇至少 1 个三角形
    CHECK(build.stats.maxClusterTriangles <= kNaniteMaxClusterTriangles);     // ≤64
    CHECK(build.stats.maxClusterVertices  <= kNaniteMaxClusterVertices);      // ≤128
    CHECK(build.stats.degenerateClusterCount == 0u);                          // 无退化簇
    CHECK(AllClustersRespectCaps(build));
}

// ============================================================
// 2. 稠密网格 32×32 四边形（2048 三角形）—— 打印验收读数
// ============================================================
TEST_CASE("NaniteBuilder: 32×32 网格（2048 三角形）簇数与上限") {
    const GridMesh mesh = MakeGrid(32);
    REQUIRE(mesh.indices.size() == 2048u * 3u);

    NaniteClusterBuild build;
    REQUIRE(BuildNaniteClusters(mesh.positions, mesh.indices, build));

    MESSAGE(StatsLine("32x32 网格（2048 tri / 1089 vert）:", build).c_str());

    CHECK(build.stats.triangleCount == 2048u);
    // 下界由"每簇 ≤64 三角形"直接推出：2048 / 64 = 32 个簇起步
    CHECK(build.stats.clusterCount >= 32u);
    CHECK(build.stats.clusterCount <= 2048u);
    CHECK(build.stats.maxClusterTriangles <= kNaniteMaxClusterTriangles);
    CHECK(build.stats.maxClusterVertices  <= kNaniteMaxClusterVertices);
    CHECK(build.stats.degenerateClusterCount == 0u);
    // 每簇三角形数/顶点数的**逐簇**读数（不只是最大值）
    CHECK((u32)build.clusterVertexCount.size() == build.stats.clusterCount);
    u32 counted = 0;
    for (usize c = 0; c < build.clusters.size(); ++c) {
        counted += build.clusters[c].triangleCount;
        CHECK(build.clusterVertexCount[c] > 0u);
    }
    CHECK(counted == build.stats.triangleCount);   // Σ 每簇三角形数 = 总数
    CHECK(AllClustersRespectCaps(build));
}

// ============================================================
// 3. 三角形覆盖完整性（不丢不重、绕序保留）
// ============================================================
TEST_CASE("NaniteBuilder: 三角形覆盖完整性（有序三元组多重集相等）") {
    const GridMesh mesh = MakeGrid(32);

    NaniteClusterBuild build;
    REQUIRE(BuildNaniteClusters(mesh.positions, mesh.indices, build));

    std::vector<TriangleKey> fromBuild = CollectMeshTriangles(build);
    std::vector<TriangleKey> fromInput = CollectInputTriangles(mesh.indices);

    CHECK(fromBuild.size() == 2048u);
    CHECK(fromInput.size() == 2048u);
    CHECK(fromBuild.size() == fromInput.size());

    // 有序比较：既证明"不丢不重"，也证明每个三角形的绕序与输入一致
    std::sort(fromBuild.begin(), fromBuild.end());
    std::sort(fromInput.begin(), fromInput.end());
    const bool identical = (fromBuild == fromInput);
    CHECK(identical);
}

// ============================================================
// 4. 可复现性（同一输入两次切分逐位一致）
// ============================================================
TEST_CASE("NaniteBuilder: 同一输入两次切分逐位可复现") {
    const GridMesh mesh = MakeGrid(16);   // 512 三角形

    NaniteClusterBuild first;
    NaniteClusterBuild second;
    REQUIRE(BuildNaniteClusters(mesh.positions, mesh.indices, first));
    REQUIRE(BuildNaniteClusters(mesh.positions, mesh.indices, second));

    CHECK(first.stats.clusterCount == second.stats.clusterCount);
    CHECK(first.clusters.size() == second.clusters.size());
    // 逐位一致：簇记录（含 bounds/cone/偏移/计数）、顶点表、三角形表、统计
    CHECK(SameBuild(first, second));

    // 关键几个簇的读数（验收要求"前若干簇的顶点/三角形计数一致"）
    const usize probe = first.clusters.size() < 8u ? first.clusters.size() : 8u;
    for (usize c = 0; c < probe; ++c) {
        CHECK(first.clusters[c].triangleCount == second.clusters[c].triangleCount);
        CHECK(first.clusters[c].triangleOffset == second.clusters[c].triangleOffset);
        CHECK(first.clusters[c].vertexOffset == second.clusters[c].vertexOffset);
        CHECK(first.clusterVertexCount[c] == second.clusterVertexCount[c]);
    }
}

// ============================================================
// 5. 边界：空网格（0 三角形）
// ============================================================
TEST_CASE("NaniteBuilder: 空网格（0 三角形）成功且产物为空") {
    const std::vector<float> positions;   // 无顶点
    const std::vector<u32>   indices;     // 无三角形

    NaniteClusterBuild build;
    // 先污染出参：成功路径必须"整体写满"（而不是把旧内容留在里面）
    build.stats.clusterCount = 7u;
    build.clusters.resize(2u);
    build.clusterVertexCount.push_back(99u);

    CHECK(BuildNaniteClusters(positions, indices, build));
    CHECK(build.Empty());
    CHECK(build.clusters.empty());
    CHECK(build.clusterVertexCount.empty());
    CHECK(build.vertexIndices.empty());
    CHECK(build.triangles.empty());
    CHECK(build.stats.clusterCount == 0u);
    CHECK(build.stats.triangleCount == 0u);
    CHECK(build.stats.vertexCount == 0u);
    CHECK(build.stats.maxClusterTriangles == 0u);
    CHECK(build.stats.maxClusterVertices == 0u);
    CHECK(build.stats.degenerateClusterCount == 0u);

    // 有顶点但没三角形：同样是合法的空网格
    const std::vector<float> orphanPositions = { 0.0f, 0.0f, 0.0f };
    NaniteClusterBuild orphan;
    CHECK(BuildNaniteClusters(orphanPositions, indices, orphan));
    CHECK(orphan.stats.clusterCount == 0u);
}

// ============================================================
// 6. 边界：不足一个整簇的小网格
// ============================================================
TEST_CASE("NaniteBuilder: 不足一个整簇的小网格（1 / 18 三角形）都恰好 1 簇") {
    // ① 单个三角形
    {
        const std::vector<float> positions = { 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f };
        const std::vector<u32>   indices   = { 0u, 1u, 2u };
        NaniteClusterBuild build;
        REQUIRE(BuildNaniteClusters(positions, indices, build));
        MESSAGE(StatsLine("1 个三角形:", build).c_str());
        CHECK(build.stats.clusterCount == 1u);
        CHECK(build.stats.triangleCount == 1u);
        CHECK(build.stats.maxClusterTriangles == 1u);
        CHECK(build.stats.maxClusterVertices == 3u);
        CHECK(build.stats.degenerateClusterCount == 0u);
        CHECK(AllClustersRespectCaps(build));
    }
    // ② 3×3 四边形 = 18 个三角形（< 64，且 16 顶点 < 128）⇒ 一簇装得下
    {
        const GridMesh mesh = MakeGrid(3);
        REQUIRE(mesh.indices.size() == 54u);
        NaniteClusterBuild build;
        REQUIRE(BuildNaniteClusters(mesh.positions, mesh.indices, build));
        MESSAGE(StatsLine("3x3 网格（18 tri / 16 vert）:", build).c_str());
        CHECK(build.stats.clusterCount == 1u);
        CHECK(build.stats.triangleCount == 18u);
        CHECK(build.stats.maxClusterTriangles == 18u);
        CHECK(build.stats.maxClusterVertices <= kNaniteMaxClusterVertices);
        CHECK(build.stats.degenerateClusterCount == 0u);
        CHECK(AllClustersRespectCaps(build));
    }
}

// ============================================================
// 7. 边界：恰好一个整簇（8×4 四边形 = 64 三角形）
// ============================================================
TEST_CASE("NaniteBuilder: 恰好一个整簇（8×4 = 64 三角形）") {
    const GridMesh mesh = MakeGridRect(8, 4);   // 8×4 个四边形 = 64 个三角形，45 个顶点
    REQUIRE(mesh.indices.size() == 192u);   // 64 个三角形

    NaniteClusterBuild build;
    REQUIRE(BuildNaniteClusters(mesh.positions, mesh.indices, build));

    MESSAGE(StatsLine("8x4 网格（64 tri / 45 vert）:", build).c_str());

    CHECK(build.stats.clusterCount == 1u);                                        // 恰好一簇
    CHECK(build.stats.triangleCount == 64u);
    CHECK(build.stats.maxClusterTriangles == kNaniteMaxClusterTriangles);         // 打满 64
    CHECK(build.stats.maxClusterVertices <= kNaniteMaxClusterVertices);           // 81 ≤ 128
    CHECK(build.clusters[0].triangleOffset == 0u);
    CHECK(build.clusters[0].vertexOffset == 0u);
    CHECK(build.stats.degenerateClusterCount == 0u);
    CHECK(AllClustersRespectCaps(build));
}

// ============================================================
// 8. 非法输入 ⇒ false 且不改写出参
// ============================================================
TEST_CASE("NaniteBuilder: 非法输入返回 false 且不改写出参") {
    const GridMesh mesh = MakeGrid(2);   // 8 个三角形

    auto makePoisoned = []() {
        NaniteClusterBuild build;
        build.stats.clusterCount = 42u;      // 哨兵：若被改写就说明失败路径动了出参
        build.clusters.resize(3u);
        build.clusterVertexCount.push_back(7u);
        return build;
    };

    // ① 索引个数不是 3 的倍数
    {
        std::vector<u32> badIndices(mesh.indices.begin(), mesh.indices.begin() + 4);
        NaniteClusterBuild build = makePoisoned();
        CHECK_FALSE(BuildNaniteClusters(mesh.positions, badIndices, build));
        CHECK(build.stats.clusterCount == 42u);
        CHECK(build.clusters.size() == 3u);
        CHECK(build.clusterVertexCount.size() == 1u);
    }
    // ② 位置个数不是 3 的倍数
    {
        std::vector<float> badPositions(mesh.positions.begin(), mesh.positions.begin() + 4);
        NaniteClusterBuild build = makePoisoned();
        CHECK_FALSE(BuildNaniteClusters(badPositions, mesh.indices, build));
        CHECK(build.stats.clusterCount == 42u);
    }
    // ③ 存在越界索引（顶点数只有 3，索引却指向 3）
    {
        const std::vector<float> positions = { 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f };
        const std::vector<u32>   indices   = { 0u, 1u, 3u };
        NaniteClusterBuild build = makePoisoned();
        CHECK_FALSE(BuildNaniteClusters(positions, indices, build));
        CHECK(build.stats.clusterCount == 42u);
    }
    // ④ 有三角形但没有任何顶点
    {
        const std::vector<float> positions;
        const std::vector<u32>   indices = { 0u, 1u, 2u };
        NaniteClusterBuild build = makePoisoned();
        CHECK_FALSE(BuildNaniteClusters(positions, indices, build));
        CHECK(build.stats.clusterCount == 42u);
    }
}

// ============================================================
// 9. 包围球 / 锥字段的合法性（含"无锥哨兵"映射）
// ============================================================
TEST_CASE("NaniteBuilder: 包围球包含簇内顶点、锥字段合法、无锥哨兵映射") {
    // ① 平面网格：法线一致 ⇒ 每个簇都有可用的单位锥轴（不会退化成哨兵）
    {
        const GridMesh mesh = MakeGrid(16);
        NaniteClusterBuild build;
        REQUIRE(BuildNaniteClusters(mesh.positions, mesh.indices, build));
        REQUIRE(build.stats.clusterCount > 0u);
        CHECK(build.stats.noConeClusterCount == 0u);   // 平面网格的锥一定可用

        for (usize c = 0; c < build.clusters.size(); ++c) {
            const NaniteClusterRecord& record = build.clusters[c];
            const float radius = record.boundsCenterRadius[3];
            CHECK(radius >= 0.0f);
            CHECK(IsValidConeAxisAngle(record.cone));                 // 单位轴 + cos ∈ [-1,1]
            CHECK(record.cone.cosHalfAngle != kNaniteConeNoCullCos);  // 有真锥
            // 每个簇顶点都必须落在自己的包围球内（球心/半径来自 meshopt）
            for (u32 v = 0; v < build.clusterVertexCount[c]; ++v) {
                const u32 meshVertex = build.vertexIndices[record.vertexOffset + v];
                const float dx = mesh.positions[(usize)meshVertex * 3u + 0u] - record.boundsCenterRadius[0];
                const float dy = mesh.positions[(usize)meshVertex * 3u + 1u] - record.boundsCenterRadius[1];
                const float dz = mesh.positions[(usize)meshVertex * 3u + 2u] - record.boundsCenterRadius[2];
                const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
                CHECK(distance <= radius + 1.0e-3f);
            }
        }
    }
    // ② 立方体：六个面的法线铺满 ±x/±y/±z ⇒ 法线锥没有可用的单位轴 ⇒ 全簇走"无锥哨兵"
    {
        const GridMesh cube = MakeCube();
        REQUIRE(cube.indices.size() == 36u);   // 12 个三角形
        NaniteClusterBuild build;
        REQUIRE(BuildNaniteClusters(cube.positions, cube.indices, build));
        REQUIRE(build.stats.clusterCount > 0u);
        MESSAGE(StatsLine("立方体（12 tri / 8 vert）:", build).c_str());

        CHECK(build.stats.noConeClusterCount == build.stats.clusterCount);
        for (usize c = 0; c < build.clusters.size(); ++c) {
            const NaniteConeAxisAngle& cone = build.clusters[c].cone;
            CHECK(cone.cosHalfAngle == kNaniteConeNoCullCos);   // 无锥哨兵 = -1
            CHECK(cone.axis[0] == 0.0f);                        // 轴不伪造
            CHECK(cone.axis[1] == 0.0f);
            CHECK(cone.axis[2] == 0.0f);
            CHECK(IsValidConeAxisAngle(cone));                  // 哨兵本身是合法编码
        }
    }
}

// ============================================================
// 10. 与任务 7 的 `.nanite` 布局兼容（计数 → 段表 → 校验）
// ============================================================
TEST_CASE("NaniteBuilder: 产物与任务 7 的 .nanite 布局兼容") {
    const GridMesh mesh = MakeGrid(6);
    NaniteClusterBuild build;
    REQUIRE(BuildNaniteClusters(mesh.positions, mesh.indices, build));
    REQUIRE(build.stats.clusterCount > 0u);

    // ── 偏移/计数自身的一致性（不越界、且按簇连续排布）──
    u32 expectedTriangleOffset = 0u;
    u32 expectedVertexOffset   = 0u;
    for (usize c = 0; c < build.clusters.size(); ++c) {
        const NaniteClusterRecord& record = build.clusters[c];
        CHECK(record.triangleOffset == expectedTriangleOffset);
        CHECK(record.vertexOffset == expectedVertexOffset);
        expectedTriangleOffset += record.triangleCount;
        expectedVertexOffset   += build.clusterVertexCount[c];
        // 任务 9/10/12 的字段本任务必须为 0（不伪造语义）
        CHECK(record.materialID == 0u);
        CHECK(record.maxParentLODError == 0.0f);
        CHECK(record.childClusterOffset == 0u);
        CHECK(record.childCount == 0u);
        CHECK(record._pad == 0u);
    }
    CHECK(expectedTriangleOffset == (u32)build.triangles.size());
    CHECK(expectedVertexOffset == (u32)build.vertexIndices.size());

    // ── 打包三角形：局部下标 ≤127（u16 打包前提）且 hi 的高 16 位为 0 ──
    for (const NanitePackedTriangle& packed : build.triangles) {
        CHECK(IsValidClusterLocalVertexIndex(NaniteTriangleIndex0(packed)));
        CHECK(IsValidClusterLocalVertexIndex(NaniteTriangleIndex1(packed)));
        CHECK(IsValidClusterLocalVertexIndex(NaniteTriangleIndex2(packed)));
        CHECK((packed.hi >> 16) == 0u);
    }

    // ── 按任务 7 的段表把产物铺进一段内存，再交给 ValidateNaniteFile ──
    NaniteFileHeader header{};
    header.clusterCount  = (u32)build.clusters.size();
    header.vertexCount   = (u32)build.vertexIndices.size();
    header.indexCount    = (u32)build.triangles.size() * kNaniteIndicesPerTriangle;
    header.materialCount = 0u;
    header.lodLevelCount = 0u;
    // bbox 填真实网格范围（量化本身是任务 10，这里只为让头部自洽）
    header.bboxMin[0] = 0.0f; header.bboxMin[1] = 0.0f; header.bboxMin[2] = 0.0f;
    header.bboxMax[0] = 6.0f; header.bboxMax[1] = 6.0f; header.bboxMax[2] = 0.0f;

    NaniteFileLayout layout{};
    REQUIRE(TryBuildNaniteFileLayout(header, layout) == NaniteFileError::None);

    std::vector<u8> file((usize)layout.totalBytes, 0u);
    std::memcpy(file.data(), &header, sizeof(header));
    std::memcpy(file.data() + layout.clusterOffset, build.clusters.data(),
                build.clusters.size() * sizeof(NaniteClusterRecord));
    // 顶点段：任务 8 **不做量化**，这里放全 0 的 `NaniteVertex` 占位（只在验证段表容得下）
    for (u32 v = 0; v < header.vertexCount; ++v) {
        const NaniteVertex placeholder{};
        std::memcpy(file.data() + layout.vertexOffset + (usize)v * kNaniteVertexRecordBytes,
                    &placeholder, sizeof(placeholder));
    }
    std::memcpy(file.data() + layout.indexOffset, build.triangles.data(),
                build.triangles.size() * sizeof(NanitePackedTriangle));

    NaniteFileLayout checked{};
    CHECK(ValidateNaniteFile(file.data(), file.size(), &checked) == NaniteFileError::None);
    CHECK(checked.clusterBytes == layout.clusterBytes);
    CHECK(checked.vertexBytes == layout.vertexBytes);
    CHECK(checked.triangleCount == (u32)build.triangles.size());
    // 文件里的每个段都必须装得下任务 8 的产物
    CHECK((usize)build.clusters.size() * kNaniteClusterRecordBytes <= layout.clusterBytes);
    CHECK((usize)build.vertexIndices.size() * kNaniteVertexRecordBytes <= layout.vertexBytes);
    CHECK((usize)build.triangles.size() * kNaniteIndexBytesPerTriangle <= layout.indexBytes);
}

// ============================================================
// §14.8 任务 9 的测试网格与检查辅助
// ============================================================
namespace {

/// 平铺的相同子网格：`tilesX × tilesY` 个**互不相连**的副本，每片都是同一份
/// `quadsX × quadsY` 规则网格，按 `spacing` 平移铺开（每片自带一份顶点 ⇒ 不是共享顶点的大网格）。
/// 这样"内容相同的簇"会在不同世界位置重复出现 —— DAG 去重的天然用武之地。
struct TiledMesh {
    GridMesh mesh;    ///< 合并后的网格
    u32      tiles = 0;
};

TiledMesh MakeTiledPatches(u32 tilesX, u32 tilesY, u32 quadsX, u32 quadsY, float spacing) {
    TiledMesh tiled;
    const GridMesh patch = MakeGridRect(quadsX, quadsY);
    tiled.tiles = tilesX * tilesY;
    tiled.mesh.positions.reserve((usize)tiled.tiles * patch.positions.size());
    tiled.mesh.indices.reserve((usize)tiled.tiles * patch.indices.size());
    for (u32 ty = 0; ty < tilesY; ++ty) {
        for (u32 tx = 0; tx < tilesX; ++tx) {
            const float offsetX = (float)tx * spacing;
            const float offsetY = (float)ty * spacing;
            const u32   base    = (u32)(tiled.mesh.positions.size() / 3u);
            for (usize v = 0; v + 2u < patch.positions.size(); v += 3u) {
                tiled.mesh.positions.push_back(patch.positions[v + 0u] + offsetX);
                tiled.mesh.positions.push_back(patch.positions[v + 1u] + offsetY);
                tiled.mesh.positions.push_back(patch.positions[v + 2u]);
            }
            for (const u32 index : patch.indices) {
                tiled.mesh.indices.push_back(base + index);
            }
        }
    }
    return tiled;
}

/// 任务 9 的验收读数行（每级簇数/每级三角形/levelCount/unique/去重率/上限/退化簇）
std::string DAGStatsLine(const char* label, const NaniteClusterDAG& dag) {
    const NaniteClusterDAGStats& s = dag.stats;
    std::string line = std::string(label) +
        " levelCount=" + std::to_string(s.levelCount) +
        " 简化级数=" + std::to_string(s.simplifiedLevelCount) +
        " 总簇数=" + std::to_string(s.totalClusterCount) +
        " unique=" + std::to_string(s.uniqueClusterCount) +
        " 去重率=" + std::to_string(s.dedupRate) +
        " (" + std::to_string(s.dedupRate * 100.0f) + "%)" +
        " 每级簇数=[";
    for (usize level = 0; level < dag.levelClusterCount.size(); ++level) {
        line += std::to_string(dag.levelClusterCount[level]);
        line += (level + 1u < dag.levelClusterCount.size()) ? "," : "";
    }
    line += "] 每级三角形=[";
    for (usize level = 0; level < dag.levelTriangleCount.size(); ++level) {
        line += std::to_string(dag.levelTriangleCount[level]);
        line += (level + 1u < dag.levelTriangleCount.size()) ? "," : "";
    }
    line += "] 每级unique=[";
    for (usize level = 0; level < dag.levelUniqueCount.size(); ++level) {
        line += std::to_string(dag.levelUniqueCount[level]);
        line += (level + 1u < dag.levelUniqueCount.size()) ? "," : "";
    }
    line += "] 最大每簇三角形=" + std::to_string(s.maxClusterTriangles) +
            " 最大每簇顶点=" + std::to_string(s.maxClusterVertices) +
            " 退化簇=" + std::to_string(s.degenerateClusterCount) +
            " 叶子簇=" + std::to_string(s.leafClusterCount) +
            " 根簇=" + std::to_string(s.rootClusterCount) +
            " maxLODError=" + std::to_string(s.maxLODError);
    return line;
}

/// 任务 9 的"每簇硬上限 + 退化 + 偏移自洽"检查
bool AllDAGClustersRespectCaps(const NaniteClusterDAG& dag) {
    if (dag.clusters.size() != dag.clusterVertexCount.size()) return false;
    if (dag.clusters.size() != dag.clusterVertexIndexOffset.size()) return false;
    if (dag.clusters.size() != dag.clusterLevel.size()) return false;
    if (dag.clusters.size() != dag.clusterUnique.size()) return false;
    if (dag.uniqueVertexOffset.size() != dag.uniqueVertexCount.size()) return false;
    if (dag.uniqueTriangleOffset.size() != dag.uniqueTriangleCount.size()) return false;
    if (dag.uniqueVertexOffset.size() != dag.uniqueVertexCount.size()) return false;
    if (dag.stats.totalClusterCount != (u32)dag.clusters.size()) return false;
    if (dag.stats.uniqueClusterCount != (u32)dag.uniqueVertexCount.size()) return false;
    if (dag.levelClusterOffset.size() != dag.levelClusterCount.size() + 1u) return false;

    u32 expectedVertexIndexOffset = 0u;
    for (usize c = 0; c < dag.clusters.size(); ++c) {
        const u32 localTriangles = dag.clusters[c].triangleCount;
        const u32 localVertices  = dag.clusterVertexCount[c];
        if (localTriangles == 0u || localTriangles > kNaniteMaxClusterTriangles) return false;
        if (localVertices  == 0u || localVertices  > kNaniteMaxClusterVertices)  return false;
        // 出现记录的偏移必须落在**共享内容**表内
        if (dag.clusters[c].vertexOffset + localVertices > (u32)dag.uniqueVertexWords.size()) return false;
        if (dag.clusters[c].triangleOffset + localTriangles >
            (u32)dag.uniqueTriangles.size()) return false;
        // 放置数据（局部顶点 → 网格顶点）按出现连续排布
        if (dag.clusterVertexIndexOffset[c] != expectedVertexIndexOffset) return false;
        expectedVertexIndexOffset += localVertices;
        // 出现 → 唯一内容的映射必须与偏移/计数一致（共享口径的守卫）
        const u32 unique = dag.clusterUnique[c];
        if (unique >= dag.uniqueVertexCount.size()) return false;
        if (dag.clusters[c].vertexOffset   != dag.uniqueVertexOffset[unique])   return false;
        if (dag.clusters[c].triangleOffset != dag.uniqueTriangleOffset[unique]) return false;
        if (localVertices  != dag.uniqueVertexCount[unique])    return false;
        if (localTriangles != dag.uniqueTriangleCount[unique])  return false;
    }

    // 共享内容表自身连续排布
    u32 expectedVertexOffset = 0u;
    u32 expectedTriangleOffset = 0u;
    for (usize u = 0; u < dag.uniqueVertexCount.size(); ++u) {
        if (dag.uniqueVertexOffset[u] != expectedVertexOffset) return false;
        if (dag.uniqueTriangleOffset[u] != expectedTriangleOffset) return false;
        expectedVertexOffset   += dag.uniqueVertexCount[u];
        expectedTriangleOffset += dag.uniqueTriangleCount[u];
    }
    return expectedVertexOffset == (u32)dag.uniqueVertexWords.size() &&
           expectedTriangleOffset == (u32)dag.uniqueTriangles.size() &&
           expectedVertexIndexOffset == (u32)dag.clusterVertexIndices.size();
}

/// 两次 DAG 构建是否逐位一致（记录/表/平行数组/统计都按字节比）
bool SameDAG(const NaniteClusterDAG& a, const NaniteClusterDAG& b) {
    if (a.uniqueVertexWords != b.uniqueVertexWords) return false;
    if (a.uniqueVertexOffset != b.uniqueVertexOffset) return false;
    if (a.uniqueVertexCount != b.uniqueVertexCount) return false;
    if (a.uniqueTriangleOffset != b.uniqueTriangleOffset) return false;
    if (a.uniqueTriangleCount != b.uniqueTriangleCount) return false;
    if (a.clusterVertexCount != b.clusterVertexCount) return false;
    if (a.clusterVertexIndexOffset != b.clusterVertexIndexOffset) return false;
    if (a.clusterVertexIndices != b.clusterVertexIndices) return false;
    if (a.clusterLevel != b.clusterLevel) return false;
    if (a.clusterUnique != b.clusterUnique) return false;
    if (a.levelClusterOffset != b.levelClusterOffset) return false;
    if (a.levelClusterCount != b.levelClusterCount) return false;
    if (a.levelTriangleCount != b.levelTriangleCount) return false;
    if (a.levelUniqueCount != b.levelUniqueCount) return false;
    if (a.childClusterIndices != b.childClusterIndices) return false;
    if (a.parentCluster != b.parentCluster) return false;
    if (a.clusters.size() != b.clusters.size()) return false;
    if (a.uniqueTriangles.size() != b.uniqueTriangles.size()) return false;
    if (!a.clusters.empty() &&
        std::memcmp(a.clusters.data(), b.clusters.data(),
                    a.clusters.size() * sizeof(NaniteClusterRecord)) != 0) return false;
    if (!a.uniqueTriangles.empty() &&
        std::memcmp(a.uniqueTriangles.data(), b.uniqueTriangles.data(),
                    a.uniqueTriangles.size() * sizeof(NanitePackedTriangle)) != 0) return false;
    return std::memcmp(&a.stats, &b.stats, sizeof(NaniteClusterDAGStats)) == 0;
}

} // namespace

// ============================================================
// 11. LOD 链：levelCount > 0 且逐级减半（单调性）
// ============================================================
TEST_CASE("NaniteDAG: LOD 链逐级减半（levelCount > 0、每级三角形不增且不超过上一级一半）") {
    const GridMesh mesh = MakeGrid(32);   // 2048 个三角形
    REQUIRE(mesh.indices.size() == 2048u * 3u);

    NaniteClusterDAG dag;
    REQUIRE(BuildNaniteClusterDAG(mesh.positions, mesh.indices, dag));
    MESSAGE(DAGStatsLine("(b) 一般网格 32x32（2048 tri）:", dag).c_str());

    // 验收原文：LOD 层级 > 0；这里同时钉住"至少 1 级以上"（简化级数 = levelCount - 1）
    CHECK(dag.stats.levelCount > 0u);
    CHECK(dag.stats.simplifiedLevelCount > 0u);
    CHECK(dag.stats.simplifiedLevelCount == dag.stats.levelCount - 1u);
    // 级数上限（含 LOD0）：meshopt 每次可能略微超额（结果 ≤ 目标），故实际级数 ≤ 6
    CHECK(dag.stats.levelCount >= 2u);
    CHECK(dag.stats.levelCount <= kNaniteMaxLODLevels);
    CHECK(dag.levelTriangleCount[0] == 2048u);

    for (usize level = 1; level < dag.levelTriangleCount.size(); ++level) {
        // 单调性：每级三角形数 ≤ 上一级；"逐级减半"更强：≤ 上一级 / 2（整除向下）
        CHECK(dag.levelTriangleCount[level] <= dag.levelTriangleCount[level - 1]);
        CHECK(dag.levelTriangleCount[level] <= dag.levelTriangleCount[level - 1] / 2u);
        // 终止阈值：最后一级仍不少于一个满簇，下一级目标会低于它
        CHECK(dag.levelTriangleCount[level] >= kNaniteMinLODTriangles);
        // 级视图与出现表的区间自洽
        CHECK(dag.levelClusterCount[level] > 0u);
        CHECK(dag.levelClusterOffset[level] + dag.levelClusterCount[level] ==
              dag.levelClusterOffset[level + 1u]);
    }
    // 簇数也随级递减（父簇数 ≤ 子簇数：父一定有人认）
    for (usize level = 1; level < dag.levelClusterCount.size(); ++level) {
        CHECK(dag.levelClusterCount[level] <= dag.levelClusterCount[level - 1]);
    }

    CHECK(dag.stats.maxClusterTriangles <= kNaniteMaxClusterTriangles);
    CHECK(dag.stats.maxClusterVertices  <= kNaniteMaxClusterVertices);
    CHECK(dag.stats.degenerateClusterCount == 0u);
    CHECK(AllDAGClustersRespectCaps(dag));

    // 出现表与每级视图对得上：Σ 每级簇数 = 总簇数
    u32 summed = 0u;
    for (const u32 count : dag.levelClusterCount) summed += count;
    CHECK(summed == dag.stats.totalClusterCount);
    CHECK(dag.levelClusterOffset.back() == dag.stats.totalClusterCount);
}

// ============================================================
// 12. DAG 去重率：平铺相同子网格（>10%） vs 一般网格（如实报告）
// ============================================================
TEST_CASE("NaniteDAG: DAG 去重率（平铺的相同子网格 vs 一般网格）") {
    // ── (a) 有重复结构的网格：4×4 个互不相连的同一份 8×4 网格副本（每片 64 tri）──
    {
        const TiledMesh tiled = MakeTiledPatches(4, 4, 8, 4, 20.0f);
        REQUIRE(tiled.tiles == 16u);
        REQUIRE(tiled.mesh.indices.size() == 16u * 64u * 3u);   // 1024 个三角形

        NaniteClusterDAG dag;
        REQUIRE(BuildNaniteClusterDAG(tiled.mesh.positions, tiled.mesh.indices, dag));
        MESSAGE(DAGStatsLine("(a) 平铺相同子网格 16×64 tri:", dag).c_str());

        // 验收：DAG 去重率 > 10%
        CHECK(dag.stats.dedupRate > 0.10f);
        CHECK(dag.stats.uniqueClusterCount < dag.stats.totalClusterCount);
        // 每级都应当受益于重复内容（每级引用的唯一内容远少于该级簇数）
        REQUIRE(dag.levelUniqueCount.size() == dag.levelClusterCount.size());
        for (usize level = 0; level < dag.levelUniqueCount.size(); ++level) {
            CHECK(dag.levelUniqueCount[level] <= dag.levelClusterCount[level]);
        }
        CHECK(dag.levelUniqueCount[0] < dag.levelClusterCount[0]);
        CHECK(dag.stats.maxClusterTriangles <= kNaniteMaxClusterTriangles);
        CHECK(dag.stats.maxClusterVertices  <= kNaniteMaxClusterVertices);
        CHECK(dag.stats.degenerateClusterCount == 0u);
        CHECK(AllDAGClustersRespectCaps(dag));
    }
    // ── (b) 一般网格：单一连通的规则网格，几何本身没有任何重复 ──
    {
        const GridMesh mesh = MakeGrid(32);   // 2048 tri
        NaniteClusterDAG dag;
        REQUIRE(BuildNaniteClusterDAG(mesh.positions, mesh.indices, dag));
        MESSAGE(DAGStatsLine("(b) 一般网格 32x32（2048 tri）:", dag).c_str());

        // 如实报告：不设 >10% 的下限，只钉住口径合法性（[0,1] 且与计数一致）
        CHECK(dag.stats.dedupRate >= 0.0f);
        CHECK(dag.stats.dedupRate <= 1.0f);
        const float expectedRate = 1.0f - (float)dag.stats.uniqueClusterCount /
                                            (float)dag.stats.totalClusterCount;
        CHECK(dag.stats.dedupRate == expectedRate);
        CHECK(AllDAGClustersRespectCaps(dag));
    }
    // ── (c) 混合网格：一般网格 + 重复子网格（证明"一般场景里出现重复结构 ⇒ 去重率被拉过 10%"）──
    {
        GridMesh mixed = MakeGrid(32);                   // 2048 tri 的连通地面
        const TiledMesh repeated = MakeTiledPatches(6, 6, 8, 4, 40.0f);   // 36×64 = 2304 tri 的重复柱
        const u32 base = (u32)(mixed.positions.size() / 3u);
        for (usize v = 0; v + 2u < repeated.mesh.positions.size(); v += 3u) {
            mixed.positions.push_back(repeated.mesh.positions[v + 0u] + 40.0f);
            mixed.positions.push_back(repeated.mesh.positions[v + 1u] + 40.0f);
            mixed.positions.push_back(repeated.mesh.positions[v + 2u] + 5.0f);
        }
        for (const u32 index : repeated.mesh.indices) {
            mixed.indices.push_back(base + index);
        }

        NaniteClusterDAG dag;
        REQUIRE(BuildNaniteClusterDAG(mixed.positions, mixed.indices, dag));
        MESSAGE(DAGStatsLine("(c) 混合网格（2048 地面 + 36×64 重复柱）:", dag).c_str());
        CHECK(dag.stats.dedupRate > 0.10f);
        CHECK(AllDAGClustersRespectCaps(dag));
    }
}

// ============================================================
// 13. DAG 链接自洽：不越界、每个子簇恰有一个父、无孤儿、无环
// ============================================================
TEST_CASE("NaniteDAG: 父子链接自洽（不越界 / 每子一父 / 无孤儿 / 无环）") {
    const GridMesh grid = MakeGrid(32);
    const TiledMesh tiled = MakeTiledPatches(3, 3, 8, 4, 20.0f);
    const GridMesh* const meshes[2] = { &grid, &tiled.mesh };
    const char* const     labels[2] = { "一般网格", "平铺网格" };

    for (u32 which = 0; which < 2u; ++which) {
        const GridMesh& mesh = *meshes[which];
        NaniteClusterDAG dag;
        REQUIRE(BuildNaniteClusterDAG(mesh.positions, mesh.indices, dag));
        REQUIRE(dag.stats.levelCount > 0u);
        MESSAGE(DAGStatsLine(labels[which], dag).c_str());

        const usize clusterCount = dag.clusters.size();
        const u32   levelCount   = dag.stats.levelCount;
        REQUIRE(dag.parentCluster.size() == clusterCount);

        // ① 子表不越界、连续排布、反向一致；每个子簇的级恰好比父簇细一级
        std::vector<u32> parentCount(clusterCount, 0u);
        u32 expectedChildOffset = 0u;
        for (usize c = 0; c < clusterCount; ++c) {
            const NaniteClusterRecord& record = dag.clusters[c];
            CHECK(record.childClusterOffset == expectedChildOffset);   // 扁平表按出现顺序连续写
            CHECK((usize)record.childClusterOffset + record.childCount <=
                  dag.childClusterIndices.size());
            for (u32 k = 0; k < record.childCount; ++k) {
                const u32 child = dag.childClusterIndices[record.childClusterOffset + k];
                CHECK(child < clusterCount);                              // 不越界
                CHECK(dag.clusterLevel[child] + 1u == dag.clusterLevel[c]);  // 子恰好细一级
                CHECK(dag.parentCluster[child] == (u32)c);                // 反向链接一致
                ++parentCount[child];
            }
            expectedChildOffset += record.childCount;
        }
        CHECK(expectedChildOffset == (u32)dag.childClusterIndices.size());

        // ② 每个非根簇恰有一个父（无孤儿），根簇没有父；叶子（LOD0）没有孩子
        u32 rootCount = 0u;
        u32 leafCount = 0u;
        for (usize c = 0; c < clusterCount; ++c) {
            if (dag.clusterLevel[c] + 1u < levelCount) {
                CHECK(dag.parentCluster[c] != kNaniteNoParentCluster);
                CHECK(parentCount[c] == 1u);            // 恰有一个父 ⇒ 不重复挂靠、无孤儿
            } else {
                CHECK(dag.parentCluster[c] == kNaniteNoParentCluster);
                CHECK(parentCount[c] == 0u);
                ++rootCount;
            }
            // "有孩子"的判据是"不在最细一级"（LOD0 的簇是叶子，必然没有孩子）
            if (dag.clusterLevel[c] > 0u) {
                CHECK(dag.clusters[c].childCount > 0u);   // 每个非叶簇都有人认（连通性）
            } else {
                CHECK(dag.clusters[c].childCount == 0u);  // 叶子
                ++leafCount;
            }
        }
        CHECK(rootCount == dag.stats.rootClusterCount);
        CHECK(leafCount == dag.stats.leafClusterCount);

        // ③ 无环：沿父链走，级严格递增，必然在"最高一级"终止
        for (usize c = 0; c < clusterCount; ++c) {
            u32 cursor = (u32)c;
            u32 steps  = 0u;
            while (dag.parentCluster[cursor] != kNaniteNoParentCluster) {
                const u32 parent = dag.parentCluster[cursor];
                CHECK(dag.clusterLevel[parent] == dag.clusterLevel[cursor] + 1u);
                cursor = parent;
                ++steps;
                REQUIRE(steps <= levelCount);   // 级数上限兜底：出现环必然在这里炸掉
            }
            CHECK(dag.clusterLevel[cursor] + 1u == levelCount);   // 根一定在最高一级
        }
    }
}

// ============================================================
// 14. 共享内容与 maxParentLODError
// ============================================================
TEST_CASE("NaniteDAG: 共享内容自洽、maxParentLODError 来源正确（根为 0）") {
    const TiledMesh tiled = MakeTiledPatches(4, 4, 8, 4, 20.0f);
    NaniteClusterDAG dag;
    REQUIRE(BuildNaniteClusterDAG(tiled.mesh.positions, tiled.mesh.indices, dag));
    REQUIRE(dag.stats.levelCount > 0u);

    // ① 共享内容表：每个唯一内容的顶点词都是位置词（低 30 位有效，w 位 = 1 的 R10G10B10A2）
    for (const u32 word : dag.uniqueVertexWords) {
        CHECK((word >> 30) == 1u);   // NanitePackPosition 的 w = 1（任务 7 的既定约定）
    }
    // ② 每个唯一内容都被至少一条出现记录引用（没有"死内容"）
    std::vector<u32> referenceCount(dag.uniqueVertexCount.size(), 0u);
    for (usize c = 0; c < dag.clusters.size(); ++c) {
        REQUIRE(dag.clusterUnique[c] < referenceCount.size());
        ++referenceCount[dag.clusterUnique[c]];
    }
    for (const u32 count : referenceCount) CHECK(count > 0u);

    // ③ maxParentLODError：非根级 > 0（meshopt 的绝对简化误差），根 = 0（没有父级）
    for (usize c = 0; c < dag.clusters.size(); ++c) {
        const float error = dag.clusters[c].maxParentLODError;
        CHECK(error >= 0.0f);
        if (dag.clusterLevel[c] + 1u < dag.stats.levelCount) {
            CHECK(error > 0.0f);
        } else {
            CHECK(error == 0.0f);
        }
    }
    // 同一级的误差阈值一致（口径：整网格一次简化 ⇒ 该级共用同一个保守上界）
    for (usize c = 1; c < dag.clusters.size(); ++c) {
        if (dag.clusterLevel[c] == dag.clusterLevel[c - 1u] &&
            dag.clusterLevel[c] + 1u < dag.stats.levelCount) {
            CHECK(dag.clusters[c].maxParentLODError == dag.clusters[c - 1u].maxParentLODError);
        }
    }
    // ④ 任务 9 的包围球口径：球心 = 簇 AABB 中心、半径 = 到最远顶点的距离（包含本簇全部顶点）。
    //    这条同时是"量化原点是 AABB 中心"的可验证落点 —— 平移副本因此才能算出逐位相同的位置词。
    for (usize c = 0; c < dag.clusters.size(); ++c) {
        const NaniteClusterRecord& record = dag.clusters[c];
        const u32 localVertexCount = dag.clusterVertexCount[c];
        REQUIRE(localVertexCount > 0u);

        float aabbMin[3] = { 0.0f, 0.0f, 0.0f };
        float aabbMax[3] = { 0.0f, 0.0f, 0.0f };
        for (u32 v = 0; v < localVertexCount; ++v) {
            const u32 meshVertex = dag.clusterVertexIndices[dag.clusterVertexIndexOffset[c] + v];
            for (u32 axis = 0; axis < 3u; ++axis) {
                const float value = tiled.mesh.positions[(usize)meshVertex * 3u + axis];
                if (v == 0u || value < aabbMin[axis]) aabbMin[axis] = value;
                if (v == 0u || value > aabbMax[axis]) aabbMax[axis] = value;
            }
        }
        // 注意：`clusters[c].vertexOffset` 是**共享顶点表**的偏移；本簇的"局部顶点 → 网格顶点"
        // 映射在 `clusterVertexIndices` 里按出现顺序连续存放，起点是 `clusterVertexIndexOffset[c]`。
        CHECK(record.boundsCenterRadius[0] == (aabbMin[0] + aabbMax[0]) * 0.5f);
        CHECK(record.boundsCenterRadius[1] == (aabbMin[1] + aabbMax[1]) * 0.5f);
        CHECK(record.boundsCenterRadius[2] == (aabbMin[2] + aabbMax[2]) * 0.5f);
        for (u32 v = 0; v < localVertexCount; ++v) {
            const u32 meshVertex = dag.clusterVertexIndices[dag.clusterVertexIndexOffset[c] + v];
            const float dx = tiled.mesh.positions[(usize)meshVertex * 3u + 0u] - record.boundsCenterRadius[0];
            const float dy = tiled.mesh.positions[(usize)meshVertex * 3u + 1u] - record.boundsCenterRadius[1];
            const float dz = tiled.mesh.positions[(usize)meshVertex * 3u + 2u] - record.boundsCenterRadius[2];
            CHECK(std::sqrt(dx * dx + dy * dy + dz * dz) <= record.boundsCenterRadius[3] + 1.0e-4f);
        }
        CHECK(IsValidConeAxisAngle(record.cone));
    }

    // ⑤ 局部位移上界：局部顶点词解码后（相对簇心）必须落在"网格最大范围的一半"量级内；
    //    这里只钉住"量化是可逆的位域"（w 位 = 1），具体几何误差属于任务 10 的量化往返判据。
    CHECK(dag.stats.maxLODError > 0.0f);
}

// ============================================================
// 15. 可复现性与边界（空网格 / 非法输入 / 凑不出一个满簇）
// ============================================================
TEST_CASE("NaniteDAG: 同一输入两次构建逐位可复现") {
    const TiledMesh tiled = MakeTiledPatches(3, 3, 8, 4, 20.0f);

    NaniteClusterDAG first;
    NaniteClusterDAG second;
    REQUIRE(BuildNaniteClusterDAG(tiled.mesh.positions, tiled.mesh.indices, first));
    REQUIRE(BuildNaniteClusterDAG(tiled.mesh.positions, tiled.mesh.indices, second));
    MESSAGE(DAGStatsLine("可复现性（3x3 平铺）:", first).c_str());

    CHECK(first.stats.levelCount == second.stats.levelCount);
    CHECK(first.stats.uniqueClusterCount == second.stats.uniqueClusterCount);
    CHECK(first.stats.dedupRate == second.stats.dedupRate);
    CHECK(SameDAG(first, second));
}

TEST_CASE("NaniteDAG: 空网格 / 非法输入 / 凑不出满簇的边界") {
    // ① 空网格：成功、级数 0、产物为空（出参先污染，成功路径必须整体写满）
    {
        const std::vector<float> positions;
        const std::vector<u32>   indices;
        NaniteClusterDAG dag;
        dag.stats.levelCount = 9u;
        dag.clusters.resize(3u);
        dag.clusterLevel.push_back(7u);
        CHECK(BuildNaniteClusterDAG(positions, indices, dag));
        CHECK(dag.Empty());
        CHECK(dag.clusters.empty());
        CHECK(dag.clusterVertexCount.empty());
        CHECK(dag.clusterVertexIndices.empty());
        CHECK(dag.clusterLevel.empty());
        CHECK(dag.clusterUnique.empty());
        CHECK(dag.uniqueVertexWords.empty());
        CHECK(dag.uniqueTriangles.empty());
        CHECK(dag.levelClusterCount.empty());
        CHECK(dag.levelClusterOffset.empty());
        CHECK(dag.levelTriangleCount.empty());
        CHECK(dag.levelUniqueCount.empty());
        CHECK(dag.childClusterIndices.empty());
        CHECK(dag.parentCluster.empty());
        CHECK(dag.stats.levelCount == 0u);
        CHECK(dag.stats.simplifiedLevelCount == 0u);
        CHECK(dag.stats.totalClusterCount == 0u);
        CHECK(dag.stats.uniqueClusterCount == 0u);
        CHECK(dag.stats.dedupRate == 0.0f);
    }
    // ② 非法输入：返回 false 且不改写出参（与任务 8 同口径）
    {
        const GridMesh mesh = MakeGrid(2);
        auto makePoisoned = []() {
            NaniteClusterDAG dag;
            dag.stats.levelCount = 42u;
            dag.clusters.resize(3u);
            dag.parentCluster.push_back(5u);
            return dag;
        };
        // 索引个数不是 3 的倍数
        {
            std::vector<u32> badIndices(mesh.indices.begin(), mesh.indices.begin() + 4);
            NaniteClusterDAG dag = makePoisoned();
            CHECK_FALSE(BuildNaniteClusterDAG(mesh.positions, badIndices, dag));
            CHECK(dag.stats.levelCount == 42u);
            CHECK(dag.clusters.size() == 3u);
            CHECK(dag.parentCluster.size() == 1u);
        }
        // 位置个数不是 3 的倍数
        {
            std::vector<float> badPositions(mesh.positions.begin(), mesh.positions.begin() + 4);
            NaniteClusterDAG dag = makePoisoned();
            CHECK_FALSE(BuildNaniteClusterDAG(badPositions, mesh.indices, dag));
            CHECK(dag.stats.levelCount == 42u);
        }
        // 越界索引
        {
            const std::vector<float> positions = { 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f };
            const std::vector<u32>   indices   = { 0u, 1u, 3u };
            NaniteClusterDAG dag = makePoisoned();
            CHECK_FALSE(BuildNaniteClusterDAG(positions, indices, dag));
            CHECK(dag.stats.levelCount == 42u);
        }
        // 有三角形却没有顶点
        {
            const std::vector<float> positions;
            const std::vector<u32>   indices = { 0u, 1u, 2u };
            NaniteClusterDAG dag = makePoisoned();
            CHECK_FALSE(BuildNaniteClusterDAG(positions, indices, dag));
            CHECK(dag.stats.levelCount == 42u);
        }
    }
    // ③ 单个三角形：合法，但凑不出"下一级 ≥ 一个满簇"⇒ 只有 LOD0（levelCount = 1）
    {
        const std::vector<float> positions = { 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f };
        const std::vector<u32>   indices   = { 0u, 1u, 2u };
        NaniteClusterDAG dag;
        REQUIRE(BuildNaniteClusterDAG(positions, indices, dag));
        MESSAGE(DAGStatsLine("单三角形（凑不出满簇）:", dag).c_str());
        CHECK(dag.stats.levelCount == 1u);            // 只有 LOD0
        CHECK(dag.stats.simplifiedLevelCount == 0u);
        CHECK(dag.stats.totalClusterCount == 1u);
        CHECK(dag.stats.uniqueClusterCount == 1u);
        CHECK(dag.stats.leafClusterCount == 1u);
        CHECK(dag.stats.rootClusterCount == 1u);
        CHECK(dag.clusters[0].childCount == 0u);
        CHECK(dag.parentCluster[0] == kNaniteNoParentCluster);
        CHECK(AllDAGClustersRespectCaps(dag));
    }
    // ④ 恰好 72 tri 的 6×6 网格：下一级目标 36 < 64 ⇒ 仍然只有 LOD0（终止条件①的显式覆盖）
    {
        const GridMesh mesh = MakeGrid(6);
        NaniteClusterDAG dag;
        REQUIRE(BuildNaniteClusterDAG(mesh.positions, mesh.indices, dag));
        CHECK(dag.levelTriangleCount[0] == 72u);
        CHECK(dag.stats.levelCount == 1u);
    }
    // ⑤ 恰好 128 tri（16×4 网格）：下一级目标 64 ≥ 一个满簇 ⇒ 会生成 LOD1（终止阈值边界）
    {
        const GridMesh mesh = MakeGridRect(16, 4);   // 16×4 四边形 = 128 三角形
        REQUIRE(mesh.indices.size() == 128u * 3u);
        NaniteClusterDAG dag;
        REQUIRE(BuildNaniteClusterDAG(mesh.positions, mesh.indices, dag));
        MESSAGE(DAGStatsLine("16x4 网格（128 tri，恰好能减半）:", dag).c_str());
        CHECK(dag.stats.levelCount >= 2u);
        CHECK(dag.levelTriangleCount[1] <= 64u);
    }
}

// ============================================================
// §14.8 任务 10：量化与打包的测试辅助
//
// 打包需要**顶点属性**（法线/UV），而任务 8/9 的网格只有位置 —— 所以这里按确定性公式现场
// 生成属性，不读任何资产：
//   · 法线：把网格归一化坐标 (u,v) 参数化成**整个单位球**（theta 绕 Y 一圈、phi 从北极到南极）
//     —— 这样八面体编码的**两个半球与折叠边界**都被覆盖到，而不是只有 +Z；
//   · UV：取网格归一化平面坐标（落在 [0,1]，即 unorm16 的域内）。
// ============================================================
namespace {

constexpr float kTask10Pi = 3.14159265358979323846f;

/// 每顶点的属性（法线 xyz、UV uv）
struct MeshAttributes {
    std::vector<float> normals;  ///< 每顶点 3 个 float（单位向量）
    std::vector<float> uvs;      ///< 每顶点 2 个 float（[0,1] 内）
};

/// 按网格自身的 AABB 归一化位置，再参数化成"球面法线 + 平面 UV"
MeshAttributes MakeSphereAttributes(const GridMesh& mesh) {
    MeshAttributes attributes;
    const u32 vertexCount = (u32)(mesh.positions.size() / 3u);
    if (vertexCount == 0u) return attributes;
    attributes.normals.resize((usize)vertexCount * 3u);
    attributes.uvs.resize((usize)vertexCount * 2u);

    float minValue[3] = { mesh.positions[0], mesh.positions[1], mesh.positions[2] };
    float maxValue[3] = { minValue[0], minValue[1], minValue[2] };
    for (u32 v = 1u; v < vertexCount; ++v) {
        for (u32 axis = 0; axis < 3u; ++axis) {
            const float value = mesh.positions[(usize)v * 3u + axis];
            if (value < minValue[axis]) minValue[axis] = value;
            if (value > maxValue[axis]) maxValue[axis] = value;
        }
    }
    const float extentX = maxValue[0] - minValue[0];
    const float extentY = maxValue[1] - minValue[1];

    for (u32 v = 0u; v < vertexCount; ++v) {
        const float* position = mesh.positions.data() + (usize)v * 3u;
        const float u = (extentX > 0.0f) ? (position[0] - minValue[0]) / extentX : 0.0f;
        const float w = (extentY > 0.0f) ? (position[1] - minValue[1]) / extentY : 0.0f;
        attributes.uvs[(usize)v * 2u + 0u] = u;
        attributes.uvs[(usize)v * 2u + 1u] = w;
        // 球面法线：u → 绕 Y 一圈（theta）、w → 北极到南极（phi）
        const float theta = u * 2.0f * kTask10Pi;
        const float phi   = w * kTask10Pi;
        attributes.normals[(usize)v * 3u + 0u] = std::sin(phi) * std::cos(theta);
        attributes.normals[(usize)v * 3u + 1u] = std::cos(phi);
        attributes.normals[(usize)v * 3u + 2u] = std::sin(phi) * std::sin(theta);
    }
    return attributes;
}

/// 平铺网格 + 属性：**每片 UV 平移**，用来构造"位置/拓扑相同、UV 不同"的共享冲突
struct TiledMeshWithAttributes {
    GridMesh       mesh;        ///< 与 `MakeTiledPatches` 同构的合并网格
    MeshAttributes attributes;  ///< 每顶点法线/UV（UV 随片号平移）
};

TiledMeshWithAttributes MakeTiledPatchesWithAttributes(u32 tilesX, u32 tilesY,
                                                       u32 quadsX, u32 quadsY, float spacing) {
    const TiledMesh tiled = MakeTiledPatches(tilesX, tilesY, quadsX, quadsY, spacing);
    TiledMeshWithAttributes result;
    result.mesh = tiled.mesh;

    const u32 side     = quadsX + 1u;
    const u32 rows     = quadsY + 1u;
    const u32 perTile  = side * rows;
    const u32 vertexCount = (u32)(result.mesh.positions.size() / 3u);
    result.attributes.normals.resize((usize)vertexCount * 3u);
    result.attributes.uvs.resize((usize)vertexCount * 2u);

    for (u32 v = 0u; v < vertexCount; ++v) {
        const u32 tile   = (perTile > 0u) ? (v / perTile) : 0u;
        const u32 local  = (perTile > 0u) ? (v % perTile) : 0u;
        const u32 localY = (side > 0u) ? (local / side) : 0u;
        const u32 localX = (side > 0u) ? (local % side) : 0u;
        const float localU = (quadsX > 0u) ? (float)localX / (float)quadsX : 0.0f;
        const float localV = (quadsY > 0u) ? (float)localY / (float)quadsY : 0.0f;
        // UV：每片平移 0.0 / 0.1 / 0.2（仍落在 [0,1] 内）⇒ 同一共享内容的不同出现 UV 不同
        result.attributes.uvs[(usize)v * 2u + 0u] = localU * 0.7f + 0.1f * (float)(tile % 3u);
        result.attributes.uvs[(usize)v * 2u + 1u] = localV * 0.7f + 0.1f * (float)((tile / 3u) % 3u);
        // 法线：同样用球面参数化（与片号无关，保证"UV 冲突"这条读数不被法线冲突污染）
        const float theta = localU * 2.0f * kTask10Pi;
        const float phi   = localV * kTask10Pi;
        result.attributes.normals[(usize)v * 3u + 0u] = std::sin(phi) * std::cos(theta);
        result.attributes.normals[(usize)v * 3u + 1u] = std::cos(phi);
        result.attributes.normals[(usize)v * 3u + 2u] = std::sin(phi) * std::sin(theta);
    }
    return result;
}

/// 平铺网格 + **逐片完全相同**的属性（UV / 法线只依赖**片内局部坐标**）
///
/// 【为什么要它】任务 18 / P0 之后内容键流包含"按局部下标的属性词"：只有**逐下标属性逐位相同**
///   的簇才共享内容。让属性只依赖片内局部坐标 ⇒ 各片是**真正的完全副本**（几何 + 属性都相同），
///   去重照常命中；它与 `MakeTiledPatchesWithAttributes`（每片 UV 平移 = 属性不同）形成正反对照，
///   一起证明"属性进了键流"这件事既有约束力、又没有把该命中的去重掐死。
MeshAttributes MakeTileLocalAttributes(const TiledMesh& tiled, u32 quadsX, u32 quadsY) {
    const u32 side      = quadsX + 1u;
    const u32 rows      = quadsY + 1u;
    const u32 perTile   = side * rows;
    const u32 vertexCount = (u32)(tiled.mesh.positions.size() / 3u);
    MeshAttributes attributes;
    attributes.normals.resize((usize)vertexCount * 3u);
    attributes.uvs.resize((usize)vertexCount * 2u);
    for (u32 v = 0u; v < vertexCount; ++v) {
        const u32 local  = (perTile > 0u) ? (v % perTile) : 0u;
        const u32 localY = (side > 0u) ? (local / side) : 0u;
        const u32 localX = (side > 0u) ? (local % side) : 0u;
        const float localU = (quadsX > 0u) ? (float)localX / (float)quadsX : 0.0f;
        const float localV = (quadsY > 0u) ? (float)localY / (float)quadsY : 0.0f;
        attributes.uvs[(usize)v * 2u + 0u] = localU;
        attributes.uvs[(usize)v * 2u + 1u] = localV;
        const float theta = localU * 2.0f * kTask10Pi;
        const float phi   = localV * kTask10Pi;
        attributes.normals[(usize)v * 3u + 0u] = std::sin(phi) * std::cos(theta);
        attributes.normals[(usize)v * 3u + 1u] = std::cos(phi);
        attributes.normals[(usize)v * 3u + 2u] = std::sin(phi) * std::sin(theta);
    }
    return attributes;
}

/// 段对齐（与 `NaniteAlignUpFile` 同口径，测试里独立写一遍以免"用被测代码验证被测代码"）
[[nodiscard]] u64 AlignUp16(u64 value) { return (value + 15ull) & ~15ull; }

/// 打包读数行（验收要的每个数字；用 MESSAGE 打印到测试输出里）
std::string PackStatsLine(const char* label, const NanitePackedAsset& asset) {
    const NanitePackStats& s = asset.stats;
    return std::string(label) +
        " 簇=" + std::to_string(s.clusterCount) +
        " 顶点=" + std::to_string(s.vertexCount) +
        " 三角形=" + std::to_string(s.triangleCount) +
        " 材质=" + std::to_string(s.materialCount) +
        " LOD级=" + std::to_string(s.lodLevelCount) +
        " 段字节[头" + std::to_string(s.headerBytes) +
        " 簇" + std::to_string(s.clusterBytes) +
        " 顶点" + std::to_string(s.vertexBytes) +
        " 索引" + std::to_string(s.indexBytes) +
        " 材质" + std::to_string(s.materialBytes) +
        " LOD" + std::to_string(s.lodBytes) + "]" +
        " 原始字节=" + std::to_string(s.rawBytes) +
        " 总字节=" + std::to_string(s.totalBytes) +
        " meshMaxExtent=" + std::to_string(s.meshMaxExtent) +
        " 位置clamp=" + std::to_string(s.positionClampCount) +
        " 位置不一致=" + std::to_string(s.positionMismatchCount) +
        " 位置最大误差=" + std::to_string(s.maxPositionError) +
        " 位置误差上界=" + std::to_string(s.positionErrorBound) +
        " 法线最大角误差(度)=" + std::to_string(s.maxNormalAngleErrorDegrees) +
        " UV最大误差=" + std::to_string(s.maxUVError) +
        " UVclamp=" + std::to_string(s.uvClampCount) +
        " 属性冲突=" + std::to_string(s.attributeConflictCount);
}

/// **shader 侧解码的参考实现**（任务 18 的软光栅要写同一套公式）：
/// 用该簇自己的球心 + 网格最大范围 + 顶点自带的 quantBias 还原世界坐标
void DecodeClusterVertex(const NanitePackedAsset& asset, const NaniteClusterRecord& record,
                         u32 localVertex, float outPosition[3]) {
    const NaniteVertex& vertex = asset.vertices[(usize)record.vertexOffset + localVertex];
    for (u32 axis = 0; axis < 3u; ++axis) {
        outPosition[axis] = NaniteDequantizePositionAxis(
            NaniteUnpackR10G10B10A2(vertex.packedPosition, axis),
            record.boundsCenterRadius[axis], asset.stats.meshMaxExtent, vertex.quantBias);
    }
}

} // namespace

// ============================================================
// 16. 段布局：偏移/长度/对齐/总字节数（任务 7 的段表契约）
// ============================================================
TEST_CASE("NanitePack: 段布局、总字节数与字节镜像（任务 7 契约）") {
    const GridMesh mesh = MakeGrid(6);                       // 72 tri / 49 vert
    const MeshAttributes attributes = MakeSphereAttributes(mesh);
    const std::vector<NaniteMaterialRecord> materials = {
        NaniteMakeTestMaterial(0x11u, 0x22u),
        NaniteMakeTestMaterial(0x33u, 0x44u),
    };

    NaniteClusterDAG dag;
    REQUIRE(BuildNaniteClusterDAG(mesh.positions, mesh.indices, dag));
    REQUIRE(dag.stats.uniqueClusterCount > 0u);

    NanitePackedAsset asset;
    REQUIRE(PackNaniteClusters(mesh.positions, attributes.normals, attributes.uvs, materials, dag, asset));
    MESSAGE(PackStatsLine("6x6 网格（72 tri）:", asset).c_str());

    // ── 头部：计数/范围/flags 都有真值来源 ──
    CHECK(std::memcmp(asset.header.magic, kNaniteFileMagic, 8u) == 0);
    CHECK(asset.header.version == kNaniteFileVersion);
    CHECK(asset.header.clusterCount == (u32)dag.clusters.size());
    CHECK(asset.header.vertexCount == (u32)dag.uniqueVertexWords.size());
    CHECK(asset.header.indexCount == (u32)dag.uniqueTriangles.size() * kNaniteIndicesPerTriangle);
    CHECK(asset.header.materialCount == (u32)materials.size());
    CHECK(asset.header.lodLevelCount == (u32)dag.levelClusterCount.size());
    CHECK((asset.header.flags & kNaniteFileFlagHasDAG) != 0u);
    CHECK(asset.header.bboxMin[0] == 0.0f);
    CHECK(asset.header.bboxMax[0] == 6.0f);
    CHECK(asset.header.maxLODError == dag.stats.maxLODError);
    for (u32 i = 0; i < kNaniteFileHeaderReservedU32; ++i) CHECK(asset.header._reserved[i] == 0u);

    // ── 各段条数与强类型分段一致 ──
    CHECK(asset.clusters.size() == asset.header.clusterCount);
    CHECK(asset.vertices.size() == asset.header.vertexCount);
    CHECK(asset.triangles.size() * kNaniteIndicesPerTriangle == asset.header.indexCount);
    CHECK(asset.materials.size() == asset.header.materialCount);
    CHECK(asset.lodOffsets.size() == asset.header.lodLevelCount);
    // 索引段：每条三角形的三个局部下标都必须落在**该唯一内容**的顶点数内
    for (usize u = 0; u < dag.uniqueVertexCount.size(); ++u) {
        for (u32 t = 0; t < dag.uniqueTriangleCount[u]; ++t) {
            const NanitePackedTriangle& triangle = asset.triangles[dag.uniqueTriangleOffset[u] + t];
            CHECK(NaniteTriangleIndex0(triangle) < dag.uniqueVertexCount[u]);
            CHECK(NaniteTriangleIndex1(triangle) < dag.uniqueVertexCount[u]);
            CHECK(NaniteTriangleIndex2(triangle) < dag.uniqueVertexCount[u]);
        }
    }

    // ── 段表：起点 16B 对齐、长度 == alignUp(记录数 × 记录大小)、段间无重叠 ──
    const u64 rawCluster  = (u64)asset.clusters.size()  * kNaniteClusterRecordBytes;
    const u64 rawVertex   = (u64)asset.vertices.size()  * kNaniteVertexRecordBytes;
    const u64 rawIndex    = (u64)asset.triangles.size() * kNaniteIndexBytesPerTriangle;
    const u64 rawMaterial = (u64)asset.materials.size() * kNaniteMaterialRecordBytes;
    const u64 rawLod      = (u64)asset.lodOffsets.size() * kNaniteLodOffsetBytes;

    CHECK(asset.layout.headerOffset == 0u);
    CHECK(asset.layout.headerBytes == kNaniteFileHeaderBytes);
    CHECK(asset.layout.clusterOffset == kNaniteFileHeaderBytes);
    CHECK(asset.layout.clusterBytes  == AlignUp16(rawCluster));
    CHECK(asset.layout.vertexOffset  == asset.layout.clusterOffset + asset.layout.clusterBytes);
    CHECK(asset.layout.vertexBytes   == AlignUp16(rawVertex));
    CHECK(asset.layout.indexOffset   == asset.layout.vertexOffset + asset.layout.vertexBytes);
    CHECK(asset.layout.indexBytes    == AlignUp16(rawIndex));
    CHECK(asset.layout.materialOffset == asset.layout.indexOffset + asset.layout.indexBytes);
    CHECK(asset.layout.materialBytes == AlignUp16(rawMaterial));
    CHECK(asset.layout.lodOffset     == asset.layout.materialOffset + asset.layout.materialBytes);
    CHECK(asset.layout.lodBytes      == AlignUp16(rawLod));
    CHECK(asset.layout.triangleCount == (u32)asset.triangles.size());

    const u64 expectedTotal = kNaniteFileHeaderBytes + AlignUp16(rawCluster) + AlignUp16(rawVertex) +
                              AlignUp16(rawIndex) + AlignUp16(rawMaterial) + AlignUp16(rawLod);
    CHECK(asset.layout.totalBytes == (usize)expectedTotal);
    CHECK(asset.stats.totalBytes == (usize)expectedTotal);
    CHECK(asset.stats.rawBytes == (usize)(rawCluster + rawVertex + rawIndex + rawMaterial + rawLod));
    CHECK(asset.bytes.size() == asset.layout.totalBytes);
    // 每个段起点都必须 16B 对齐（任务 7 的段对齐要求）
    for (const usize offset : { asset.layout.clusterOffset, asset.layout.vertexOffset,
                                asset.layout.indexOffset, asset.layout.materialOffset,
                                asset.layout.lodOffset }) {
        CHECK(offset % kNaniteFileAlignment == 0u);
    }

    // ── 字节镜像逐字节等于强类型分段（"CPU 侧布局 == 待上传字节"的自证）──
    CHECK(std::memcmp(asset.bytes.data() + asset.layout.clusterOffset, asset.clusters.data(),
                      asset.clusters.size() * sizeof(NaniteClusterRecord)) == 0);
    CHECK(std::memcmp(asset.bytes.data() + asset.layout.vertexOffset, asset.vertices.data(),
                      asset.vertices.size() * sizeof(NaniteVertex)) == 0);
    CHECK(std::memcmp(asset.bytes.data() + asset.layout.indexOffset, asset.triangles.data(),
                      asset.triangles.size() * sizeof(NanitePackedTriangle)) == 0);
    CHECK(std::memcmp(asset.bytes.data() + asset.layout.materialOffset, asset.materials.data(),
                      asset.materials.size() * sizeof(NaniteMaterialRecord)) == 0);
    CHECK(std::memcmp(asset.bytes.data() + asset.layout.lodOffset, asset.lodOffsets.data(),
                      asset.lodOffsets.size() * sizeof(u32)) == 0);
    // 对齐填充区必须是 0（不泄漏未初始化内存到"待上传字节"里）
    for (usize i = asset.layout.lodOffset + asset.lodOffsets.size() * sizeof(u32);
         i < asset.layout.totalBytes; ++i) {
        CHECK(asset.bytes[i] == 0u);
    }

    // ── 镜像本身是一份合法且完整的 `.nanite` ──
    NaniteFileLayout checked{};
    CHECK(ValidateNaniteFile(asset.bytes.data(), asset.bytes.size(), &checked) == NaniteFileError::None);
    CHECK(checked.totalBytes == asset.layout.totalBytes);
    CHECK(checked.triangleCount == asset.layout.triangleCount);

    // ── LOD 段：每级一个 u32 = 该级第一个出现簇的下标（任务 10 定的语义）──
    REQUIRE(asset.lodOffsets.size() == dag.levelClusterOffset.size() - 1u);
    for (usize level = 0; level < asset.lodOffsets.size(); ++level) {
        CHECK(asset.lodOffsets[level] == dag.levelClusterOffset[level]);
    }

    // ── 材质段：原样搬运（不解析 ID）──
    REQUIRE(asset.materials.size() == materials.size());
    for (usize i = 0; i < materials.size(); ++i) {
        CHECK(asset.materials[i].bindlessTextureBase == materials[i].bindlessTextureBase);
        CHECK(asset.materials[i].textureMask == materials[i].textureMask);
        CHECK(asset.materials[i].metallicFactor == materials[i].metallicFactor);
    }
    // 簇段的 materialID：本任务统一写 0（归属任务 12/19）
    for (const NaniteClusterRecord& record : asset.clusters) CHECK(record.materialID == 0u);
}

// ============================================================
// 17. 量化误差实测：位置 / 法线 / UV + "无 clamp" + 与 DAG 位置词口径一致
// ============================================================
TEST_CASE("NanitePack: 量化误差实测（位置/法线/UV）与无 clamp") {
    const GridMesh mesh = MakeGrid(32);                      // 2048 tri / 1089 vert
    const MeshAttributes attributes = MakeSphereAttributes(mesh);
    const std::vector<NaniteMaterialRecord> materials;       // 空材质表（合法）

    NaniteClusterDAG dag;
    REQUIRE(BuildNaniteClusterDAG(mesh.positions, mesh.indices, dag));

    NanitePackedAsset asset;
    REQUIRE(PackNaniteClusters(mesh.positions, attributes.normals, attributes.uvs, materials, dag, asset));
    MESSAGE(PackStatsLine("32x32 网格（2048 tri，空材质表）:", asset).c_str());

    const NanitePackStats& stats = asset.stats;
    CHECK(stats.clusterCount > 0u);
    CHECK(stats.vertexCount > 0u);
    CHECK(stats.materialCount == 0u);
    CHECK(stats.meshMaxExtent == 32.0f);                     // 32×32 网格的最大范围
    CHECK(stats.positionErrorBound == doctest::Approx(stats.meshMaxExtent / 2044.0f));

    // ① 位置：**没有 clamp**（口径证明见 NaniteTypes.h），且与 DAG 的位置词逐位一致
    CHECK(stats.positionClampCount == 0u);
    CHECK(stats.positionMismatchCount == 0u);
    // ② 位置往返误差 ≤ meshMaxExtent/2044（半个量化步）；实测值由 MESSAGE 给出
    CHECK(stats.maxPositionError > 0.0f);
    CHECK(stats.maxPositionError <= stats.positionErrorBound * 1.001f);
    // ③ 法线：八面体 10+10 位的角误差 ≤ 0.5°（比 TestNaniteTypes 的稠密球面采样更"真实"：
    //    这里量的是实际网格顶点的法线场）
    CHECK(stats.maxNormalAngleErrorDegrees > 0.0f);
    CHECK(stats.maxNormalAngleErrorDegrees <= stats.normalAngleErrorBoundDegrees);
    // ④ UV：unorm16 的往返误差 ≤ 1/65535，且本网格的 UV 全在 [0,1] 内（没有 clamp）
    CHECK(stats.maxUVError > 0.0f);
    CHECK(stats.maxUVError <= stats.uvErrorBound);
    CHECK(stats.uvClampCount == 0u);
    // ⑤ 该网格没有重复内容（去重率 0）⇒ 每个唯一内容只有一个出现 ⇒ 不可能有属性冲突
    CHECK(dag.stats.dedupRate == 0.0f);
    CHECK(stats.attributeConflictCount == 0u);

    // 顶点段里的每个位置词都带 §8.4 定稿的 quantBias（自包含解码）
    for (const NaniteVertex& vertex : asset.vertices) {
        CHECK(vertex.quantBias == kNaniteVertexQuantBias);
        // 位置的 w 域恒为 1（§8.4 的 R10G10B10A2_SNORM + w=1 约定）
        CHECK(NaniteUnpackR10G10B10A2(vertex.packedPosition, 3u) == 1u);
        // 法线的 z/w 域保留 0（八面体只占 x/y）
        CHECK(NaniteUnpackR10G10B10A2(vertex.packedNormal, 2u) == 0u);
        CHECK(NaniteUnpackR10G10B10A2(vertex.packedNormal, 3u) == 0u);
    }
}

// ============================================================
// 18. 与 DAG 的衔接：打包后每个出现簇的 vertexOffset/triangleOffset 能解回它的几何
//
// 用**平铺网格**（去重率 > 0）才能真正覆盖这条口径：同一份共享顶点（同一个 `vertexOffset`）
// 被多个位置的簇引用，解码必须靠**各自的簇心**还原世界坐标（§14.19 硬约束①）。
// ============================================================
TEST_CASE("NanitePack: DAG 衔接（共享内容 + 各自簇心解码回几何）") {
    const TiledMesh tiled = MakeTiledPatches(3, 3, 8, 4, 20.0f);   // 9 片 × 64 tri
    // 【任务 18 / P0】属性也交给 DAG（五参数重载）：本用例要走**真实的资产路径**
    //   （`BuildNaniteAssetFromGeometry` 恒带属性），属性是"逐片相同"的真副本 ⇒ 共享照常命中。
    const MeshAttributes attributes = MakeTileLocalAttributes(tiled, 8, 4);
    const std::vector<NaniteMaterialRecord> materials = { NaniteMakeTestMaterial(7u, 9u) };

    NaniteClusterDAG dag;
    REQUIRE(BuildNaniteClusterDAG(tiled.mesh.positions, attributes.normals, attributes.uvs,
                                  tiled.mesh.indices, dag));
    REQUIRE(dag.stats.dedupRate > 0.0f);                    // 有共享内容才有"衔接"可测
    REQUIRE(dag.stats.levelCount > 0u);

    NanitePackedAsset asset;
    REQUIRE(PackNaniteClusters(tiled.mesh.positions, attributes.normals, attributes.uvs,
                               materials, dag, asset));
    MESSAGE(PackStatsLine("平铺网格 3x3（9×64 tri，含共享内容）:", asset).c_str());
    CHECK(asset.stats.attributeConflictCount == 0u);   // 属性进键流 ⇒ 共享簇必然逐位一致

    const float bound = asset.stats.positionErrorBound;
    const u32   fullCheckClusters = 4u;    // 前 4 个簇逐顶点全查，其余每簇查前 8 个（验收口径）
    // 解码的**浮点舍入余量**：解码式 `origin + signed/1022*range` 在 |origin| ~ 50、range = 48 下
    // 有 binary32 的舍入（ulp ≈ 50×2^-23 ≈ 6e-6，两次运算 ⇒ ≲2e-5）；取 1e-4（5 倍余量）后，
    // 仍然能抓住"错一个量化步"（= 0.047，比它大 470 倍）这类真实错误。
    const float decodeSlack = 1.0e-4f;
    // `positionErrorBound` 是**每轴**的上界（半个量化步）⇒ 三维点距离的上界要乘 √3
    const float pointSetBound = bound * 1.7320508f + decodeSlack;

    // 每个唯一内容的"首次出现"（与打包器同一口径）：首份出现的**下标一一对应**必须精确成立，
    // 其余出现只能用"点集匹配"（原因见下面的注释）。
    std::vector<u32> firstOfUnique(dag.uniqueVertexCount.size(), 0xFFFFFFFFu);
    for (u32 c = 0u; c < (u32)asset.clusters.size(); ++c) {
        const u32 unique = dag.clusterUnique[c];
        if (firstOfUnique[unique] == 0xFFFFFFFFu) firstOfUnique[unique] = c;
    }

    u32   checkedVertices       = 0u;
    u32   orderMismatchClusters = 0u;   ///< 簇内顶点顺序与"首份出现"不一致的簇数（诊断读数）
    float maxPointSetError      = 0.0f; ///< 点集匹配（最近点）误差 —— **验收判据**
    float maxDirectError        = 0.0f; ///< 下标一一对应误差 —— 诊断读数

    // ── ① 每个出现簇：顶点段/索引段的偏移必须落在段内；顶点段解码出的**点集**必须回到该簇几何 ──
    //
    // 【为什么"下标一一对应"不是判据】本用例刻意用**不带属性**的三参数重载：内容键流只覆盖
    //   "位置 + 拓扑"（任务 9 口径），而任务 8 的 `meshopt_optimizeMeshlet` 会按拓扑就地重排
    //   每个簇的顶点。于是两个"内容相同"的簇可以有不同的**簇内顶点顺序**：共享的
    //   `uniqueVertexWords`/`uniqueTriangles` 是首份出现的那一套一致配对（几何因此完全正确），
    //   但"第 v 个局部顶点"在两次出现里未必指向同一个网格顶点。故验收判据是"解码点集 == 输入
    //   点集"（rasterizer 消费的正是点集 + 三角形），而"下标一一对应"只在首份出现上必须精确成立。
    //   **属性一致性不由本用例承担**：任务 18 / P0 起，属性词进了内容键流，凡是按局部下标取
    //   法线/UV 的路径都必须用**五参数**重载（`BuildNaniteAssetFromGeometry` 恒用它），
    //   那条不变式由用例 19「共享簇必须属性逐位一致」断言。
    for (usize c = 0; c < asset.clusters.size(); ++c) {
        const NaniteClusterRecord& record = asset.clusters[c];
        const u32 localVertices  = dag.clusterVertexCount[c];
        const u32 localTriangles = record.triangleCount;
        REQUIRE((usize)record.vertexOffset + localVertices <= asset.vertices.size());
        REQUIRE((usize)record.triangleOffset + localTriangles <= asset.triangles.size());

        const u32 indexBase = dag.clusterVertexIndexOffset[c];
        const u32 limit = (c < fullCheckClusters) ? localVertices : std::min(localVertices, 8u);
        bool orderMatches = true;
        for (u32 v = 0u; v < limit; ++v) {
            float decoded[3] = { 0.0f, 0.0f, 0.0f };
            DecodeClusterVertex(asset, record, v, decoded);

            // (a) 最近点匹配：在该簇自己的输入点集里找最接近的一个 —— 验收判据
            float nearest = 1.0e30f;
            for (u32 u = 0u; u < localVertices; ++u) {
                const u32 meshVertex = dag.clusterVertexIndices[indexBase + u];
                float distanceSquared = 0.0f;
                for (u32 axis = 0; axis < 3u; ++axis) {
                    const float delta = decoded[axis] -
                        tiled.mesh.positions[(usize)meshVertex * 3u + axis];
                    distanceSquared += delta * delta;
                }
                if (distanceSquared < nearest) nearest = distanceSquared;
            }
            const float pointSetError = std::sqrt(nearest);
            if (pointSetError > maxPointSetError) maxPointSetError = pointSetError;
            if (pointSetError > pointSetBound) {
                MESSAGE("DIAG cluster=" << c << " level=" << dag.clusterLevel[c]
                        << " v=" << v << " origin=(" << record.boundsCenterRadius[0] << ","
                        << record.boundsCenterRadius[1] << "," << record.boundsCenterRadius[2]
                        << ") decoded=(" << decoded[0] << "," << decoded[1] << "," << decoded[2]
                        << ") error=" << pointSetError
                        << " tris=" << localTriangles << " verts=" << localVertices
                        << " unique=" << dag.clusterUnique[c]
                        << " firstCluster=" << firstOfUnique[dag.clusterUnique[c]]);
            }
            CHECK(pointSetError <= pointSetBound);

            // (b) 下标一一对应：诊断读数；首份出现必须精确成立
            const u32 meshVertex = dag.clusterVertexIndices[indexBase + v];
            for (u32 axis = 0; axis < 3u; ++axis) {
                const float source = tiled.mesh.positions[(usize)meshVertex * 3u + axis];
                const float error  = std::fabs(decoded[axis] - source);
                if (error > maxDirectError) maxDirectError = error;
                if (error > bound + decodeSlack) {
                    orderMatches = false;
                    if (firstOfUnique[dag.clusterUnique[c]] == (u32)c) {
                        // 首份出现的词就是按它自己的映射编出来的 ⇒ 这里**必须**精确对应
                        CHECK(error <= bound + decodeSlack);
                    }
                }
            }
            ++checkedVertices;
        }
        if (!orderMatches) ++orderMismatchClusters;

        // 索引段：三个局部下标都必须落在本簇顶点数内（u16 打包语义 + 每簇 ≤128）
        for (u32 t = 0u; t < localTriangles; ++t) {
            const NanitePackedTriangle& triangle = asset.triangles[record.triangleOffset + t];
            const u32 i0 = NaniteTriangleIndex0(triangle);
            const u32 i1 = NaniteTriangleIndex1(triangle);
            const u32 i2 = NaniteTriangleIndex2(triangle);
            CHECK(i0 < localVertices);
            CHECK(i1 < localVertices);
            CHECK(i2 < localVertices);
            CHECK(IsValidClusterLocalVertexIndex(i0));
            CHECK(IsValidClusterLocalVertexIndex(i1));
            CHECK(IsValidClusterLocalVertexIndex(i2));
        }
    }
    CHECK(checkedVertices > 0u);
    MESSAGE("DAG 衔接：逐顶点核对 " << checkedVertices << " 个（前 " << fullCheckClusters
            << " 簇全查，其余每簇前 8 个）；点集匹配(3D)最大误差=" << maxPointSetError
            << " 上界=√3×" << bound << "+余量=" << pointSetBound
            << "；下标一一对应最大误差=" << maxDirectError
            << "（顺序与首份不一致的簇数=" << orderMismatchClusters << "/"
            << asset.clusters.size() << "）");

    // ── ② 共享内容的证据：同一个 vertexOffset 被多个出现簇引用 ──
    std::vector<u32> referenceCount(asset.vertices.size() + 1u, 0u);
    for (const NaniteClusterRecord& record : asset.clusters) {
        if ((usize)record.vertexOffset < referenceCount.size()) ++referenceCount[record.vertexOffset];
    }
    u32 maxReferences = 0u;
    for (const u32 count : referenceCount) maxReferences = std::max(maxReferences, count);
    CHECK(maxReferences > 1u);   // 平铺网格里"同一份内容被多个位置的簇引用"
    MESSAGE("DAG 衔接：同一 vertexOffset 被引用的最大次数=" << maxReferences
            << "（>1 = 共享内容真的生效）");

    // ── ③ 打包不改动 DAG（"输入只读"的契约）──
    NaniteClusterDAG rebuilt;
    REQUIRE(BuildNaniteClusterDAG(tiled.mesh.positions, attributes.normals, attributes.uvs,
                                  tiled.mesh.indices, rebuilt));
    CHECK(SameDAG(dag, rebuilt));
}

// ============================================================
// 19. 【任务 18 / P0】共享簇必须属性逐位一致（修掉 §14.20⑥ 的"属性错配"）
//
// 修复前（任务 9/10 的口径）：内容哈希只覆盖"位置 + 拓扑"，而 `meshopt_optimizeMeshlet` 会按
//   拓扑就地重排簇内顶点 ⇒ 位置/拓扑相同但**局部顶点顺序**不同的簇会共享顶点段，
//   按局部下标取法线/UV 就会读到别的顶点 —— 实测 3×3 平铺 19 个出现里 8 个顺序不同、
//   `attributeConflictCount = 1`（当时的用例断言的就是"> 0 如实报告"）。
// 修复后（本用例的断言）：属性词（法线词 + UV 词）按局部下标写进规范键流 ⇒
//   ① `attributeConflictCount` **恒为 0**（从"如实报告"变成硬断言）；
//   ② 逐唯一内容 × 逐局部下标，全部出现重算出的属性词与写进顶点段的那一份**逐位相同**；
//   ③ 每片 UV 平移的网格（属性真的不同）**不再共享**（去重率显著低于无属性键流）；
//   ④ 逐片完全相同的副本（属性也相同）**仍然显著去重**（> 10%），证明约束没有过度收紧。
// ============================================================
TEST_CASE("NaniteDAG: 共享簇必须属性逐位一致（任务 18 / P0 修复）") {
    // 平铺网格 + 每片 UV 平移 ⇒ 位置/拓扑完全相同（旧键流会去重命中）但 UV 不同
    TiledMeshWithAttributes tiled = MakeTiledPatchesWithAttributes(4, 4, 8, 4, 20.0f);
    REQUIRE(tiled.mesh.indices.size() == 16u * 64u * 3u);

    // ── ① 属性进键流 ⇒ 这些簇不再共享（对照组：同一网格不带属性的旧口径仍显著去重）──
    NaniteClusterDAG dagNoAttrs;
    REQUIRE(BuildNaniteClusterDAG(tiled.mesh.positions, tiled.mesh.indices, dagNoAttrs));
    MESSAGE(DAGStatsLine("(对照) 平铺网格 16×64 tri（属性**不进**键流，任务 9 口径）:", dagNoAttrs).c_str());
    CHECK(dagNoAttrs.stats.dedupRate > 0.10f);

    NaniteClusterDAG dagWithAttrs;
    REQUIRE(BuildNaniteClusterDAG(tiled.mesh.positions, tiled.attributes.normals,
                                  tiled.attributes.uvs, tiled.mesh.indices, dagWithAttrs));
    MESSAGE(DAGStatsLine("(修复后) 平铺网格（每片 UV 平移 ⇒ 属性不同，不再共享）:", dagWithAttrs).c_str());
    // 属性不同 ⇒ 共享被键流挡掉：去重率必须**严格下降**（多数情况下直接到 0）
    CHECK(dagWithAttrs.stats.dedupRate < dagNoAttrs.stats.dedupRate);

    // ── ② 硬断言：共享簇（= 同一 uniqueIndex 的全部出现）逐局部下标的属性词逐位一致 ──
    NanitePackedAsset asset;
    REQUIRE(PackNaniteClusters(tiled.mesh.positions, tiled.attributes.normals,
                               tiled.attributes.uvs, {}, dagWithAttrs, asset));
    MESSAGE(PackStatsLine("平铺网格（每片 UV 平移，属性进键流）:", asset).c_str());
    CHECK(asset.stats.attributeConflictCount == 0u);

    u32 comparedVertices = 0u;
    u32 conflictVertices = 0u;
    for (usize c = 0u; c < dagWithAttrs.clusters.size(); ++c) {
        const NaniteClusterRecord& record = dagWithAttrs.clusters[c];
        const u32 localVertices = dagWithAttrs.clusterVertexCount[c];
        const u32 indexBase     = dagWithAttrs.clusterVertexIndexOffset[c];
        for (u32 v = 0u; v < localVertices; ++v) {
            const u32 meshVertex = dagWithAttrs.clusterVertexIndices[indexBase + v];
            const float* normal  = tiled.attributes.normals.data() + (usize)meshVertex * 3u;
            const float* uv      = tiled.attributes.uvs.data() + (usize)meshVertex * 2u;
            const u32 normalWord = NanitePackNormal(normal[0], normal[1], normal[2]);
            const u32 uvWord     = NanitePackUV(NaniteQuantizeUV(uv[0]), NaniteQuantizeUV(uv[1]));
            const NaniteVertex& written = asset.vertices[(usize)record.vertexOffset + v];
            if (written.packedNormal != normalWord || written.packedUV != uvWord) ++conflictVertices;
            ++comparedVertices;
        }
    }
    CHECK(comparedVertices > 0u);
    CHECK(conflictVertices == 0u);
    MESSAGE("属性逐位核对：检查 " << comparedVertices << " 个'出现 × 局部下标'，"
            << "不一致 " << conflictVertices << " 个（必须 0）");

    // ── ③ 正面照：逐片完全相同的副本（属性也逐位相同）仍然显著去重（> 10%）──
    {
        const TiledMesh plain = MakeTiledPatches(4, 4, 8, 4, 20.0f);
        const MeshAttributes tileLocal = MakeTileLocalAttributes(plain, 8, 4);
        NaniteClusterDAG dagCopies;
        REQUIRE(BuildNaniteClusterDAG(plain.mesh.positions, tileLocal.normals, tileLocal.uvs,
                                      plain.mesh.indices, dagCopies));
        MESSAGE(DAGStatsLine("(正面照) 平铺网格（逐片属性完全相同的真副本）:", dagCopies).c_str());
        CHECK(dagCopies.stats.dedupRate > 0.10f);   // 任务 9 的"平铺 > 10%"判据必须继续成立

        NanitePackedAsset copyAsset;
        REQUIRE(PackNaniteClusters(plain.mesh.positions, tileLocal.normals, tileLocal.uvs, {},
                                   dagCopies, copyAsset));
        CHECK(copyAsset.stats.attributeConflictCount == 0u);
    }

    // ── ④ 属性是顶点位置的函数时（`MakeSphereAttributes` 按整网格 AABB 归一化）同样恒为 0 ──
    {
        const MeshAttributes uniform = MakeSphereAttributes(tiled.mesh);
        NaniteClusterDAG dagUniform;
        REQUIRE(BuildNaniteClusterDAG(tiled.mesh.positions, uniform.normals, uniform.uvs,
                                      tiled.mesh.indices, dagUniform));
        NanitePackedAsset uniformAsset;
        REQUIRE(PackNaniteClusters(tiled.mesh.positions, uniform.normals, uniform.uvs, {},
                                   dagUniform, uniformAsset));
        MESSAGE("随位置变化的 UV 属性（属性进键流）：去重率="
                << dagUniform.stats.dedupRate << " 属性冲突="
                << uniformAsset.stats.attributeConflictCount);
        CHECK(uniformAsset.stats.attributeConflictCount == 0u);
    }
}

// ============================================================
// 19b. 【任务 18 / P0 的负向回归】绕过属性键流（三参数 DAG）+ 带属性打包 ⇒ **拒绝产出**
//
// 用例 19 证明"按五参数重载走的路"属性必然一致；这一条证明**另一条路走不通**：
//   手工用三参数重载拼一份"只按位置/拓扑去重"的 DAG，再拿带属性的几何去打包 ⇒
//   必然出现"同一共享内容的不同出现给出不同法线/UV" ⇒ 打包器**返回 false**（不改写出参），
//   而不是静默写出一份属性错配的资产。这正是 §14.20⑥ 缺陷不再可能复现的守卫。
// ============================================================
TEST_CASE("NanitePack: 属性错配的资产被拒绝（P0 修复的负向回归）") {
    TiledMeshWithAttributes tiled = MakeTiledPatchesWithAttributes(4, 4, 8, 4, 20.0f);
    REQUIRE(tiled.mesh.indices.size() == 16u * 64u * 3u);

    // 三参数重载：键流只覆盖"位置 + 拓扑"（任务 9 口径）⇒ 每片 UV 平移的簇仍会被共享
    NaniteClusterDAG legacyDag;
    REQUIRE(BuildNaniteClusterDAG(tiled.mesh.positions, tiled.mesh.indices, legacyDag));
    REQUIRE(legacyDag.stats.dedupRate > 0.10f);

    NanitePackedAsset poisoned;
    poisoned.stats.clusterCount = 12345u;                 // 先污染，失败路径必须整体不改写
    CHECK_FALSE(PackNaniteClusters(tiled.mesh.positions, tiled.attributes.normals,
                                   tiled.attributes.uvs, {}, legacyDag, poisoned));
    CHECK(poisoned.stats.clusterCount == 12345u);         // 出参未被改写
    CHECK(poisoned.bytes.empty());

    // 对照：同一份 DAG、**不带属性**打包 ⇒ 合法（属性段退化为默认值，与任务 9 的口径一致）
    NanitePackedAsset geometryOnly;
    REQUIRE(PackNaniteClusters(tiled.mesh.positions, {}, {}, {}, legacyDag, geometryOnly));
    CHECK(geometryOnly.stats.attributeConflictCount == 0u);
    CHECK(geometryOnly.header.clusterCount == legacyDag.stats.totalClusterCount);
}

// ============================================================
// 20. 打包的可复现性与失败路径（空 DAG / 非法输入 / 不改写出参）
// ============================================================
TEST_CASE("NanitePack: 可复现性与失败路径（空 DAG / 非法输入）") {
    const GridMesh mesh = MakeGrid(6);
    const MeshAttributes attributes = MakeSphereAttributes(mesh);
    const std::vector<NaniteMaterialRecord> materials = { NaniteMakeTestMaterial(1u, 2u) };

    NaniteClusterDAG dag;
    REQUIRE(BuildNaniteClusterDAG(mesh.positions, mesh.indices, dag));

    // ── ① 同一输入两次打包逐字节一致（字节镜像逐字节比，含填充区）──
    NanitePackedAsset first;
    NanitePackedAsset second;
    REQUIRE(PackNaniteClusters(mesh.positions, attributes.normals, attributes.uvs, materials, dag, first));
    REQUIRE(PackNaniteClusters(mesh.positions, attributes.normals, attributes.uvs, materials, dag, second));
    CHECK(first.bytes == second.bytes);
    CHECK(first.bytes.size() == second.bytes.size());
    CHECK(std::memcmp(&first.stats, &second.stats, sizeof(NanitePackStats)) == 0);
    CHECK(std::memcmp(&first.header, &second.header, sizeof(NaniteFileHeader)) == 0);

    // ── ② 空 DAG：合法输入 ⇒ 只有 96B 头部、计数全 0、仍是一份合法 `.nanite` ──
    {
        const std::vector<float> emptyPositions;
        const std::vector<u32>   emptyIndices;
        NaniteClusterDAG emptyDag;
        REQUIRE(BuildNaniteClusterDAG(emptyPositions, emptyIndices, emptyDag));
        REQUIRE(emptyDag.Empty());

        NanitePackedAsset emptyAsset;
        emptyAsset.stats.clusterCount = 99u;   // 先污染，成功路径必须整体写满
        REQUIRE(PackNaniteClusters(emptyPositions, {}, {}, {}, emptyDag, emptyAsset));
        CHECK(emptyAsset.Empty());
        CHECK(emptyAsset.header.clusterCount == 0u);
        CHECK(emptyAsset.header.vertexCount == 0u);
        CHECK(emptyAsset.header.indexCount == 0u);
        CHECK(emptyAsset.header.materialCount == 0u);
        CHECK(emptyAsset.header.lodLevelCount == 0u);
        CHECK(emptyAsset.stats.clusterCount == 0u);
        CHECK(emptyAsset.layout.totalBytes == kNaniteFileHeaderBytes);
        CHECK(emptyAsset.bytes.size() == kNaniteFileHeaderBytes);
        CHECK(ValidateNaniteFile(emptyAsset.bytes.data(), emptyAsset.bytes.size()) ==
              NaniteFileError::None);
    }

    // ── ③ 非法输入：返回 false 且**不改写出参**（与任务 7/8/9 同口径）──
    {
        const auto makePoisoned = []() {
            NanitePackedAsset asset;
            asset.stats.clusterCount = 42u;
            asset.bytes.resize(7u);
            asset.clusters.resize(3u);
            return asset;
        };
        // 位置个数不是 3 的倍数
        {
            std::vector<float> badPositions(mesh.positions.begin(), mesh.positions.begin() + 4);
            NanitePackedAsset asset = makePoisoned();
            CHECK_FALSE(PackNaniteClusters(badPositions, attributes.normals, attributes.uvs,
                                           materials, dag, asset));
            CHECK(asset.stats.clusterCount == 42u);
            CHECK(asset.bytes.size() == 7u);
            CHECK(asset.clusters.size() == 3u);
        }
        // 法线长度不是 顶点数 × 3
        {
            std::vector<float> badNormals(attributes.normals.begin(), attributes.normals.end() - 1);
            NanitePackedAsset asset = makePoisoned();
            CHECK_FALSE(PackNaniteClusters(mesh.positions, badNormals, attributes.uvs,
                                           materials, dag, asset));
            CHECK(asset.stats.clusterCount == 42u);
        }
        // UV 长度不是 顶点数 × 2
        {
            std::vector<float> badUvs(attributes.uvs.begin(), attributes.uvs.end() - 1);
            NanitePackedAsset asset = makePoisoned();
            CHECK_FALSE(PackNaniteClusters(mesh.positions, attributes.normals, badUvs,
                                           materials, dag, asset));
            CHECK(asset.stats.clusterCount == 42u);
        }
        // DAG 内部不一致（出现记录的局部顶点数超出共享内容）⇒ 拒绝而不是越界读
        {
            NaniteClusterDAG broken = dag;
            broken.clusterVertexCount[0] += 1u;
            NanitePackedAsset asset = makePoisoned();
            CHECK_FALSE(PackNaniteClusters(mesh.positions, attributes.normals, attributes.uvs,
                                           materials, broken, asset));
            CHECK(asset.stats.clusterCount == 42u);
        }
        // 出现记录指向的网格顶点越界 ⇒ 拒绝
        {
            NaniteClusterDAG broken = dag;
            broken.clusterVertexIndices[0] = 0xFFFFFFFFu;
            NanitePackedAsset asset = makePoisoned();
            CHECK_FALSE(PackNaniteClusters(mesh.positions, attributes.normals, attributes.uvs,
                                           materials, broken, asset));
            CHECK(asset.stats.clusterCount == 42u);
        }
    }

    // ── ④ 属性 span 可空：法线/UV 全空时用默认值编码，仍能打包成功（口径自洽）──
    {
        NanitePackedAsset asset;
        REQUIRE(PackNaniteClusters(mesh.positions, {}, {}, materials, dag, asset));
        CHECK(asset.stats.vertexCount > 0u);
        CHECK(asset.stats.uvClampCount == 0u);
        // 默认 UV = (0,0) ⇒ 每个顶点的 packedUV 都是 0
        for (const NaniteVertex& vertex : asset.vertices) CHECK(vertex.packedUV == 0u);
        // 默认法线 = +Z ⇒ 解码角误差极小（八面体 10 位的量化误差，不超过阈值）
        CHECK(asset.stats.maxNormalAngleErrorDegrees <= asset.stats.normalAngleErrorBoundDegrees);
        // 位置/索引段与带属性时完全一致（法线/UV 不影响位置与拓扑）
        CHECK(asset.stats.positionMismatchCount == 0u);
        CHECK(asset.stats.positionClampCount == 0u);
    }

    // ── ⑤ 材质条数为 1 时的搬运与索引段不变（防 0/1/2 条材质的边界）──
    {
        NanitePackedAsset asset;
        const std::vector<NaniteMaterialRecord> single = { NaniteMakeTestMaterial(0xABu, 0xCDu) };
        REQUIRE(PackNaniteClusters(mesh.positions, attributes.normals, attributes.uvs,
                                   single, dag, asset));
        CHECK(asset.header.materialCount == 1u);
        CHECK(asset.layout.materialBytes == AlignUp16(kNaniteMaterialRecordBytes));
        CHECK(asset.materials[0].bindlessTextureBase == 0xABu);
        CHECK(asset.materials[0].textureMask == 0xCDu);
    }
}

// ============================================================
// 21. §14.8 任务 12：资产加载入口（合并几何快照 → `.nanite` 字节镜像，CPU 侧）
//
// 【为什么补这一条】`BuildNaniteAssetFromGeometry()` 是任务 12 新加的公共入口（渲染侧用它把
//   `MeshBatcher` 的合并几何**一次性**转成待上传的字节镜像）。它虽然只是任务 9 + 任务 10 的
//   顺序组合，但仍必须钉住三件事：
//     ① 产物是一份**合法且自洽**的 `.nanite`（能用 `ValidateNaniteFile` 校验、计数与段表一致）；
//     ② 空几何 ⇒ 合法空资产（96B 头、计数全 0），上层据此跳过 GPU 上传；
//     ③ 失败 ⇒ 返回 false 且**不改写出参**（与任务 7/8/9/10 同口径）。
//   至于"上了 GPU 之后字节是否一致"，由任务 12 运行期的真实读回校验负责（本文件不碰 RHI）。
// ============================================================
TEST_CASE("NaniteUpload: 合并几何快照 → .nanite 资产（任务 12 的 CPU 侧入口）") {
    const GridMesh       mesh       = MakeGrid(8);            // 8×8 四边形 = 128 三角形（多级 LOD）
    const MeshAttributes attributes = MakeSphereAttributes(mesh);

    // ── ① 入口产物与"手工 DAG + Pack"逐字节一致：入口不许引入任何额外加工/重排 ──
    NanitePackedAsset viaEntry;
    REQUIRE(BuildNaniteAssetFromGeometry(mesh.positions, attributes.normals, attributes.uvs,
                                         mesh.indices, {}, viaEntry));
    NaniteClusterDAG dag;
    REQUIRE(BuildNaniteClusterDAG(mesh.positions, attributes.normals, attributes.uvs,
                                  mesh.indices, dag));
    NanitePackedAsset viaCalls;
    REQUIRE(PackNaniteClusters(mesh.positions, attributes.normals, attributes.uvs, {}, dag, viaCalls));
    CHECK(viaEntry.bytes == viaCalls.bytes);

    // ── ② 产物自洽：镜像长度 = 段表总长；校验通过；头部计数与各段/统计一致 ──
    CHECK(viaEntry.bytes.size() == viaEntry.layout.totalBytes);
    CHECK(ValidateNaniteFile(viaEntry.bytes.data(), viaEntry.bytes.size()) == NaniteFileError::None);
    CHECK(viaEntry.header.clusterCount == (u32)viaEntry.clusters.size());
    CHECK(viaEntry.header.clusterCount > 0u);
    CHECK(viaEntry.header.vertexCount  == (u32)viaEntry.vertices.size());
    CHECK(viaEntry.header.lodLevelCount > 0u);
    CHECK(viaEntry.header.materialCount == 0u);   // 材质段留空：逐簇材质解析属任务 19
    CHECK(viaEntry.stats.clusterCount == viaEntry.header.clusterCount);
    CHECK(viaEntry.stats.vertexCount  == viaEntry.header.vertexCount);
    CHECK(viaEntry.stats.triangleCount * kNaniteIndicesPerTriangle == viaEntry.header.indexCount);

    MESSAGE("任务 12 资产入口：镜像 " << viaEntry.bytes.size() << " 字节，簇 "
            << viaEntry.header.clusterCount << "，顶点 " << viaEntry.header.vertexCount
            << "，索引 " << viaEntry.header.indexCount
            << "，LOD 级 " << viaEntry.header.lodLevelCount);

    // ── ③ 确定性：同一输入两次调用逐位一致（入口自身不加任何状态/缓存）──
    NanitePackedAsset again;
    REQUIRE(BuildNaniteAssetFromGeometry(mesh.positions, attributes.normals, attributes.uvs,
                                         mesh.indices, {}, again));
    CHECK(again.bytes == viaEntry.bytes);
    CHECK(std::memcmp(&again.header, &viaEntry.header, sizeof(NaniteFileHeader)) == 0);

    // ── ④ 空几何：合法空资产（96B 头、计数全 0）⇒ 上层据此跳过 GPU 上传 ──
    {
        NanitePackedAsset empty;
        REQUIRE(BuildNaniteAssetFromGeometry({}, {}, {}, {}, {}, empty));
        CHECK(empty.bytes.size() == kNaniteFileHeaderBytes);
        CHECK(empty.bytes.size() == empty.layout.totalBytes);
        CHECK(empty.header.clusterCount  == 0u);
        CHECK(empty.header.vertexCount   == 0u);
        CHECK(empty.header.indexCount    == 0u);
        CHECK(empty.header.materialCount == 0u);
        CHECK(empty.header.lodLevelCount == 0u);
        CHECK(ValidateNaniteFile(empty.bytes.data(), empty.bytes.size()) == NaniteFileError::None);
    }

    // ── ⑤ 失败路径：索引越界 ⇒ false，且**不改写**出参 ──
    {
        std::vector<u32> badIndices = mesh.indices;
        badIndices[1] = 0xFFFFFFFFu;   // 越界索引（≥ 顶点数）
        NanitePackedAsset poisoned;
        poisoned.stats.clusterCount = 77u;
        poisoned.bytes.resize(5u);
        CHECK_FALSE(BuildNaniteAssetFromGeometry(mesh.positions, attributes.normals, attributes.uvs,
                                                 badIndices, {}, poisoned));
        CHECK(poisoned.stats.clusterCount == 77u);
        CHECK(poisoned.bytes.size() == 5u);
    }
}

// ============================================================
// 22. §14.8 任务 14：per-instance cluster BVH 的构建（节点数/深度上界/叶子容量/确定性）
//
// 【本组用例覆盖的验收项】（§14.8 任务 14："BVH 节点数与遍历访问数可复现"）
//   ① 节点数与深度的**上界**：节点数 = 2 × 叶子数 - 1（满二叉树）、深度 ≤ `kNaniteBVHMaxDepth`；
//   ② 叶子容量：每个叶子 ≤ `kNaniteBVHLeafCapacity`（深度上限未触发时）；
//   ③ 结构自洽：叶子簇表是 [0, 簇数) 的**排列**（不丢不重）、父子下标不越界、
//      父球包含孩子球 / 叶子球包含其簇球（"父不可见 ⇒ 整棵子树跳过"的前提）；
//   ④ 三种网格规模（72 tri / 2048 tri / 3×3 平铺 576 tri）都能建出 BVH 并给出读数；
//   ⑤ 同一输入两次构建**逐位可复现**；
//   ⑥ 空表与单簇边界（合法输入，不崩、不越界）。
//
// 【为什么在构建器这一侧做遍历断言】"访问数"取决于树形状，而树形状是构建器的产物；这里用
//   **盒状视锥 + 手工实例表**把"全部在内 / 全部在外 / 部分相交"三类算成已知数（见下一个用例），
//   避免把验收建立在"某个具体网格碰巧的读数"上。
// ============================================================

namespace {

/// 盒状视锥 [-half, half]^3（平面法线朝内；与任务 13 的单测同一口径，不引入相机/矩阵的间接性）
NaniteFrustumPlanes MakeBoxFrustum(float halfExtent) {
    NaniteFrustumPlanes frustum{};
    const float planes[6][4] = {
        {  1.0f, 0.0f, 0.0f, halfExtent },   // 左：  x >= -half
        { -1.0f, 0.0f, 0.0f, halfExtent },   // 右：  x <=  half
        {  0.0f, 1.0f, 0.0f, halfExtent },   // 下：  y >= -half
        {  0.0f,-1.0f, 0.0f, halfExtent },   // 上：  y <=  half
        {  0.0f, 0.0f, 1.0f, halfExtent },   // 近：  z >= -half
        {  0.0f, 0.0f,-1.0f, halfExtent },   // 远：  z <=  half
    };
    std::memcpy(frustum.planes, planes, sizeof(planes));
    return frustum;
}

/// 一条"只带平移 + indexCount"的合成实例（与 `NaniteCull::BuildTestInstances` 同形：
/// 平移写在列主序 `localToWorld` 的第 4 列 = 元素 12/13/14）
NaniteInstanceGpuObject MakeTranslatedInstance(float x, float y, float z, u32 indexCount) {
    NaniteInstanceGpuObject instance{};
    instance.localToWorld[0]  = 1.0f;
    instance.localToWorld[5]  = 1.0f;
    instance.localToWorld[10] = 1.0f;
    instance.localToWorld[15] = 1.0f;
    instance.localToWorld[12] = x;
    instance.localToWorld[13] = y;
    instance.localToWorld[14] = z;
    instance.indexCount = indexCount;
    return instance;
}

/// 球 a 是否包含球 b（容差用相对量，避免把浮点末位差异当成结构错误）
bool SphereContains(const float outerCenter[3], float outerRadius,
                    const float innerCenter[3], float innerRadius) {
    const float dx = innerCenter[0] - outerCenter[0];
    const float dy = innerCenter[1] - outerCenter[1];
    const float dz = innerCenter[2] - outerCenter[2];
    const float reach = std::sqrt(dx * dx + dy * dy + dz * dz) + innerRadius;
    const float tolerance = 1.0e-4f * (1.0f + outerRadius);
    return reach <= outerRadius + tolerance;
}

/// 逐条核对 BVH 的结构不变量（父球/叶子球包含关系、下标合法、叶子簇表是排列）
void CheckBVHInvariants(const NaniteClusterBVH& bvh) {
    REQUIRE_FALSE(bvh.Empty());
    CHECK(bvh.clusterCount == (u32)bvh.clusterSpheres.size());
    CHECK(bvh.nodes.size() == (usize)bvh.leafCount * 2u - 1u);   // 满二叉树
    CHECK(bvh.depth >= 1u);
    CHECK(bvh.depth <= kNaniteBVHMaxDepth);
    CHECK(bvh.maxStackDepthUpperBound == bvh.depth);
    CHECK(bvh.leafClusterIndices.size() == bvh.clusterCount);

    // 叶子簇表必须是 [0, clusterCount) 的一个**排列**：每个簇恰好出现一次
    std::vector<u32> seen(bvh.clusterCount, 0u);
    for (u32 cluster : bvh.leafClusterIndices) {
        REQUIRE(cluster < bvh.clusterCount);
        seen[cluster] += 1u;
    }
    u32 duplicated = 0u;
    u32 missing = 0u;
    for (u32 i = 0u; i < bvh.clusterCount; ++i) {
        if (seen[i] == 0u) ++missing;
        if (seen[i] > 1u)  ++duplicated;
    }
    CHECK(missing == 0u);
    CHECK(duplicated == 0u);

    u32 leafSeen = 0u;
    for (const NaniteBVHNode& node : bvh.nodes) {
        CHECK(node.radius >= 0.0f);
        if (NaniteBVHNodeIsLeaf(node)) {
            ++leafSeen;
            CHECK(node.right == kNaniteBVHNoChild);
            CHECK(node.count >= 1u);
            REQUIRE((usize)node.left + node.count <= bvh.leafClusterIndices.size());
            // 叶子球必须包含它的每一个簇球（"叶子球不可见 ⇒ 这些簇都不可见"的前提）
            for (u32 k = 0u; k < node.count; ++k) {
                const NaniteClusterSphere& s =
                    bvh.clusterSpheres[bvh.leafClusterIndices[node.left + k]];
                CHECK(SphereContains(node.center, node.radius, s.center, s.radius));
            }
        } else {
            CHECK(node.count == 2u);
            REQUIRE(node.left  < bvh.nodes.size());
            REQUIRE(node.right < bvh.nodes.size());
            CHECK(node.left != node.right);
            // 父球必须包含两个孩子球（同上：早退的正确性前提）
            for (u32 c = 0u; c < 2u; ++c) {
                const NaniteBVHNode& child = bvh.nodes[(c == 0u) ? node.left : node.right];
                CHECK(SphereContains(node.center, node.radius, child.center, child.radius));
            }
        }
    }
    CHECK(leafSeen == bvh.leafCount);
}

/// BVH 读数行（验收要的每个数字；用 MESSAGE 打印到测试输出里）
std::string BVHStatsLine(const char* label, const NaniteClusterBVH& bvh) {
    return std::string(label)
         + " 簇=" + std::to_string(bvh.clusterCount)
         + " 节点=" + std::to_string(bvh.nodes.size())
         + " 叶子=" + std::to_string(bvh.leafCount)
         + " 深度=" + std::to_string(bvh.depth)
         + " 最大叶子簇数=" + std::to_string(bvh.maxLeafClusterCount)
         + " 栈上界=" + std::to_string(bvh.maxStackDepthUpperBound)
         + " 叶子簇表=" + std::to_string(bvh.leafClusterIndices.size());
}

} // namespace

TEST_CASE("NaniteBVH: 节点数/深度上界与叶子容量（三种网格规模）") {
    // 三种规模：72 tri（6×6）、2048 tri（32×32）、3×3 平铺 576 tri（有大量重复内容）
    const GridMesh meshes[3] = {
        MakeGrid(6),
        MakeGrid(32),
        MakeTiledPatches(3u, 3u, 8u, 4u, 20.0f).mesh,
    };
    const char* labels[3] = {
        "6x6 网格（72 tri）:",
        "32x32 网格（2048 tri）:",
        "3x3 平铺（9×64 tri）:",
    };

    for (u32 which = 0u; which < 3u; ++which) {
        const GridMesh&       mesh       = meshes[which];
        const MeshAttributes attributes = MakeSphereAttributes(mesh);

        NanitePackedAsset asset;
        REQUIRE(BuildNaniteAssetFromGeometry(mesh.positions, attributes.normals, attributes.uvs,
                                             mesh.indices, {}, asset));
        REQUIRE(asset.header.clusterCount > 0u);

        NaniteClusterBVH bvh;
        REQUIRE(BuildNaniteClusterBVH(asset.clusters, bvh));
        CheckBVHInvariants(bvh);

        // 叶子容量：这三种规模都远未触发深度上限 ⇒ 每个叶子不超过叶子容量
        CHECK(bvh.depth < kNaniteBVHMaxDepth);
        CHECK(bvh.maxLeafClusterCount <= kNaniteBVHLeafCapacity);
        // 深度的**闭式上界**：每次分裂都把规模压到 ≤ ceil(2n/3)（平衡护栏）⇒
        // depth ≤ 1 + log_{1.5}(簇数 / 叶子容量)。这条断言等于把"深度上限只是安全网"钉住。
        u32 depthBound = 1u;
        for (u32 size = bvh.clusterCount; size > kNaniteBVHLeafCapacity; ) {
            size = (size * 2u + 2u) / 3u;   // ceil(2 × size / 3)
            ++depthBound;
        }
        CHECK(bvh.depth <= depthBound);
        // 节点数上界：满二叉树 ⇒ 节点 = 2 × 叶子 - 1 且 ≤ 2 × 簇数 - 1
        CHECK(bvh.nodes.size() <= (usize)bvh.clusterCount * 2u - 1u);
        // 显式栈容量必须够：DFS 栈占用上界 = 树高 ≤ 32
        CHECK(bvh.maxStackDepthUpperBound <= kNaniteBVHMaxStackDepth);
        // 簇球表与簇记录逐位一致（GPU 与 CPU 读同一份比特的前提）
        for (u32 i = 0u; i < bvh.clusterCount; ++i) {
            CHECK(bvh.clusterSpheres[i].center[0] == asset.clusters[i].boundsCenterRadius[0]);
            CHECK(bvh.clusterSpheres[i].center[1] == asset.clusters[i].boundsCenterRadius[1]);
            CHECK(bvh.clusterSpheres[i].center[2] == asset.clusters[i].boundsCenterRadius[2]);
            CHECK(bvh.clusterSpheres[i].radius    ==
                  ((asset.clusters[i].boundsCenterRadius[3] > 0.0f)
                       ? asset.clusters[i].boundsCenterRadius[3] : 0.0f));
        }

        MESSAGE(BVHStatsLine(labels[which], bvh).c_str());
    }
}

TEST_CASE("NaniteBVH: 空表与单簇边界") {
    // ── ① 空表：合法输入 ⇒ 返回 true 且产物为空（节点/叶子/深度全 0）──
    {
        const std::vector<NaniteClusterRecord> none;
        NaniteClusterBVH bvh;
        REQUIRE(BuildNaniteClusterBVH(none, bvh));
        CHECK(bvh.Empty());
        CHECK(bvh.nodes.empty());
        CHECK(bvh.leafClusterIndices.empty());
        CHECK(bvh.clusterSpheres.empty());
        CHECK(bvh.clusterCount == 0u);
        CHECK(bvh.leafCount == 0u);
        CHECK(bvh.depth == 0u);
        CHECK(bvh.maxStackDepthUpperBound == 0u);

        // 空 BVH 的视图：节点表为空 ⇒ CPU 参考遍历给出全 0（不崩）
        const NaniteClusterBVHView view = bvh.View();
        CHECK(view.nodes == nullptr);
        CHECK(view.nodeCount == 0u);
        const NaniteInstanceGpuObject instance = MakeTranslatedInstance(0.0f, 0.0f, 0.0f, 36u);
        NaniteVisibleClusterRef out[4] = {};
        NaniteClusterBVHTraversalStats stats{};
        CHECK(NaniteTraverseClusterBVHCPU(MakeBoxFrustum(100.0f), view, &instance, 1u, 4u,
                                          out, 4u, &stats) == 0u);
        CHECK(stats.visitedNodes == 0u);
        CHECK(stats.visibleClusters == 0u);
    }

    // ── ② 单簇：根既是叶子也是全部（深度 1、节点 1、叶子簇表 1 条）──
    {
        NaniteClusterRecord record{};
        record.boundsCenterRadius[0] = 1.0f;
        record.boundsCenterRadius[1] = 2.0f;
        record.boundsCenterRadius[2] = 3.0f;
        record.boundsCenterRadius[3] = 0.5f;
        const std::vector<NaniteClusterRecord> one = { record };

        NaniteClusterBVH bvh;
        REQUIRE(BuildNaniteClusterBVH(one, bvh));
        CHECK(bvh.nodes.size() == 1u);
        CHECK(bvh.leafCount == 1u);
        CHECK(bvh.depth == 1u);
        CHECK(bvh.maxLeafClusterCount == 1u);
        CHECK(bvh.leafClusterIndices.size() == 1u);
        CHECK(bvh.leafClusterIndices[0] == 0u);
        CHECK(NaniteBVHNodeIsLeaf(bvh.nodes[0]));
        CHECK(bvh.nodes[0].count == 1u);
        CHECK(bvh.nodes[0].radius == 0.5f);
        CheckBVHInvariants(bvh);

        // 退化半径（负值 / 0）被夹到 0：与遍历判据（radius < 0 归零）同口径
        std::vector<NaniteClusterRecord> degenerate = one;
        degenerate[0].boundsCenterRadius[3] = -3.0f;
        NaniteClusterBVH negative;
        REQUIRE(BuildNaniteClusterBVH(degenerate, negative));
        CHECK(negative.clusterSpheres[0].radius == 0.0f);
        CheckBVHInvariants(negative);
    }
}

TEST_CASE("NaniteBVH: 同一输入两次构建逐位可复现") {
    const TiledMesh tiled = MakeTiledPatches(3u, 3u, 8u, 4u, 20.0f);
    const MeshAttributes attributes = MakeSphereAttributes(tiled.mesh);

    NanitePackedAsset asset;
    REQUIRE(BuildNaniteAssetFromGeometry(tiled.mesh.positions, attributes.normals, attributes.uvs,
                                         tiled.mesh.indices, {}, asset));

    NaniteClusterBVH first;
    NaniteClusterBVH second;
    REQUIRE(BuildNaniteClusterBVH(asset.clusters, first));
    REQUIRE(BuildNaniteClusterBVH(asset.clusters, second));
    CheckBVHInvariants(first);

    // 逐位比较整份产物（节点表 / 叶子簇表 / 簇球表 / 四个读数）
    CHECK(first.nodes.size() == second.nodes.size());
    CHECK(first.leafClusterIndices == second.leafClusterIndices);
    CHECK(first.clusterCount == second.clusterCount);
    CHECK(first.leafCount == second.leafCount);
    CHECK(first.depth == second.depth);
    CHECK(first.maxLeafClusterCount == second.maxLeafClusterCount);
    CHECK(std::memcmp(first.nodes.data(), second.nodes.data(),
                      sizeof(NaniteBVHNode) * first.nodes.size()) == 0);
    CHECK(std::memcmp(first.clusterSpheres.data(), second.clusterSpheres.data(),
                      sizeof(NaniteClusterSphere) * first.clusterSpheres.size()) == 0);

    MESSAGE(BVHStatsLine("可复现性（3x3 平铺）:", first).c_str());
}

TEST_CASE("NaniteBVH: DFS 遍历的访问数（全部在内/全部在外/部分相交）") {
    // ── 手工簇布局：16 个半径 0.25 的簇沿 x 轴等距排在 [-7.5, 7.5] ──
    // 盒视锥取 [-4, 4]^3、实例为纯平移 ⇒ 三类情形的可见集合与访问数都是**解析可算**的已知值：
    //   · 实例在原点   ⇒ 16 个簇全在盒内：访问数 = 全部节点、可见 16
    //   · 实例在 x=100 ⇒ 全部在外：只有根被测试 ⇒ 访问数 = 1、可见 0
    //   · 实例在 x=6   ⇒ 部分相交：可见的是 x ∈ [-9.75, -2.25] 的 6 个簇（下标 0..5）
    std::vector<NaniteClusterRecord> records(16u);
    for (u32 i = 0u; i < 16u; ++i) {
        records[i].boundsCenterRadius[0] = -7.5f + (float)i;
        records[i].boundsCenterRadius[1] = 0.0f;
        records[i].boundsCenterRadius[2] = 0.0f;
        records[i].boundsCenterRadius[3] = 0.25f;
    }
    NaniteClusterBVH bvh;
    REQUIRE(BuildNaniteClusterBVH(records, bvh));
    CheckBVHInvariants(bvh);

    const NaniteFrustumPlanes frustumSmall = MakeBoxFrustum(4.0f);   // 部分相交用
    const NaniteFrustumPlanes frustumAll   = MakeBoxFrustum(8.5f);   // 全部在内用（16 个簇的跨度是 ±7.75）
    const NaniteClusterBVHView view = bvh.View();
    const u32 totalNodes = (u32)bvh.nodes.size();

    // ── ① 全部在内：整棵树都被访问（根可见 ⇒ 每个节点都会被测试到）──
    {
        const NaniteInstanceGpuObject instance = MakeTranslatedInstance(0.0f, 0.0f, 0.0f, 36u);
        std::vector<NaniteVisibleClusterRef> visible(16u);
        NaniteClusterBVHTraversalStats stats{};
        const u32 written = NaniteTraverseClusterBVHCPU(frustumAll, view, &instance, 1u, 8u,
                                                        visible.data(), (u32)visible.size(), &stats);
        CHECK(written == 16u);
        CHECK(stats.visibleClusters == 16u);
        CHECK(stats.visitedNodes == totalNodes);      // 全在内 ⇒ 无早退
        CHECK(stats.traversedInstances == 1u);
        CHECK(stats.stackOverflows == 0u);
        MESSAGE("DFS 全部在内：节点 " << totalNodes << "，访问 " << stats.visitedNodes
                << "，可见簇 " << stats.visibleClusters);
    }

    // ── ② 全部在外：只有根被测试（根球不可见 ⇒ 整棵子树跳过）──
    {
        const NaniteInstanceGpuObject instance = MakeTranslatedInstance(100.0f, 0.0f, 0.0f, 36u);
        std::vector<NaniteVisibleClusterRef> visible(16u);
        NaniteClusterBVHTraversalStats stats{};
        const u32 written = NaniteTraverseClusterBVHCPU(frustumSmall, view, &instance, 1u, 8u,
                                                        visible.data(), (u32)visible.size(), &stats);
        CHECK(written == 0u);
        CHECK(stats.visibleClusters == 0u);
        CHECK(stats.visitedNodes == 1u);              // 根：访问一次、判不可见、不再下降
        CHECK(stats.traversedInstances == 1u);
        CHECK(stats.stackOverflows == 0u);
        MESSAGE("DFS 全部在外：节点 " << totalNodes << "，访问 " << stats.visitedNodes
                << "，可见簇 " << stats.visibleClusters);
    }

    // ── ③ 部分相交：可见 = 下标 0..5（解析值），访问数在 1 与全树之间 ──
    {
        const NaniteInstanceGpuObject instance = MakeTranslatedInstance(6.0f, 0.0f, 0.0f, 36u);
        std::vector<NaniteVisibleClusterRef> visible(16u);
        NaniteClusterBVHTraversalStats stats{};
        const u32 written = NaniteTraverseClusterBVHCPU(frustumSmall, view, &instance, 1u, 8u,
                                                        visible.data(), (u32)visible.size(), &stats);
        CHECK(written == 6u);
        CHECK(stats.visibleClusters == 6u);
        CHECK(stats.visitedNodes > 1u);
        CHECK(stats.visitedNodes < totalNodes);       // 有一部分子树被剪掉
        CHECK(stats.traversedInstances == 1u);
        std::vector<u32> clusters;
        for (u32 i = 0u; i < written; ++i) {
            CHECK(visible[i].instance == 0u);
            clusters.push_back(visible[i].cluster);
        }
        std::sort(clusters.begin(), clusters.end());
        for (u32 i = 0u; i < 6u; ++i) CHECK(clusters[i] == i);
        MESSAGE("DFS 部分相交：节点 " << totalNodes << "，访问 " << stats.visitedNodes
                << "，可见簇 " << stats.visibleClusters << "（下标 0..5）");
    }

    // ── ④ 多实例：内 + 外 = 访问数 = 全树 + 1、可见 = 16；空实例不参与遍历 ──
    {
        const std::vector<NaniteInstanceGpuObject> instances = {
            MakeTranslatedInstance(0.0f, 0.0f, 0.0f, 36u),     // 全在内
            MakeTranslatedInstance(100.0f, 0.0f, 0.0f, 36u),   // 全在外
            MakeTranslatedInstance(0.0f, 0.0f, 0.0f, 0u),      // 空实例（indexCount = 0）
        };
        std::vector<NaniteVisibleClusterRef> visible(64u);
        NaniteClusterBVHTraversalStats stats{};
        const u32 written = NaniteTraverseClusterBVHCPU(frustumAll, view, instances.data(), 3u, 8u,
                                                        visible.data(), (u32)visible.size(), &stats);
        CHECK(written == 16u);
        CHECK(stats.visibleClusters == 16u);
        CHECK(stats.visitedNodes == totalNodes + 1u);
        CHECK(stats.traversedInstances == 2u);            // 空实例被跳过
        CHECK(stats.stackOverflows == 0u);
        // 可见簇必须全部属于实例 0（实例 1 一个都看不见）
        for (u32 i = 0u; i < written; ++i) CHECK(visible[i].instance == 0u);
    }

    // ── ⑤ 实例域钳制：maxInstances = 1 ⇒ 只遍历第 0 个实例（CPU 与 GPU 同一口径）──
    {
        const std::vector<NaniteInstanceGpuObject> instances = {
            MakeTranslatedInstance(0.0f, 0.0f, 0.0f, 36u),
            MakeTranslatedInstance(0.0f, 0.0f, 0.0f, 36u),
        };
        std::vector<NaniteVisibleClusterRef> visible(64u);
        NaniteClusterBVHTraversalStats stats{};
        const u32 written = NaniteTraverseClusterBVHCPU(frustumAll, view, instances.data(), 2u, 1u,
                                                        visible.data(), (u32)visible.size(), &stats);
        CHECK(written == 16u);                            // 只算第 1 个实例
        CHECK(stats.traversedInstances == 1u);
        CHECK(stats.visitedNodes == totalNodes);
        for (u32 i = 0u; i < written; ++i) CHECK(visible[i].instance == 0u);
    }

    // ── ⑥ 容量截断：计数不受容量影响，只截断写入（与 GPU 的槽位口径一致）──
    {
        const NaniteInstanceGpuObject instance = MakeTranslatedInstance(0.0f, 0.0f, 0.0f, 36u);
        NaniteVisibleClusterRef out[4] = {};
        NaniteClusterBVHTraversalStats stats{};
        const u32 written = NaniteTraverseClusterBVHCPU(frustumAll, view, &instance, 1u, 8u,
                                                        out, 4u, &stats);
        CHECK(written == 16u);                  // 计数不截断
        CHECK(stats.visibleClusters == 16u);
        CHECK(out[0].instance == 0u);           // 容量内的前 4 条确实写了
        CHECK(out[3].cluster != 0xFFFFFFFFu);

        // 容量 0 / 空输出指针：只计数、不写（不崩）
        CHECK(NaniteTraverseClusterBVHCPU(frustumAll, view, &instance, 1u, 8u,
                                          nullptr, 0u, &stats) == 16u);
        CHECK(NaniteTraverseClusterBVHCPU(frustumAll, view, &instance, 1u, 8u,
                                          out, 0u, nullptr) == 16u);
    }
}

// ============================================================
// 23. §14.8 任务 15：每簇 LOD 元数据的构建（own/parent 误差 + LOD 级 + 根标志）
//
// 【验收对应】任务 15 的 Phase 3（DAG 割）需要三个判据量：`ownError`（本簇替代其孩子的误差）、
//   `parentError`（父簇替代本簇的误差）、"是不是根"。它们全部由本构建器从 `.nanite` 簇记录 +
//   LOD 段推出 ⇒ 这里逐条钉住：级号来自 LOD 段、own 来自孩子的 `maxParentLODError`、
//   parent 来自本簇的 `maxParentLODError`、根由"没有任何簇以它为子"判定（**不能**只看数值 0）、
//   确定性（两次逐位一致）、非法输入（越界孩子 / 非单调 LOD 段）返回 false 且不改写出参。
// ============================================================
TEST_CASE("NaniteLOD: 每簇 LOD 元数据的构建（级/误差/根/确定性/非法输入）") {
    // 三条出现记录：簇 0、1 属级 0（细），簇 2 属级 1（根，是 0/1 的父）
    //   · 级 0 → 级 1 那次简化的绝对误差 = 12.5（记在级 0 的 maxParentLODError 上）
    //   · 级 1 是根 ⇒ maxParentLODError = 0
    std::vector<NaniteClusterRecord> clusters(3);
    for (NaniteClusterRecord& record : clusters) record = NaniteClusterRecord{};
    clusters[0].boundsCenterRadius[3] = 1.0f;
    clusters[1].boundsCenterRadius[3] = 1.0f;
    clusters[2].boundsCenterRadius[3] = 2.0f;
    clusters[0].maxParentLODError = 12.5f;   // 级 0 ⇒ 切到父级（级 1）的误差
    clusters[1].maxParentLODError = 12.5f;
    clusters[2].maxParentLODError = 0.0f;    // 根
    clusters[2].childClusterOffset = 0u;     // 根的孩子的扁平区间 [0, 2)
    clusters[2].childCount         = 2u;
    const u32 lodOffsets[2] = { 0u, 2u };    // 级 0 = [0,2)、级 1 = [2,3)

    std::vector<NaniteClusterLODInfo> info;
    REQUIRE(BuildNaniteClusterLODInfo(clusters, lodOffsets, info));
    REQUIRE(info.size() == 3u);

    CHECK(info[0].lodLevel == 0u);
    CHECK(info[1].lodLevel == 0u);
    CHECK(info[2].lodLevel == 1u);
    // ownError：叶子（级 0）没有孩子 ⇒ 0；根（级 1）用本簇替代孩子 ⇒ 孩子的 maxParentLODError
    CHECK(info[0].ownError == 0.0f);
    CHECK(info[1].ownError == 0.0f);
    CHECK(info[2].ownError == 12.5f);
    // parentError：级 0 是 12.5（切到父级），根是 0
    CHECK(info[0].parentError == 12.5f);
    CHECK(info[2].parentError == 0.0f);
    // 根标志只能靠"谁是谁的孩子"判定
    CHECK((info[0].flags & kNaniteLODInfoFlagRoot) == 0u);
    CHECK((info[1].flags & kNaniteLODInfoFlagRoot) == 0u);
    CHECK((info[2].flags & kNaniteLODInfoFlagRoot) != 0u);

    // ── 确定性：两次构建逐位一致 ──
    std::vector<NaniteClusterLODInfo> again;
    REQUIRE(BuildNaniteClusterLODInfo(clusters, lodOffsets, again));
    REQUIRE(again.size() == info.size());
    CHECK(std::memcmp(info.data(), again.data(),
                      info.size() * sizeof(NaniteClusterLODInfo)) == 0);

    // ── 空输入是合法输入（长度 0）──
    {
        std::vector<NaniteClusterLODInfo> empty;
        CHECK(BuildNaniteClusterLODInfo({}, {}, empty));
        CHECK(empty.empty());
        // 簇表非空但 LOD 段为空 ⇒ 全部记为 0 级（退化为"只有一级"），仍成功
        std::vector<NaniteClusterLODInfo> noLods;
        CHECK(BuildNaniteClusterLODInfo(clusters, {}, noLods));
        REQUIRE(noLods.size() == 3u);
        for (const NaniteClusterLODInfo& entry : noLods) CHECK(entry.lodLevel == 0u);
    }

    // ── 非法输入：孩子区间越界 ⇒ 失败且不改写出参 ──
    {
        std::vector<NaniteClusterRecord> bad = clusters;
        bad[2].childClusterOffset = 2u;
        bad[2].childCount         = 5u;   // 2 + 5 > 3
        std::vector<NaniteClusterLODInfo> untouched(7u);
        CHECK_FALSE(BuildNaniteClusterLODInfo(bad, lodOffsets, untouched));
        CHECK(untouched.size() == 7u);    // 出参未被改动
    }
    // ── 非法输入：LOD 段非单调 / 越界 ⇒ 失败 ──
    {
        const u32 notMonotonic[3] = { 0u, 2u, 1u };
        std::vector<NaniteClusterLODInfo> untouched;
        CHECK_FALSE(BuildNaniteClusterLODInfo(clusters, notMonotonic, untouched));
        const u32 outOfRange[2] = { 0u, 99u };
        std::vector<NaniteClusterLODInfo> untouched2;
        CHECK_FALSE(BuildNaniteClusterLODInfo(clusters, outOfRange, untouched2));
    }
}


// ============================================================
// §14.8 任务 25 回归：**去重命中 + 多源网格**下的簇材质归属
//
// 【为什么必须有这一条】簇材质映射的缺陷**只在真实资产路径上显形** —— 拿**去重后**的
//   `triangleOffset` 当合并空间三角形下标去查 `meshes[]`，是构建器曾犯过的错。本用例走
//   `BuildNaniteAssetFromGeometry` 的**带材质重载**（真实路径），并刻意构造出缺陷的两个前提：
//     ① **多个源网格**（否则"选错网格"观察不到）；
//     ② **平移副本 ⇒ DAG 去重真的命中**（否则去重空间的偏移恰好等于真值，缺陷不显形）。
//   实测 Sponza 上"按 `triangleOffset` 投票"的失败形态是"非 LOD0 的簇 `triangleOffset` 越出
//   原始三角形表" + "LOD0 有 3957/4099 选错网格"；本用例用最小构造复现后者的**同型**失败：
//   平移副本的簇共享同一份内容 ⇒ 它们的 `triangleOffset` 全都指向**第一份副本**的三角形区间
//   ⇒ 按那个字段投票会把第 2/3 份副本的簇判成第 1 份的材质。
//
// 【判据是"手工期望"，不是实现的自证】三个源网格在空间上互不相连（间距 100，
//   自身范围只有 x∈[0,16]）⇒ **每个三角形的归属由它的顶点 x 唯一确定**（副本之间不共享顶点，
//   简化也不可能跨副本折叠）⇒ 期望值可以逐三角形手算，再按"多数票、平票取小下标"汇总。
//   这一条与实现用的"顶点归属表 + 逐三角形多数票"是**两条独立路径**（这里只看顶点位置）。
//
// 【一个必须写明的实测性质】`meshopt_buildMeshlets` 在当前簇没有未发射的**连通**邻居时，
//   会退化成"按空间最近挑一个三角形、**不管连通性**"（`meshoptimizer/src/clusterizer.cpp`：
//   "we currently just pick the closest triangle irrespective of connectivity"）⇒ 一个簇
//   **合法地**可以同时含两份远距离、互不相连的副本的三角形（本用例实测 23 簇里有 5 簇如此）。
//   所以"多网格簇"不是错误，`multiMeshClusters` 只是如实计数。
// ============================================================
TEST_CASE("NaniteMaterialMap: 平移副本（去重命中）仍归属各自的源网格，unmapped==0") {
    const GridMesh patch = MakeGridRect(16u, 8u);                 // 128 四边形 = 256 三角形
    const u32 patchVertexCount  = (u32)(patch.positions.size() / 3u);
    const u32 patchTriangleCount = (u32)(patch.indices.size() / 3u);
    constexpr u32 kCopies = 3u;
    constexpr float kSpacing = 100.0f;   // 远大于 patch 自身范围（x∈[0,16]）⇒ 副本互不相连

    std::vector<float> positions, normals, uvs;
    std::vector<u32>   indices;
    std::vector<NaniteSourceMeshRange> meshes;
    std::vector<NaniteMaterialRecord>  materials;
    for (u32 copy = 0u; copy < kCopies; ++copy) {
        const float offsetX = (float)copy * kSpacing;
        for (usize v = 0u; v + 2u < patch.positions.size(); v += 3u) {
            positions.push_back(patch.positions[v + 0u] + offsetX);
            positions.push_back(patch.positions[v + 1u]);
            positions.push_back(patch.positions[v + 2u]);
            normals.push_back(0.0f);
            normals.push_back(0.0f);
            normals.push_back(1.0f);
            // UV 是**局部**量（同一份 patch 上逐顶点相同）⇒ 平移副本的簇内容才逐位相同（去重会命中）
            uvs.push_back(patch.positions[v + 0u] / 16.0f);
            uvs.push_back(patch.positions[v + 1u] / 8.0f);
        }
        const u32 baseVertex = copy * patchVertexCount;
        for (const u32 index : patch.indices) indices.push_back(baseVertex + index);

        NaniteSourceMeshRange range;
        range.firstTriangle = (u32)meshes.size() * patchTriangleCount;
        range.triangleCount = patchTriangleCount;
        range.materialIndex = copy;
        meshes.push_back(range);
        materials.push_back(NaniteMakeTestMaterial(copy * 3u + 1u, 0u));
    }

    // ── ① 真实路径：带 `meshes` 的重载（缺陷就在这里）──
    NanitePackedAsset asset;
    REQUIRE(BuildNaniteAssetFromGeometry(positions, normals, uvs, indices, materials, meshes, asset));
    REQUIRE(asset.clusters.size() > 0u);

    // 去重真的命中（否则本用例没有覆盖缺陷路径；这是**前提**，不是结论）
    NaniteClusterDAG dag;
    REQUIRE(BuildNaniteClusterDAG(positions, normals, uvs, indices, dag));
    REQUIRE(dag.clusters.size() == asset.clusters.size());
    CHECK(dag.stats.dedupRate > 0.0f);

    // ── ② 契约：正常必须 0 ──
    CHECK(asset.stats.unmappedClusters == 0u);
    // 多级 LOD 链确实产生（否则"非 LOD0 也映射对"这条没被覆盖）
    CHECK(dag.stats.levelCount > 1u);

    // ── ③ 手工期望：**逐三角形**判它属于哪一份副本（按顶点 x 所在的带），再取多数票 ──
    std::vector<u32> perCopy(kCopies, 0u);
    u32 wrongMaterial = 0u;
    u32 straddlingClusters = 0u;
    std::string wrongDetail;
    for (usize ci = 0u; ci < asset.clusters.size(); ++ci) {
        const NaniteClusterRecord& cluster = asset.clusters[ci];
        const u32 unique = dag.clusterUnique[ci];
        const u32 triBase = dag.uniqueTriangleOffset[unique];
        const u32 vertexBase = dag.clusterVertexIndexOffset[ci];
        std::vector<u32> votes(kCopies, 0u);
        for (u32 t = 0u; t < cluster.triangleCount; ++t) {
            const NanitePackedTriangle& packed = dag.uniqueTriangles[triBase + t];
            const u32 local[3] = { NaniteTriangleIndex0(packed),
                                   NaniteTriangleIndex1(packed),
                                   NaniteTriangleIndex2(packed) };
            // 用**顶点位置**（而不是实现用的归属表）判副本：x ∈ [0,16] → 0，[100,116] → 1，其余 → 2
            const float x = positions[(usize)dag.clusterVertexIndices[vertexBase + local[0]] * 3u];
            const u32 copy = (x < 0.5f * kSpacing) ? 0u : (x < 1.5f * kSpacing) ? 1u : 2u;
            ++votes[copy];
        }
        u32 expected = 0u;
        for (u32 copy = 1u; copy < kCopies; ++copy) {
            if (votes[copy] > votes[expected]) expected = copy;   // 平票取小下标（与实现同口径）
        }
        u32 contributing = 0u;
        for (u32 copy = 0u; copy < kCopies; ++copy) if (votes[copy] > 0u) ++contributing;
        if (contributing > 1u) ++straddlingClusters;   // 如实计数，不是错误

        CHECK(cluster.materialID < (u32)materials.size());
        if (cluster.materialID != expected) {
            ++wrongMaterial;
            if (wrongDetail.size() < 400u) {
                wrongDetail += " [#" + std::to_string(ci) + " level=" +
                               std::to_string(dag.clusterLevel[ci]) + " tri=" +
                               std::to_string(cluster.triangleCount) + " votes=[" +
                               std::to_string(votes[0]) + "," + std::to_string(votes[1]) + "," +
                               std::to_string(votes[2]) + "] got=" +
                               std::to_string(cluster.materialID) + " want=" +
                               std::to_string(expected) + "]";
            }
        }
        if (cluster.materialID < kCopies) ++perCopy[cluster.materialID];
    }
    CHECK(wrongMaterial == 0u);
    // 每一份副本都必须**真的有簇**归属到它的材质（否则"全部判成第 1 份"也会让上面那条通过）
    for (u32 copy = 0u; copy < kCopies; ++copy) CHECK(perCopy[copy] > 0u);

    MESSAGE("任务 25 回归：簇数=" << asset.clusters.size()
            << " levels=" << dag.stats.levelCount
            << " 去重率=" << dag.stats.dedupRate
            << " ⇒ 新口径 unmapped=" << asset.stats.unmappedClusters
            << " 选错材质=" << wrongMaterial
            << " 跨副本簇=" << straddlingClusters
            << " 每份副本簇数=[" << perCopy[0] << "," << perCopy[1] << "," << perCopy[2] << "]"
            << wrongDetail.c_str());
}

