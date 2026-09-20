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
// ============================================================

#include "doctest.h"

#include "Nanite/NaniteUpload.h"   // BuildNaniteClusters（任务 8）

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
