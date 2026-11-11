// ============================================================
//  Tests/TestNaniteFileV2.cpp
//
//  T2（《01.Nanite离线预处理与全Mesh管线方案》）：`.nanite` v2 的读写、v1 兼容与确定性。
//
//  【覆盖的验收项】
//   1. 空资产 / 单簇 / 多簇资产往返：读回来的固定五段与写出去**逐字节相等**；
//   2. 扩展段往返：段表条目（类型/偏移/长度）与载荷字节一致，且载荷 16B 对齐；
//   3. v1 兼容：把 v2 文件改写成 v1 头部后仍可解析，`extSectionCount = 0`、哈希/构建器
//      版本被标记为无效（几何段不变）；
//   4. 失败路径：魔数错、版本超范围、截断、段表越界/压住固定五段都能报出对应错误码；
//   5. 构建器确定性：同一输入两次 `BuildNaniteAssetFromGeometry` 逐字节相等（v2 头也相等）。
// ============================================================

#include "doctest.h"

#include "Nanite/NaniteTypes.h"
#include "Nanite/NaniteUpload.h"

#include <cstring>
#include <string>
#include <vector>

using namespace he;
using namespace he::render;

namespace {

// ── 测试几何：确定性、不依赖资产文件 ──

struct TestMesh {
    std::vector<float> positions;   // 每顶点 3 个 float
    std::vector<float> normals;     // 每顶点 3 个 float
    std::vector<float> uvs;         // 每顶点 2 个 float
    std::vector<u32>   indices;     // 三角形列表
};

/// 规则平面网格（XY 平面、+Z 法线共享顶点），`quads × quads` 个四边形
TestMesh MakeGrid(u32 quads) {
    TestMesh mesh;
    const u32 side = quads + 1u;
    for (u32 y = 0; y <= quads; ++y) {
        for (u32 x = 0; x < side; ++x) {
            mesh.positions.push_back((float)x);
            mesh.positions.push_back((float)y);
            mesh.positions.push_back(0.0f);
            mesh.normals.push_back(0.0f);
            mesh.normals.push_back(0.0f);
            mesh.normals.push_back(1.0f);
            mesh.uvs.push_back((float)x / (float)quads);
            mesh.uvs.push_back((float)y / (float)quads);
        }
    }
    for (u32 y = 0; y < quads; ++y) {
        for (u32 x = 0; x < quads; ++x) {
            const u32 i0 = y * side + x;
            const u32 i1 = i0 + 1u;
            const u32 i2 = i0 + side;
            const u32 i3 = i2 + 1u;
            mesh.indices.push_back(i0); mesh.indices.push_back(i1); mesh.indices.push_back(i3);
            mesh.indices.push_back(i0); mesh.indices.push_back(i3); mesh.indices.push_back(i2);
        }
    }
    return mesh;
}

/// 用给定几何构建一份 v1 语义的资产（`BuildNaniteAssetFromGeometry` 的产物）
bool BuildAsset(const TestMesh& mesh, NanitePackedAsset& outAsset) {
    const std::vector<NaniteMaterialRecord> materials;   // 空：materialCount = 0
    return BuildNaniteAssetFromGeometry(mesh.positions, mesh.normals, mesh.uvs, mesh.indices,
                                        materials, outAsset);
}

/// 造一个 MaterialDesc 载荷（内容只用确定性的常量，便于逐字节比较）
std::vector<u8> MakeMaterialDescPayload(u32 count) {
    std::vector<NaniteMaterialDesc> descs(count);
    for (u32 i = 0; i < count; ++i) {
        descs[i].materialIndex   = i;
        descs[i].alphaMode       = i % 3u;
        descs[i].baseColorFactor[0] = 0.25f * (float)(i + 1u);
        descs[i].metallicFactor  = 0.1f * (float)i;
        descs[i].roughnessFactor = 0.5f;
        descs[i].textureImageIndex[kNaniteMaterialTextureSlotBaseColor] = 100u + i;
    }
    std::vector<u8> bytes(sizeof(NaniteMaterialDesc) * descs.size());
    std::memcpy(bytes.data(), descs.data(), bytes.size());
    return bytes;
}

/// 把 v2 文件字节改写成"v1 头部 + 固定五段"的形式（模拟历史版本产出的文件）
std::vector<u8> MakeV1BytesFromV2(const std::vector<u8>& v2Bytes) {
    NaniteFileLayout layout{};
    REQUIRE(ValidateNaniteFile(v2Bytes.data(), v2Bytes.size(), &layout) == NaniteFileError::None);

    // v1 的固定五段长度（无扩展段表）
    const usize v1TotalBytes = layout.lodOffset + layout.lodBytes;
    std::vector<u8> v1Bytes(v2Bytes.begin(), v2Bytes.begin() + (long long)v1TotalBytes);

    NaniteFileHeader header{};
    std::memcpy(&header, v1Bytes.data(), sizeof(header));
    header.version = 1u;
    for (u32 i = 0; i < kNaniteFileHeaderReservedU32; ++i) header._reserved[i] = 0u;
    std::memcpy(v1Bytes.data(), &header, sizeof(header));
    return v1Bytes;
}

} // namespace

// ============================================================
// 1. 往返：空资产 / 单簇 / 多簇
// ============================================================

TEST_CASE("NaniteFileV2: 空资产往返（只有 96B 头部，计数全 0）") {
    NanitePackedAsset asset;
    TestMesh empty;
    REQUIRE(BuildAsset(empty, asset));
    CHECK(asset.clusters.empty());

    NaniteFileWriteOptions options;   // 无扩展段
    std::vector<u8> bytes;
    REQUIRE(WriteNaniteFile(asset, options, bytes));
    CHECK(bytes.size() >= kNaniteFileHeaderBytes);

    NaniteFileImage image;
    REQUIRE(ReadNaniteFile(bytes.data(), bytes.size(), image) == NaniteFileError::None);
    CHECK(image.header.version == kNaniteFileVersion);
    CHECK(image.header.clusterCount == 0u);
    CHECK(image.layout.extSectionCount == 0u);
    CHECK(image.extEntries.empty());
    CHECK(image.bytes.size() == bytes.size());
    CHECK(std::memcmp(image.bytes.data(), bytes.data(), bytes.size()) == 0);
}

TEST_CASE("NaniteFileV2: 单簇 / 多簇资产的固定五段往返逐字节相等") {
    for (u32 quads : { 1u, 8u }) {   // 1×1 = 2 三角形（单簇）；8×8 = 128 三角形（多簇）
        NanitePackedAsset asset;
        TestMesh mesh = MakeGrid(quads);
        REQUIRE(BuildAsset(mesh, asset));
        REQUIRE_FALSE(asset.clusters.empty());

        NaniteFileWriteOptions options;
        std::vector<u8> bytes;
        REQUIRE(WriteNaniteFile(asset, options, bytes));

        NaniteFileImage image;
        REQUIRE(ReadNaniteFile(bytes.data(), bytes.size(), image) == NaniteFileError::None);

        // 固定五段：簇 / 顶点 / 索引 / 材质 / LOD 五段逐字节相等
        CHECK(image.layout.extSectionCount == 0u);
        CHECK(image.layout.clusterBytes == asset.layout.clusterBytes);
        CHECK(std::memcmp(image.bytes.data() + image.layout.clusterOffset,
                          asset.bytes.data() + asset.layout.clusterOffset,
                          asset.layout.clusterBytes) == 0);
        CHECK(std::memcmp(image.bytes.data() + image.layout.vertexOffset,
                          asset.bytes.data() + asset.layout.vertexOffset,
                          asset.layout.vertexBytes) == 0);
        CHECK(std::memcmp(image.bytes.data() + image.layout.indexOffset,
                          asset.bytes.data() + asset.layout.indexOffset,
                          asset.layout.indexBytes) == 0);
        CHECK(std::memcmp(image.bytes.data() + image.layout.materialOffset,
                          asset.bytes.data() + asset.layout.materialOffset,
                          asset.layout.materialBytes) == 0);
        CHECK(std::memcmp(image.bytes.data() + image.layout.lodOffset,
                          asset.bytes.data() + asset.layout.lodOffset,
                          asset.layout.lodBytes) == 0);
    }
}

// ============================================================
// 2. 扩展段往返
// ============================================================

TEST_CASE("NaniteFileV2: 扩展段（MaterialDesc + SourceClusterRange）往返一致") {
    NanitePackedAsset asset;
    REQUIRE(BuildAsset(MakeGrid(4), asset));

    std::vector<NaniteSourceClusterRange> ranges(1);
    ranges[0].firstCluster   = 0u;
    ranges[0].clusterCount   = (u32)asset.clusters.size();
    ranges[0].primitiveIndex = 0u;
    ranges[0].materialIndex  = 0u;
    std::vector<u8> rangeBytes(sizeof(NaniteSourceClusterRange));
    std::memcpy(rangeBytes.data(), ranges.data(), rangeBytes.size());

    NaniteFileWriteOptions options;
    options.builderVersion = 7u;
    options.settingsHash   = 0x1122334455667788ull;
    options.sourceHash     = 0x99AABBCCDDEEFF00ull;
    options.extSections.push_back({ NaniteExtSectionType::MaterialDesc, MakeMaterialDescPayload(2) });
    options.extSections.push_back({ NaniteExtSectionType::SourceMeshRange, rangeBytes });

    std::vector<u8> bytes;
    REQUIRE(WriteNaniteFile(asset, options, bytes));

    NaniteFileImage image;
    REQUIRE(ReadNaniteFile(bytes.data(), bytes.size(), image) == NaniteFileError::None);

    // 头部元数据
    CHECK(image.header.version == kNaniteFileVersion);
    CHECK(NaniteHeaderBuilderVersion(image.header) == 7u);
    bool settingsValid = false, sourceValid = false;
    CHECK(NaniteHeaderSettingsHash(image.header, &settingsValid) == options.settingsHash);
    CHECK(NaniteHeaderSourceHash(image.header, &sourceValid) == options.sourceHash);
    CHECK(settingsValid);
    CHECK(sourceValid);

    // 段表
    REQUIRE(image.extEntries.size() == 2u);
    CHECK(image.extEntries[0].type == (u32)NaniteExtSectionType::MaterialDesc);
    CHECK(image.extEntries[1].type == (u32)NaniteExtSectionType::SourceMeshRange);
    CHECK((image.extEntries[0].offset % kNaniteFileAlignment) == 0u);
    CHECK((image.extEntries[1].offset % kNaniteFileAlignment) == 0u);
    CHECK(image.extEntries[0].size == options.extSections[0].payload.size());
    CHECK(image.extEntries[1].size == options.extSections[1].payload.size());

    // 载荷字节
    for (usize s = 0; s < image.extEntries.size(); ++s) {
        const NaniteExtSectionEntry& entry = image.extEntries[s];
        CHECK(std::memcmp(image.bytes.data() + entry.offset,
                          options.extSections[s].payload.data(), entry.size) == 0);
    }

    // 五段仍然逐字节相等（扩展段没有挤动固定段）
    CHECK(std::memcmp(image.bytes.data() + image.layout.clusterOffset,
                      asset.bytes.data() + asset.layout.clusterOffset,
                      asset.layout.clusterBytes) == 0);
}

// ============================================================
// 3. v1 兼容
// ============================================================

TEST_CASE("NaniteFileV2: v1 文件可解析，元数据标记为无效、几何段不变") {
    NanitePackedAsset asset;
    REQUIRE(BuildAsset(MakeGrid(4), asset));

    NaniteFileWriteOptions options;
    options.extSections.push_back({ NaniteExtSectionType::MaterialDesc, MakeMaterialDescPayload(1) });
    std::vector<u8> v2Bytes;
    REQUIRE(WriteNaniteFile(asset, options, v2Bytes));

    const std::vector<u8> v1Bytes = MakeV1BytesFromV2(v2Bytes);

    NaniteFileImage image;
    REQUIRE(ReadNaniteFile(v1Bytes.data(), v1Bytes.size(), image) == NaniteFileError::None);
    CHECK(image.header.version == 1u);
    CHECK(image.layout.extSectionCount == 0u);
    CHECK(image.extEntries.empty());
    CHECK(NaniteHeaderBuilderVersion(image.header) == 0u);   // 0 = 无版本信息
    bool settingsValid = true, sourceValid = true;
    CHECK(NaniteHeaderSettingsHash(image.header, &settingsValid) == 0u);
    CHECK(NaniteHeaderSourceHash(image.header, &sourceValid) == 0u);
    CHECK_FALSE(settingsValid);
    CHECK_FALSE(sourceValid);

    // 几何段与 v2 文件里的同一段逐字节相等（兼容的实质）
    CHECK(std::memcmp(image.bytes.data() + image.layout.clusterOffset,
                      v2Bytes.data() + image.layout.clusterOffset,
                      image.layout.clusterBytes) == 0);
}

// ============================================================
// 4. 失败路径
// ============================================================

TEST_CASE("NaniteFileV2: 魔数/版本/截断/段表越界都能报出对应错误码") {
    NanitePackedAsset asset;
    REQUIRE(BuildAsset(MakeGrid(4), asset));

    NaniteFileWriteOptions options;
    options.extSections.push_back({ NaniteExtSectionType::MaterialDesc, MakeMaterialDescPayload(1) });
    std::vector<u8> bytes;
    REQUIRE(WriteNaniteFile(asset, options, bytes));

    NaniteFileImage image;

    SUBCASE("魔数被改坏") {
        std::vector<u8> broken = bytes;
        broken[0] = 'X';
        CHECK(ReadNaniteFile(broken.data(), broken.size(), image) == NaniteFileError::BadMagic);
    }

    SUBCASE("版本超出可读范围") {
        std::vector<u8> broken = bytes;
        NaniteFileHeader header{};
        std::memcpy(&header, broken.data(), sizeof(header));
        header.version = kNaniteFileVersion + 1u;
        std::memcpy(broken.data(), &header, sizeof(header));
        CHECK(ReadNaniteFile(broken.data(), broken.size(), image) == NaniteFileError::BadVersion);

        header.version = 0u;   // 低于可读下限
        std::memcpy(broken.data(), &header, sizeof(header));
        CHECK(ReadNaniteFile(broken.data(), broken.size(), image) == NaniteFileError::BadVersion);
    }

    SUBCASE("截断（短于段表推导的最小长度）") {
        // 注意：只砍掉尾部扩展段载荷会命中 BadExtSection（段表存在、载荷越界），
        // 要验证 OutOfBounds 必须砍到 layout.totalBytes 以下（即固定五段/段表不完整）。
        NaniteFileLayout layout{};
        REQUIRE(ValidateNaniteFile(bytes.data(), bytes.size(), &layout) == NaniteFileError::None);
        std::vector<u8> broken(bytes.begin(), bytes.begin() + (long long)(layout.totalBytes - 8u));
        CHECK(ReadNaniteFile(broken.data(), broken.size(), image) == NaniteFileError::OutOfBounds);
    }

    SUBCASE("段表条目压住固定五段") {
        std::vector<u8> broken = bytes;
        NaniteFileLayout layout{};
        REQUIRE(ValidateNaniteFile(broken.data(), broken.size(), &layout) == NaniteFileError::None);
        REQUIRE(layout.extSectionCount > 0u);

        NaniteExtSectionEntry entry{};
        std::memcpy(&entry, broken.data() + layout.extEntryOffset, sizeof(entry));
        entry.offset = 0u;   // 指向文件头 —— 非法
        std::memcpy(broken.data() + layout.extEntryOffset, &entry, sizeof(entry));
        CHECK(ReadNaniteFile(broken.data(), broken.size(), image) == NaniteFileError::BadExtSection);
    }

    SUBCASE("扩展段数超上限") {
        std::vector<u8> broken = bytes;
        NaniteFileHeader header{};
        std::memcpy(&header, broken.data(), sizeof(header));
        header._reserved[kNaniteReservedSlotExtSectionCount] = kNaniteMaxExtSections + 1u;
        std::memcpy(broken.data(), &header, sizeof(header));
        CHECK(ReadNaniteFile(broken.data(), broken.size(), image) ==
              NaniteFileError::BadExtSectionCount);
    }
}

TEST_CASE("NaniteFileV2: 写入端拒绝超上限的扩展段数") {
    NanitePackedAsset asset;
    REQUIRE(BuildAsset(MakeGrid(1), asset));

    NaniteFileWriteOptions options;
    options.extSections.resize(kNaniteMaxExtSections + 1u);   // 空载荷，仅数量超限
    std::vector<u8> bytes;
    CHECK_FALSE(WriteNaniteFile(asset, options, bytes));
    CHECK(bytes.empty());   // 失败时不改写输出
}

// ============================================================
// 5. 构建器确定性（同一输入两次逐字节相等）
// ============================================================

TEST_CASE("NaniteFileV2: 同一输入两次构建 + 两次写文件逐字节相等") {
    TestMesh mesh = MakeGrid(8);

    NanitePackedAsset first;
    NanitePackedAsset second;
    REQUIRE(BuildAsset(mesh, first));
    REQUIRE(BuildAsset(mesh, second));

    CHECK(first.bytes.size() == second.bytes.size());
    CHECK(std::memcmp(first.bytes.data(), second.bytes.data(), first.bytes.size()) == 0);

    NaniteFileWriteOptions options;
    options.settingsHash = 0x1234u;
    options.sourceHash   = 0x5678u;
    options.extSections.push_back({ NaniteExtSectionType::MaterialDesc, MakeMaterialDescPayload(1) });

    std::vector<u8> firstBytes, secondBytes;
    REQUIRE(WriteNaniteFile(first, options, firstBytes));
    REQUIRE(WriteNaniteFile(second, options, secondBytes));
    CHECK(firstBytes.size() == secondBytes.size());
    CHECK(std::memcmp(firstBytes.data(), secondBytes.data(), firstBytes.size()) == 0);
}

// ============================================================
// 6. T5：字节 → 强类型资产（运行时离线加载的解析层）
// ============================================================

TEST_CASE("NaniteFileV2: ParseNaniteAsset 往返五段向量并取出元数据/材质描述") {
    NanitePackedAsset asset;
    REQUIRE(BuildAsset(MakeGrid(4), asset));

    NaniteFileWriteOptions options;
    options.builderVersion = kNaniteBuilderVersion;
    options.settingsHash   = 0xABCDEF01ull;
    options.sourceHash     = 0x11223344ull;
    options.extSections.push_back({ NaniteExtSectionType::MaterialDesc, MakeMaterialDescPayload(2) });

    std::vector<u8> bytes;
    REQUIRE(WriteNaniteFile(asset, options, bytes));

    NanitePackedAsset parsed;
    NaniteAssetLoadInfo info;
    REQUIRE(ParseNaniteAsset(bytes.data(), bytes.size(), kNaniteBuilderVersion, parsed, &info) ==
            NaniteAssetLoadStatus::Ok);

    // 五段向量逐元素相等（这是"离线读入 == 运行时构建"的解析层保证）
    REQUIRE(parsed.clusters.size()   == asset.clusters.size());
    REQUIRE(parsed.vertices.size()   == asset.vertices.size());
    REQUIRE(parsed.triangles.size()  == asset.triangles.size());
    REQUIRE(parsed.lodOffsets.size() == asset.lodOffsets.size());
    CHECK(std::memcmp(parsed.clusters.data(), asset.clusters.data(),
                      asset.clusters.size() * sizeof(NaniteClusterRecord)) == 0);
    CHECK(std::memcmp(parsed.vertices.data(), asset.vertices.data(),
                      asset.vertices.size() * sizeof(NaniteVertex)) == 0);
    CHECK(std::memcmp(parsed.triangles.data(), asset.triangles.data(),
                      asset.triangles.size() * sizeof(NanitePackedTriangle)) == 0);
    CHECK(std::memcmp(parsed.lodOffsets.data(), asset.lodOffsets.data(),
                      asset.lodOffsets.size() * sizeof(u32)) == 0);

    // 元数据与扩展段
    CHECK(info.formatVersion == kNaniteFileVersion);
    CHECK(info.builderVersion == kNaniteBuilderVersion);
    CHECK(info.settingsHash == options.settingsHash);
    CHECK(info.sourceHash == options.sourceHash);
    CHECK(info.hasHashes);
    REQUIRE(info.materialDescs.size() == 2u);
    CHECK(info.materialDescs[0].materialIndex == 0u);
    CHECK(info.materialDescs[1].materialIndex == 1u);
}

TEST_CASE("NaniteFileV2: ParseNaniteAsset 区分损坏 / 版本过期 / 构建器过期") {
    NanitePackedAsset asset;
    REQUIRE(BuildAsset(MakeGrid(2), asset));
    NaniteFileWriteOptions options;
    std::vector<u8> bytes;
    REQUIRE(WriteNaniteFile(asset, options, bytes));

    NanitePackedAsset parsed;

    SUBCASE("魔数损坏 ⇒ BadFile") {
        std::vector<u8> broken = bytes;
        broken[0] = 'X';
        CHECK(ParseNaniteAsset(broken.data(), broken.size(), 0u, parsed) ==
              NaniteAssetLoadStatus::BadFile);
    }

    SUBCASE("格式版本超范围 ⇒ VersionMismatch") {
        std::vector<u8> broken = bytes;
        NaniteFileHeader header{};
        std::memcpy(&header, broken.data(), sizeof(header));
        header.version = kNaniteFileVersion + 1u;
        std::memcpy(broken.data(), &header, sizeof(header));
        CHECK(ParseNaniteAsset(broken.data(), broken.size(), 0u, parsed) ==
              NaniteAssetLoadStatus::VersionMismatch);
    }

    SUBCASE("构建器版本不一致 ⇒ BuilderMismatch") {
        CHECK(ParseNaniteAsset(bytes.data(), bytes.size(), kNaniteBuilderVersion + 1u, parsed) ==
              NaniteAssetLoadStatus::BuilderMismatch);
        // expectedBuilderVersion = 0 ⇒ 不校验
        CHECK(ParseNaniteAsset(bytes.data(), bytes.size(), 0u, parsed) == NaniteAssetLoadStatus::Ok);
    }
}