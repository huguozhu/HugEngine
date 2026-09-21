// ============================================================
// Tests/TestNaniteMaterialBin.cpp — Material Bin 的正确性用例（§14.8 任务 25）
//
// 【本文件钉住什么（任务 27 明确要求补的那一项）】
//   任务 25 的交付物是一份 `u32[clusterCount]` 的"按材质排序的簇下标"辅助数组。它是**只读**的
//   收益证据（光栅仍按可见簇列表的顺序处理簇），因此它的正确性**必须**由不变量来钉：
//     ① `bins` 是 `0..clusterCount-1` 的一个**排列**（每个簇恰好出现一次 —— 既不能漏也不能重）；
//     ② `bins` 里的 `materialID` **非降序**（即"同材质的簇连续"，这才是"按材质分组"的定义）；
//     ③ 同材质的簇之间保持原有的相对顺序（**稳定**）⇒ 同一个输入逐位可复现；
//     ④ 生成 bin **不改动任何输入**（资产/BVH/DAG/格式的"一个字节都不动"是可断言的）；
//     ⑤ 越界 `materialID` **不 clamp**：不产出 bin 并如实报出计数（否则 bin 会静默分错组）；
//     ⑥ 真实资产上的**收益**：按 bin 顺序遍历的 `material_switches` 必须**不高于**当前顺序
//        （顺序本身就是收益的定义），且当前顺序的切换次数明显大于"材质数"这个下界。
//
// 【三个用例的层次】合成小例子（①～⑤，快且边界清楚）+ 真实 Sponza 资产（①～⑥，真且全）。
//   合成例子用的是**手工构造的簇记录**（只填 `materialID`，其余字段留 0）—— 本文件测的是
//   "按 `materialID` 分组"这一条纯函数规则，与簇的几何字段无关，故意不引入几何数据。
// ============================================================

#include "doctest.h"

#include "Nanite/NaniteUpload.h"

#include "cgltf.h"

#include <algorithm>
#include <cstring>     // std::memcmp（"生成 bin 不改动资产"的逐字节断言）
#include <string>
#include <vector>

using namespace he;

namespace {

using he::render::NaniteClusterRecord;
using he::render::NaniteMaterialBin;
using he::render::NaniteMaterialBinStats;

/// 造一批只带 `materialID` 的簇记录（其余字段留 0 —— 本文件只测分组规则）
std::vector<NaniteClusterRecord> MakeClusters(const std::vector<u32>& materialIds) {
    std::vector<NaniteClusterRecord> clusters(materialIds.size());
    for (usize i = 0u; i < materialIds.size(); ++i) clusters[i].materialID = materialIds[i];
    return clusters;
}

/// 按一条"簇下标顺序"数相邻换材质的次数（与 `NaniteRenderer::LogMaterialBinReadback` 同口径：
/// 第一条不计为切换）。写成与实现**独立**的一份，免得"用实现验证实现"。
u32 CountMaterialSwitches(const std::vector<NaniteClusterRecord>& clusters,
                          const std::vector<u32>& order) {
    u32 switches = 0u;
    u32 prev = 0xFFFFFFFFu;
    for (const u32 cluster : order) {
        const u32 material = clusters[cluster].materialID;
        if (prev != 0xFFFFFFFFu && material != prev) ++switches;
        prev = material;
    }
    return switches;
}

/// 把一条簇下标序列按 bin 位次重排（与读数行的 `material_switches_bin` 同一个构造）
std::vector<u32> OrderByBin(const NaniteMaterialBin& bin, const std::vector<u32>& order) {
    std::vector<u32> rank(bin.bins.size(), 0xFFFFFFFFu);
    for (u32 i = 0u; i < (u32)bin.bins.size(); ++i) rank[bin.bins[i]] = i;
    std::vector<u32> ranks;
    ranks.reserve(order.size());
    for (const u32 cluster : order) {
        if (rank[cluster] != 0xFFFFFFFFu) ranks.push_back(rank[cluster]);
    }
    std::sort(ranks.begin(), ranks.end());
    std::vector<u32> out;
    out.reserve(ranks.size());
    for (const u32 r : ranks) out.push_back(bin.bins[r]);
    return out;
}

/// 自然顺序 `0..n-1`（"当前顺序"的一个具体代表）
std::vector<u32> NaturalOrder(usize n) {
    std::vector<u32> order(n);
    for (usize i = 0u; i < n; ++i) order[i] = (u32)i;
    return order;
}

/// 三条结构不变量（排列 + 非降序 + 稳定），返回违反数之和；0 = 全部成立
u32 CountBinViolations(const std::vector<NaniteClusterRecord>& clusters,
                       const NaniteMaterialBin& bin) {
    u32 violations = 0u;
    // ① 排列：每个簇下标恰好出现一次
    std::vector<u32> seen(clusters.size(), 0u);
    for (const u32 cluster : bin.bins) {
        if (cluster >= clusters.size()) { ++violations; continue; }
        if (seen[cluster] != 0u) ++violations;   // 重复 ⇒ 必有另一个被漏掉
        ++seen[cluster];
    }
    for (const u32 count : seen) {
        if (count != 1u) ++violations;
    }
    // ② 非降序（同材质连续）+ ③ 稳定（同材质内部保持原簇下标升序）
    u32 prevMaterial = 0xFFFFFFFFu;
    u32 prevCluster = 0u;
    bool first = true;
    for (const u32 cluster : bin.bins) {
        if (cluster >= clusters.size()) continue;
        const u32 material = clusters[cluster].materialID;
        if (!first) {
            if (material < prevMaterial) ++violations;                       // 降序 ⇒ 破了分组
            if (material == prevMaterial && cluster < prevCluster) ++violations;  // 不稳定
        }
        prevMaterial = material;
        prevCluster = cluster;
        first = false;
    }
    return violations;
}

// ── 真实 Sponza 合并几何（与 `TestNaniteMaterialMap.cpp` 同一套离线合并口径）──
struct MergedGeometry {
    std::vector<float> positions;
    std::vector<float> normals;
    std::vector<float> uvs;
    std::vector<u32>   indices;
    std::vector<render::NaniteSourceMeshRange> meshes;
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

    render::NaniteSourceMeshRange range;
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

} // namespace

// ============================================================
// ① 合成小例子：排列 / 非降序 / 稳定 / 只算读数 / 越界不 clamp
// ============================================================
TEST_CASE("NaniteMaterialBin(合成): bin 是 0..N-1 的排列且按材质非降序、同材质稳定") {
    // 材质序列 0,1,0,1,2,0 ⇒ bin 应为 [0,2,5, 1,3, 4]（0 组的原序 0,2,5；1 组的 1,3；2 组的 4）
    const std::vector<u32> materialIds = { 0u, 1u, 0u, 1u, 2u, 0u };
    const std::vector<NaniteClusterRecord> clusters = MakeClusters(materialIds);

    NaniteMaterialBin bin;
    const NaniteMaterialBinStats stats = render::BuildNaniteMaterialBin(clusters, 3u, bin);

    CHECK(stats.clusterCount == 6u);
    CHECK(stats.materialCount == 3u);
    CHECK(stats.distinctCount == 3u);
    CHECK(stats.outOfRangeCount == 0u);
    CHECK(bin.bins.size() == 6u);

    // ② 三条结构不变量（排列 + 非降序 + 稳定）—— 这是本用例的核心断言
    CHECK(CountBinViolations(clusters, bin) == 0u);

    // 逐位核对期望结果（比"只有不变量"更强：不变量对"任意非降序排列"都成立，而**稳定**这一条
    // 把结果钉成唯一的那一个）
    REQUIRE(bin.bins.size() == 6u);
    CHECK(bin.bins[0] == 0u);
    CHECK(bin.bins[1] == 2u);
    CHECK(bin.bins[2] == 5u);
    CHECK(bin.bins[3] == 1u);
    CHECK(bin.bins[4] == 3u);
    CHECK(bin.bins[5] == 4u);

    // 收益：当前顺序 0,1,0,1,2,0 有 5 次切换（0→1→0→1→2→0）；
    //       bin 顺序 0,0,0,1,1,2 只有 2 次（= "出现的材质数 - 1"，即理论下界）
    std::vector<u32> current;
    for (u32 i = 0u; i < 6u; ++i) current.push_back(i);
    const u32 before = CountMaterialSwitches(clusters, current);
    const std::vector<u32> byBin = OrderByBin(bin, current);
    const u32 after = CountMaterialSwitches(clusters, byBin);
    CHECK(before == 5u);
    CHECK(after == 2u);          // = "出现的材质数 - 1"
    CHECK(after <= before);

    // 【只算读数】空输出 span ⇒ 不写数组，但读数照常给出（调用方只要统计时用这条路）
    const NaniteMaterialBinStats statsOnly =
        render::BuildNaniteMaterialBin(clusters, 3u, std::span<u32>{});
    CHECK(statsOnly.clusterCount == 6u);
    CHECK(statsOnly.distinctCount == 3u);
    CHECK(statsOnly.outOfRangeCount == 0u);
}

TEST_CASE("NaniteMaterialBin(合成): materialID 越界不 clamp —— 不产出 bin 并如实报数") {
    // 3 号簇的 materialID = 9 超出材质段 [0, 2) ⇒ 必须**拒绝**，而不是悄悄夹到某个桶里
    const std::vector<u32> materialIds = { 0u, 1u, 9u, 1u };
    const std::vector<NaniteClusterRecord> clusters = MakeClusters(materialIds);

    NaniteMaterialBin bin;
    const NaniteMaterialBinStats stats = render::BuildNaniteMaterialBin(clusters, 2u, bin);
    CHECK(stats.clusterCount == 4u);
    CHECK(stats.outOfRangeCount == 1u);   // 如实报出（不静默）
    CHECK(bin.bins.empty());              // 不产出半成品 bin

    // `materialCount == 0`（空材质段）时任何非 0 ID 都算越界；全 0 的资产仍然合法
    std::vector<NaniteClusterRecord> allZero = MakeClusters({ 0u, 0u });
    NaniteMaterialBin zeroBin;
    const NaniteMaterialBinStats zeroStats = render::BuildNaniteMaterialBin(allZero, 0u, zeroBin);
    CHECK(zeroStats.outOfRangeCount == 2u);
    CHECK(zeroBin.bins.empty());
}

TEST_CASE("NaniteMaterialBin(合成): 空输入合法 + 同一输入两次调用逐位一致") {
    // 空输入 ⇒ 全 0 读数、空 bin（与任务 7/8/9/10 的"空网格是合法输入"同口径）
    NaniteMaterialBin emptyBin;
    const NaniteMaterialBinStats emptyStats =
        render::BuildNaniteMaterialBin(std::span<const NaniteClusterRecord>{}, 4u, emptyBin);
    CHECK(emptyStats.clusterCount == 0u);
    CHECK(emptyStats.distinctCount == 0u);
    CHECK(emptyStats.outOfRangeCount == 0u);
    CHECK(emptyBin.bins.empty());

    // 确定性：两次调用（两个独立出参）逐位一致
    const std::vector<u32> materialIds = { 3u, 1u, 3u, 0u, 1u, 3u, 2u, 0u, 1u };
    const std::vector<NaniteClusterRecord> clusters = MakeClusters(materialIds);
    NaniteMaterialBin a, b;
    // 返回值本身已在别处断言过；这里只关心**两次结果是否逐位相同** ⇒ 显式丢弃（避免 C4834）
    (void)render::BuildNaniteMaterialBin(clusters, 4u, a);
    (void)render::BuildNaniteMaterialBin(clusters, 4u, b);
    REQUIRE(a.bins.size() == b.bins.size());
    CHECK(std::equal(a.bins.begin(), a.bins.end(), b.bins.begin()));
    CHECK(CountBinViolations(clusters, a) == 0u);
}

// ============================================================
// ② 真实 Sponza 资产：不变量 + 收益 + "不改动资产"
// ============================================================
TEST_CASE("NaniteMaterialBin(真实资产): Sponza 上 bin 有序且是排列；按 bin 顺序切换数不增") {
    const std::string path = std::string(HUGE_CONTENT_DIR) + "gltf/Sponza/glTF/Sponza.gltf";
    MergedGeometry g;
    REQUIRE(LoadMerged(path.c_str(), g));

    // 逐源网格一条材质记录（与 `NaniteRenderer::EnsureAssetUploaded` 同口径）
    std::vector<render::NaniteMaterialRecord> materials;
    materials.reserve(g.meshes.size());
    for (usize i = 0; i < g.meshes.size(); ++i) {
        const float rgb[4] = { (float)(i % 7u) * 0.1f, (float)(i % 5u) * 0.2f,
                               (float)(i % 3u) * 0.3f, 1.0f };
        materials.push_back(render::NaniteMakeMaterialRecord(rgb, 0.0f, 0.8f, 0u, (u32)i));
    }

    render::NanitePackedAsset asset;
    REQUIRE(render::BuildNaniteAssetFromGeometry(g.positions, g.normals, g.uvs, g.indices,
                                                 materials, g.meshes, asset));
    REQUIRE(asset.clusters.size() > 0u);
    const u32 materialCount = (u32)asset.materials.size();

    // ── "不改动资产"的**前置快照**：整段簇记录的字节副本（生成 bin 之后必须逐字节相同）──
    const std::vector<render::NaniteClusterRecord> before = asset.clusters;

    NaniteMaterialBin bin;
    const NaniteMaterialBinStats stats =
        render::BuildNaniteMaterialBin(asset.clusters, materialCount, bin);

    // ① 读数与不变式
    CHECK(stats.clusterCount == (u32)asset.clusters.size());
    CHECK(stats.materialCount == materialCount);
    CHECK(stats.outOfRangeCount == 0u);
    CHECK(stats.distinctCount > 0u);
    CHECK(stats.distinctCount <= materialCount);
    CHECK(bin.bins.size() == asset.clusters.size());
    CHECK(CountBinViolations(asset.clusters, bin) == 0u);

    // ② 【硬性要求】生成 bin **不改动资产本体**（簇记录逐字节相同 —— `materialID` 是 bin 的输入，
    //    绝不能被它顺带改写；资产/BVH/DAG/格式的"一个字节都不动"在这里落成断言）
    REQUIRE(before.size() == asset.clusters.size());
    CHECK(std::memcmp(before.data(), asset.clusters.data(),
                      before.size() * sizeof(render::NaniteClusterRecord)) == 0);

    // ③ 收益：本用例用**资产的自然顺序** `0..N-1` 当"当前顺序"（读数行用的是 GPU 可见簇列表的
    //    顺序，那个只有跑起来才有；这里要验证的是"按材质分组这件事本身是否真的降低切换数"）。
    const std::vector<u32> naturalOrder = NaturalOrder(before.size());
    const u32 switchBefore = CountMaterialSwitches(before, naturalOrder);
    const std::vector<u32> binOrder = OrderByBin(bin, naturalOrder);
    const u32 switchAfter = CountMaterialSwitches(before, binOrder);

    // 下界：每个出现的材质至少贡献一次"进入该材质组"的切换 ⇒ 组数 - 1 是理论下界
    CHECK(switchAfter == (stats.distinctCount > 0u ? stats.distinctCount - 1u : 0u));
    CHECK(switchAfter <= switchBefore);
    // 真实资产上收益必须是**显著的**（否则说明 bin 没起作用）：至少减半
    CHECK(switchBefore > switchAfter * 2u);

    MESSAGE("Sponza 材质 bin: clusters=" << stats.clusterCount
            << " materials=" << stats.materialCount
            << " distinct=" << stats.distinctCount
            << " out_of_range=" << stats.outOfRangeCount
            << " | material_switches(自然顺序)=" << switchBefore
            << " ⇒ (bin 顺序)=" << switchAfter
            << " [下界 distinct-1=" << (stats.distinctCount > 0u ? stats.distinctCount - 1u : 0u) << "]");
}
