// ============================================================
//  Tests/TestNaniteGltfReader.cpp
//
//  T3：最小 glTF 几何读取器的行为契约。
//
//  【为什么跳过缺失资产】`Content/gltf/` 不入库（`.gitignore`），CI 上没有源资产；
//  文件不存在时用例直接跳过，而不是失败 —— 这与 `ProcessMeshes` "源缺失空跑"的口径一致。
// ============================================================

#include "doctest.h"

#include "Nanite/NaniteGltfReader.h"

#include <chrono>
#include <filesystem>
#include <string>

using namespace he;
using namespace he::render;

namespace {

std::string CubePath() { return std::string(HUGE_CONTENT_DIR) + "gltf/cube/cube.gltf"; }

bool HasCube() {
    std::error_code ec;
    return std::filesystem::exists(CubePath(), ec);
}

} // namespace

TEST_CASE("NaniteGltfReader: 读 cube.gltf（primitive/三角形/材质与合并几何口径）") {
    if (!HasCube()) {
        MESSAGE("跳过：Content/gltf/cube/cube.gltf 不存在（源资产不入库）");
        return;
    }

    NaniteGltfScene scene;
    std::string error;
    REQUIRE(LoadNaniteGltfFile(CubePath().c_str(), scene, &error));

    CHECK(scene.primitives.size() == 1u);
    const NaniteGltfPrimitive& primitive = scene.primitives.front();
    CHECK(primitive.positions.size() % 3u == 0u);
    CHECK(primitive.TriangleCount() == 12u);          // 立方体 12 个三角形
    CHECK(primitive.indices.size() == 36u);
    CHECK(primitive.hasNormals);

    // 源哈希：确定性与"源不变则哈希不变"
    CHECK(scene.sourceHash == ComputeNaniteGltfSourceHash(scene));

    // 合并几何：索引已加 baseVertex、区间首尾相接且覆盖全部三角形
    NaniteGltfMergedGeometry merged;
    MergeNaniteGltfScene(scene, merged);
    CHECK(merged.indices.size() / 3u == scene.TotalTriangleCount());
    CHECK(merged.meshes.size() == scene.primitives.size());

    u32 expectedFirst = 0u;
    bool contiguous = true;
    for (const NaniteSourceMeshRange& range : merged.meshes) {
        if (range.firstTriangle != expectedFirst) contiguous = false;
        expectedFirst += range.triangleCount;
        CHECK(range.materialIndex == &range - merged.meshes.data());
    }
    CHECK(contiguous);
    CHECK(expectedFirst == merged.indices.size() / 3u);

    // 所有索引都必须落在合并后的顶点范围内（baseVertex 偏移正确）
    const u32 vertexCount = (u32)(merged.positions.size() / 3u);
    for (u32 index : merged.indices) CHECK(index < vertexCount);
}

TEST_CASE("NaniteGltfReader: 文件不存在时返回失败并给出原因") {
    NaniteGltfScene scene;
    std::string error;
    CHECK_FALSE(LoadNaniteGltfFile("no/such/file.gltf", scene, &error));
    CHECK_FALSE(error.empty());
}

TEST_CASE("NaniteFallback: cube 的低模简化与序列化自洽") {
    if (!HasCube()) {
        MESSAGE("跳过：Content/gltf/cube/cube.gltf 不存在（源资产不入库）");
        return;
    }
    NaniteGltfScene scene;
    REQUIRE(LoadNaniteGltfFile(CubePath().c_str(), scene));
    const NaniteGltfPrimitive& primitive = scene.primitives.front();

    NaniteFallbackMeshData fallback;
    REQUIRE(BuildNaniteFallbackMesh(primitive.positions, primitive.normals, primitive.indices, 0.5f, fallback));
    CHECK(fallback.header.vertexCount > 0u);
    CHECK(fallback.header.indexCount >= 3u);
    CHECK((fallback.header.indexCount % 3u) == 0u);
    CHECK(fallback.positions.size() == (usize)fallback.header.vertexCount * 3u);
    CHECK(fallback.normals.size()   == (usize)fallback.header.vertexCount * 3u);
    CHECK(fallback.indices.size()   == (usize)fallback.header.indexCount);
    for (u32 index : fallback.indices) CHECK(index < fallback.header.vertexCount);

    std::vector<u8> payload;
    SerializeNaniteFallbackMesh(fallback, payload);
    CHECK(payload.size() >= sizeof(NaniteFallbackMeshHeader));
    NaniteFallbackMeshHeader header{};
    std::memcpy(&header, payload.data(), sizeof(header));
    CHECK(header.vertexCount == fallback.header.vertexCount);
    CHECK(header.indexCount  == fallback.header.indexCount);
}

TEST_CASE("NaniteFallback: Sponza 的大 primitive 都能生成低模（索引/顶点自洽）") {
    const std::string path = std::string(HUGE_CONTENT_DIR) + "gltf/Sponza/glTF/Sponza.gltf";
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        MESSAGE("跳过：Sponza 不存在（源资产不入库）");
        return;
    }
    NaniteGltfScene scene;
    REQUIRE(LoadNaniteGltfFile(path.c_str(), scene));

    u32 checked = 0u;
    for (const NaniteGltfPrimitive& primitive : scene.primitives) {
        if (primitive.TriangleCount() < 2000u) continue;   // 只看大网格（小网格有 cube 用例覆盖）
        NaniteFallbackMeshData fallback;
        REQUIRE(BuildNaniteFallbackMesh(primitive.positions, primitive.normals,
                                        primitive.indices, 0.02f, fallback));
        CHECK(fallback.header.vertexCount > 0u);
        CHECK(fallback.header.indexCount >= 3u);
        CHECK(fallback.header.indexCount < primitive.indices.size());   // 确实简化了
        CHECK(fallback.positions.size() == (usize)fallback.header.vertexCount * 3u);
        CHECK(fallback.indices.size()   == (usize)fallback.header.indexCount);
        for (u32 index : fallback.indices) CHECK(index < fallback.header.vertexCount);
        ++checked;
    }
    CHECK(checked > 0u);
}
