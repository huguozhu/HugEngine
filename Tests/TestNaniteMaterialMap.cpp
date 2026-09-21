// ============================================================
// Tests/TestNaniteMaterialMap.cpp — 簇 → 源网格 → 材质映射的**回归测试**（§14.8 任务 19 / 25）
//
// 【本文件钉住的缺陷（§14.33 ⑤、任务 25 修复）】
//   任务 19 的材质映射把 `NaniteClusterRecord::triangleOffset` 当成"**原始合并空间**的三角形下标"
//   去查 `meshes[]` 的区间表。但那个字段真正的语义是"任务 9 **去重后**共享三角形段的下标"
//   （`NaniteClusterDAG::uniqueTriangleOffset`）。两个坐标系不一致：
//     · `meshopt_buildMeshlets` 会按簇内局部性重排三角形 ⇒ 连 LOD0 的共享段顺序都与原始顺序不同；
//     · 级 ≥1 的簇来自 `meshopt_simplify` 的新三角形，共享段按级序追加 ⇒ 偏移整体落在原始表**之后**。
//   ⇒ 真实 Sponza 资产上的实测（本文件复现的线上读数）：
//     `clusters=8287 unmapped=4103 multi_mesh=97`，且 4103 个簇的 `triangleOffset` 全部 ≥ 262267
//     （= 原始三角形数），正好一一对应 —— 这 4103 个**全部是级 ≥1**。
//   更严重的一层（§14.33 当时未记录）：LOD0 的 4099 个簇虽然"落得进区间"，但 **3957 个选出错的
//   源网格**（静默错材质）—— 那是近处可见的一级，属可见的正确性缺陷，不只是计数缺陷。
//
// 【修复后的口径（本文件的判据）】按"**三角形顶点的源网格归属**"投票（
//   `NaniteAssignClusterMaterialsByVertexOwner`）：归属表由"原始合并索引 + `meshes[]` 区间"推出，
//   与 `meshes[]` 天然同一空间，且对**所有 LOD 级**都成立。
//
// 【为什么用真实资产而不是合成网格】这个缺陷的三个关键性质（多 LOD 级、去重命中、103 个源网格）
//   只有在真实资产上才同时出现；合成网格容易"恰好不触发"。`Tests/TestNaniteBuilder.cpp` 另有一条
//   合成网格的用例覆盖同一口径的**小尺度**行为（平移副本 ⇒ 去重命中 ⇒ 必须仍归属各自的源网格），
//   两条互补：合成用例快而稳，本用例真而全。
//
// 【为什么不链接 `MeshBatcher`】它要 `Map()` GPU 缓冲（单测没有 RHI 设备）。这里按
//   `glTFLoader.cpp:383-425` 的同一 DFS 顺序、同一 `LoadPrimitive` 口径自己合并几何，
//   得到与 `MeshBatcher` 相同的"原始合并三角形空间 + 逐网格连续区间表"。
//   cgltf 的实现体已由 `HugEngineAsset`（`glTFLoader.cpp` 展开 `CGLTF_IMPLEMENTATION`）提供。
// ============================================================

#include "doctest.h"

#include "Nanite/NaniteUpload.h"

#include "cgltf.h"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

using namespace he;

namespace {

using he::render::NaniteSourceMeshRange;

struct MergedGeometry {
    std::vector<float> positions;
    std::vector<float> normals;
    std::vector<float> uvs;
    std::vector<u32>   indices;
    std::vector<NaniteSourceMeshRange> meshes;
};

void AppendPrimitive(const cgltf_primitive& prim, MergedGeometry& out) {
    if (prim.type != cgltf_primitive_type_triangles) return;
    const cgltf_attribute* posAttr = nullptr;
    const cgltf_attribute* normalAttr = nullptr;
    const cgltf_attribute* uvAttr = nullptr;
    for (cgltf_size a = 0; a < prim.attributes_count; ++a) {
        const cgltf_attribute& attr = prim.attributes[a];
        if (attr.type == cgltf_attribute_type_position) posAttr = &attr;
        else if (attr.type == cgltf_attribute_type_normal) normalAttr = &attr;
        else if (attr.type == cgltf_attribute_type_texcoord) uvAttr = &attr;
    }
    if (!posAttr || !posAttr->data) return;

    const cgltf_size vertexCount = posAttr->data->count;
    const cgltf_size posComps = cgltf_num_components(posAttr->data->type);
    std::vector<float> posFloats(posComps * vertexCount);
    cgltf_accessor_unpack_floats(posAttr->data, posFloats.data(), posComps * vertexCount);

    std::vector<float> normalFloats;
    if (normalAttr && normalAttr->data) {
        const cgltf_size n = cgltf_num_components(normalAttr->data->type);
        normalFloats.resize(n * vertexCount);
        cgltf_accessor_unpack_floats(normalAttr->data, normalFloats.data(), n * vertexCount);
    }
    std::vector<float> uvFloats;
    bool hasUV = false;
    if (uvAttr && uvAttr->data) {
        const cgltf_size n = cgltf_num_components(uvAttr->data->type);
        uvFloats.resize(n * vertexCount);
        hasUV = cgltf_accessor_unpack_floats(uvAttr->data, uvFloats.data(), n * vertexCount) > 0;
    }

    const u32 baseVertex = (u32)(out.positions.size() / 3u);
    for (cgltf_size i = 0; i < vertexCount; ++i) {
        out.positions.push_back(posFloats[i * posComps + 0]);
        out.positions.push_back(posFloats[i * posComps + 1]);
        out.positions.push_back(posFloats[i * posComps + 2]);
        if (!normalFloats.empty()) {
            out.normals.push_back(normalFloats[i * 3 + 0]);
            out.normals.push_back(normalFloats[i * 3 + 1]);
            out.normals.push_back(normalFloats[i * 3 + 2]);
        } else {
            out.normals.push_back(0.0f);
            out.normals.push_back(1.0f);
            out.normals.push_back(0.0f);
        }
        if (hasUV) {
            out.uvs.push_back(uvFloats[i * 2 + 0]);
            out.uvs.push_back(uvFloats[i * 2 + 1]);
        } else {
            out.uvs.push_back(0.0f);
            out.uvs.push_back(0.0f);
        }
    }

    NaniteSourceMeshRange range;
    range.firstTriangle = (u32)(out.indices.size() / 3u);
    range.materialIndex = (u32)out.meshes.size();
    range.triangleCount = 0u;
    if (prim.indices) {
        const cgltf_size indexCount = cgltf_accessor_unpack_indices(prim.indices, nullptr, sizeof(u32), 0);
        if (indexCount > 0) {
            std::vector<u32> indices(indexCount);
            cgltf_accessor_unpack_indices(prim.indices, indices.data(), sizeof(u32), indexCount);
            for (cgltf_size i = 0; i < indexCount; ++i) out.indices.push_back(indices[i] + baseVertex);
            range.triangleCount = (u32)(indexCount / 3u);
        }
    }
    if (range.triangleCount > 0u) out.meshes.push_back(range);
}

void ProcessNode(const cgltf_node* node, MergedGeometry& out) {
    if (!node) return;
    if (node->mesh && !node->skin) {
        for (cgltf_size p = 0; p < node->mesh->primitives_count; ++p) {
            AppendPrimitive(node->mesh->primitives[p], out);
        }
    }
    for (cgltf_size c = 0; c < node->children_count; ++c) ProcessNode(node->children[c], out);
}

bool LoadMerged(const char* path, MergedGeometry& out) {
    cgltf_options options{};
    cgltf_data* data = nullptr;
    if (cgltf_parse_file(&options, path, &data) != cgltf_result_success) return false;
    if (cgltf_load_buffers(&options, data, path) != cgltf_result_success) {
        cgltf_free(data);
        return false;
    }
    const cgltf_scene* scene = data->scene ? data->scene
                                          : (data->scenes_count > 0 ? &data->scenes[0] : nullptr);
    if (scene) {
        for (cgltf_size i = 0; i < scene->nodes_count; ++i) ProcessNode(scene->nodes[i], out);
    }
    cgltf_free(data);
    return !out.indices.empty();
}

/// 三角形键：三个**升序**顶点下标（顶点数 < 2^20 ⇒ 60 位无损打包，比较精确）
u64 TriKey(u32 a, u32 b, u32 c) {
    if (a > b) std::swap(a, b);
    if (b > c) std::swap(b, c);
    if (a > b) std::swap(a, b);
    return (u64)a | ((u64)b << 20) | ((u64)c << 40);
}

} // namespace

TEST_CASE("NaniteMaterialMap(真实资产): Sponza 上 unmapped==0 且材质与原始三角形真值逐簇一致") {
    const std::string path = std::string(HUGE_CONTENT_DIR) + "gltf/Sponza/glTF/Sponza.gltf";
    MergedGeometry g;
    REQUIRE(LoadMerged(path.c_str(), g));

    const u32 vertexCount = (u32)(g.positions.size() / 3u);
    const u32 origTriangleCount = (u32)(g.indices.size() / 3u);
    REQUIRE(vertexCount < (1u << 20));   // TriKey 打包前提

    // 源网格区间表自洽：升序、首尾相接、恰好覆盖全部原始三角形（"不存在空洞"是可断言的前提）
    {
        u32 expected = 0u;
        bool contiguous = true;
        for (const NaniteSourceMeshRange& m : g.meshes) {
            if (m.firstTriangle != expected) contiguous = false;
            expected += m.triangleCount;
        }
        CHECK(contiguous);
        CHECK(expected == origTriangleCount);
    }

    // 逐源网格一条材质记录（与 `NaniteRenderer::EnsureAssetUploaded` 同口径：`materialIndex = i`）
    std::vector<render::NaniteMaterialRecord> materials;
    materials.reserve(g.meshes.size());
    for (usize i = 0; i < g.meshes.size(); ++i) {
        const float rgb[4] = { (float)(i % 7u) * 0.1f, (float)(i % 5u) * 0.2f,
                               (float)(i % 3u) * 0.3f, 1.0f };
        materials.push_back(render::NaniteMakeMaterialRecord(rgb, 0.0f, 0.8f, 0u, (u32)i));
    }

    // ── ① 真实路径：带 `meshes` 的资产构建重载（缺陷就在这里）──
    render::NanitePackedAsset asset;
    REQUIRE(render::BuildNaniteAssetFromGeometry(g.positions, g.normals, g.uvs, g.indices,
                                                 materials, g.meshes, asset));
    REQUIRE(asset.clusters.size() > 0u);

    // ── ② 【判据 1】契约：正常必须 0（缺陷前实测 4103 / 8287）──
    CHECK(asset.stats.unmappedClusters == 0u);

    // ── ③ DAG：用来把簇的三角形还原成**原始合并**三角形（LOD0 的真值来源）──
    render::NaniteClusterDAG dag;
    REQUIRE(render::BuildNaniteClusterDAG(g.positions, g.normals, g.uvs, g.indices, dag));
    REQUIRE(dag.clusters.size() == asset.clusters.size());

    std::unordered_map<u64, u32> origTriByKey;
    origTriByKey.reserve((usize)origTriangleCount * 2u);
    for (u32 t = 0; t < origTriangleCount; ++t) {
        origTriByKey.emplace(TriKey(g.indices[t * 3u], g.indices[t * 3u + 1u], g.indices[t * 3u + 2u]), t);
    }
    const auto meshOfTri = [&](u32 tri) -> u32 {
        u32 lo = 0u, hi = (u32)g.meshes.size();
        while (lo < hi) {
            const u32 mid = lo + (hi - lo) / 2u;
            const NaniteSourceMeshRange& m = g.meshes[mid];
            if (tri < m.firstTriangle) hi = mid;
            else if (tri >= m.firstTriangle + m.triangleCount) lo = mid + 1u;
            else return mid;
        }
        return 0xFFFFFFFFu;
    };

    // 逐源网格的原始几何 AABB（用于**与投票算法无关**的几何一致性核对）
    std::vector<float> meshMin(g.meshes.size() * 3u, 0.0f), meshMax(g.meshes.size() * 3u, 0.0f);
    for (usize m = 0; m < g.meshes.size(); ++m) {
        const NaniteSourceMeshRange& r = g.meshes[m];
        bool first = true;
        for (u32 t = r.firstTriangle; t < r.firstTriangle + r.triangleCount; ++t) {
            for (u32 k = 0; k < 3u; ++k) {
                const u32 v = g.indices[t * 3u + k];
                for (u32 axis = 0; axis < 3u; ++axis) {
                    const float value = g.positions[(usize)v * 3u + axis];
                    if (first || value < meshMin[m * 3u + axis]) meshMin[m * 3u + axis] = value;
                    if (first || value > meshMax[m * 3u + axis]) meshMax[m * 3u + axis] = value;
                }
            }
            first = false;
        }
    }

    // 顶点 → 源网格 归属表（独立重算一份，只用于"每个顶点都有归属"的核对）
    std::vector<u32> ownerOfVertex(vertexCount, 0xFFFFFFFFu);
    for (usize m = 0; m < g.meshes.size(); ++m) {
        const NaniteSourceMeshRange& r = g.meshes[m];
        for (u32 t = r.firstTriangle; t < r.firstTriangle + r.triangleCount; ++t) {
            for (u32 k = 0u; k < 3u; ++k) ownerOfVertex[g.indices[t * 3u + k]] = (u32)m;
        }
    }

    u32 level0Checked = 0u;      // LOD0 里"真值可用"（三角形全部还原成原始三角形）的簇数
    u32 level0Mismatch = 0u;     // 其中实现给出的材质 ≠ 真值 ⇒ 必须 0
    u32 unresolvedTriangles = 0u;///< LOD0 里还原不出原始三角形的三角形数（应为 0：LOD0 就是原始三角形集）
    u32 badMeshLookup = 0u;      // 真值三角形查不到源网格的次数 ⇒ 必须 0（区间表应无缝覆盖）
    u32 unownedVertexClusters = 0u;  // 有顶点查不到源网格的簇数 ⇒ 必须 0
    u32 offMeshClusters = 0u;        // 簇的包围球与所选源网格的 AABB 不相交的簇数 ⇒ 必须 0

    // DAG 结构性守卫：一次核对（**不放进主循环** —— 那样会把断言数刷成几万条噪声）
    u32 dagBoundsViolations = 0u;
    for (usize ci = 0u; ci < asset.clusters.size(); ++ci) {
        const render::NaniteClusterRecord& cluster = dag.clusters[ci];
        const u32 unique = dag.clusterUnique[ci];
        const u32 triBase = dag.uniqueTriangleOffset[unique];
        const u32 count = dag.clusterVertexCount[ci];
        if ((usize)triBase + cluster.triangleCount > dag.uniqueTriangles.size()) ++dagBoundsViolations;
        if ((usize)dag.clusterVertexIndexOffset[ci] + count > dag.clusterVertexIndices.size()) {
            ++dagBoundsViolations;
            continue;
        }
        for (u32 t = 0u; t < cluster.triangleCount; ++t) {
            const render::NanitePackedTriangle& packed = dag.uniqueTriangles[triBase + t];
            if (render::NaniteTriangleIndex0(packed) >= count ||
                render::NaniteTriangleIndex1(packed) >= count ||
                render::NaniteTriangleIndex2(packed) >= count) ++dagBoundsViolations;
        }
    }
    CHECK(dagBoundsViolations == 0u);

    for (usize ci = 0u; ci < asset.clusters.size(); ++ci) {
        const render::NaniteClusterRecord& cluster = asset.clusters[ci];
        const u32 level = dag.clusterLevel[ci];

        // 该出现的三角形 → 网格顶点（原始合并空间）
        const u32 unique = dag.clusterUnique[ci];
        const u32 triBase = dag.uniqueTriangleOffset[unique];
        const u32 vertexBase = dag.clusterVertexIndexOffset[ci];
        const u32 localVertexCount = dag.clusterVertexCount[ci];

        u32 resolved = 0u;
        std::vector<u32> truthVotes(g.meshes.size(), 0u);
        for (u32 t = 0u; t < cluster.triangleCount; ++t) {
            const render::NanitePackedTriangle& packed = dag.uniqueTriangles[triBase + t];
            const u32 local[3] = { render::NaniteTriangleIndex0(packed),
                                   render::NaniteTriangleIndex1(packed),
                                   render::NaniteTriangleIndex2(packed) };
            u32 meshVertices[3];
            for (u32 k = 0u; k < 3u; ++k) {
                meshVertices[k] = dag.clusterVertexIndices[vertexBase + local[k]];
            }
            // 真值：这个三角形在**原始合并几何**里是哪一个三角形 ⇒ 属于哪个源网格
            //   （原始三角形恰好属于一个源网格 ⇒ 这是**逐三角形精确**的真值，不依赖任何投票口径）
            const auto found = origTriByKey.find(TriKey(meshVertices[0], meshVertices[1], meshVertices[2]));
            if (found == origTriByKey.end()) {
                if (level == 0u) ++unresolvedTriangles;
                continue;
            }
            ++resolved;
            const u32 mesh = meshOfTri(found->second);
            if (mesh == 0xFFFFFFFFu) { ++badMeshLookup; continue; }
            ++truthVotes[mesh];
        }
        // 真值的多数票（平票取下标更小的网格 —— 与实现同口径，但票源是**原始三角形**）
        u32 truthMesh = 0xFFFFFFFFu;
        u32 truthBest = 0u;
        for (u32 m = 0u; m < (u32)truthVotes.size(); ++m) {
            if (truthVotes[m] > truthBest) { truthBest = truthVotes[m]; truthMesh = m; }
        }

        // 【判据 2】LOD0 的真值核对：每个簇必须拿到"它自己的原始三角形所属源网格"的材质
        if (level == 0u && resolved == cluster.triangleCount && truthMesh < (u32)g.meshes.size()) {
            ++level0Checked;
            const u32 expected = g.meshes[truthMesh].materialIndex;
            if (cluster.materialID != expected) ++level0Mismatch;
        }

        // 【判据 3】每个簇的每个顶点都必须有源网格归属（"任何空间都命不中"的簇数为 0）
        {
            bool unowned = false;
            for (u32 v = 0u; v < localVertexCount; ++v) {
                if (ownerOfVertex[dag.clusterVertexIndices[vertexBase + v]] == 0xFFFFFFFFu) {
                    unowned = true;
                    break;
                }
            }
            if (unowned) ++unownedVertexClusters;
        }

        // 【判据 4】与投票算法无关的几何核对：所选源网格必须真的出现在该簇所在的位置
        //   （簇的包围球与所选网格 AABB 的距离必须 ≤ 半径）
        if (cluster.materialID < (u32)g.meshes.size()) {
            const u32 m = cluster.materialID;   // 本用例 materialIndex == 网格下标
            float distanceSquared = 0.0f;
            for (u32 axis = 0u; axis < 3u; ++axis) {
                const float center = cluster.boundsCenterRadius[axis];
                float delta = 0.0f;
                if (center < meshMin[m * 3u + axis]) delta = meshMin[m * 3u + axis] - center;
                else if (center > meshMax[m * 3u + axis]) delta = center - meshMax[m * 3u + axis];
                distanceSquared += delta * delta;
            }
            const float radius = cluster.boundsCenterRadius[3];
            if (distanceSquared > radius * radius) ++offMeshClusters;
        }
    }

    CHECK(level0Checked > 0u);
    CHECK(level0Mismatch == 0u);
    CHECK(unresolvedTriangles == 0u);
    CHECK(badMeshLookup == 0u);
    CHECK(unownedVertexClusters == 0u);
    CHECK(offMeshClusters == 0u);

    // `materials_sample` 那一行的 `cluster_material_id=[min max distinct]` 口径
    const auto idRange = [](const std::vector<u32>& ids, u32& outMin, u32& outMax, u32& outDistinct) {
        outMin = 0xFFFFFFFFu; outMax = 0u; outDistinct = 0u;
        std::vector<u32> seen;
        for (const u32 id : ids) {
            if (id < outMin) outMin = id;
            if (id > outMax) outMax = id;
            if (std::find(seen.begin(), seen.end(), id) == seen.end()) { seen.push_back(id); ++outDistinct; }
        }
    };
    std::vector<u32> newIds(asset.clusters.size());
    for (usize ci = 0u; ci < asset.clusters.size(); ++ci) newIds[ci] = asset.clusters[ci].materialID;
    u32 newMin = 0u, newMax = 0u, newDistinct = 0u;
    idRange(newIds, newMin, newMax, newDistinct);

    MESSAGE("真实资产映射: clusters=" << asset.clusters.size()
            << " levels=" << dag.stats.levelCount
            << " unmapped=" << asset.stats.unmappedClusters
            << " multi_mesh=" << asset.stats.multiMeshClusters
            << " | LOD0 真值核对 " << level0Checked << " 簇错 " << level0Mismatch
            << " | 无归属顶点簇 " << unownedVertexClusters
            << " | 几何核对越界簇 " << offMeshClusters);
    MESSAGE("材质段映射读数: unmapped=" << asset.stats.unmappedClusters
            << " multi_mesh=" << asset.stats.multiMeshClusters
            << " cluster_material_id=[min=" << newMin << " max=" << newMax
            << " distinct=" << newDistinct << "]");
}
