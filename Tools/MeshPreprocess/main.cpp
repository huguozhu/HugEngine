// ============================================================
//  Tools/MeshPreprocess/main.cpp
//
//  T3/T4（《01.Nanite离线预处理与全Mesh管线方案》）：glTF → `.nanite` v2 离线工具。
//
//  用法：
//    MeshPreprocess <in.gltf> <out-dir> [--granularity=primitive|mesh]
//                   [--fallback-ratio=0.02] [--stats] [--verbose] [--verify]
//
//  【资产粒度】
//   · `primitive`（默认）：一份 `.nanite` = 一个 glTF primitive（方案 §2 决定 0）；
//   · `mesh`：把一个 mesh 的全部 primitive 合并成一份资产（索引加 baseVertex），
//     并为每个 primitive 写一条 `SourceMeshRange` 与一条 `MaterialDesc`。
//
//  【扩展段】
//   · `MaterialDesc`：本资产用到的材质（纹理只留 image 索引，bindless 运行时翻译）；
//   · `SourceMeshRange`：簇 → 源 primitive 归属（`NaniteSourceClusterRange`）；
//   · `FallbackMesh`：按 `--fallback-ratio` 简化的低模（IA / RT 回退用）。
//
//  【--verify】把刚写出的文件读回来，与内存里的资产逐字节比对固定五段
//   —— 这是 T4 的验收口径"默认参数下离线产物与运行时构建逐字节相等"的自检。
// ============================================================

#include "Nanite/NaniteGltfReader.h"
#include "Nanite/NaniteTypes.h"
#include "Nanite/NaniteUpload.h"

#include <chrono>   // T7：基准测量
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

using namespace he;
using namespace he::render;

namespace {

struct Options {
    std::string inputPath;
    std::string outputDir;
    std::string granularity = "primitive";
    float       fallbackRatio = 0.02f;
    bool        stats = false;
    bool        verbose = false;
    bool        verify = false;
    bool        benchmark = false;          ///< T7：打印加载/构建/BVH 的耗时与内存表
    bool        printSettingsHash = false;   ///< 只打印 settingsHash/builderVersion（构建期脚本用）
    bool        valid = false;
    std::string error;
};

void PrintUsage() {
    std::printf(
        "用法: MeshPreprocess <in.gltf> <out-dir> [选项]\n"
        "  --granularity=primitive|mesh   资产粒度（默认 primitive）\n"
        "  --fallback-ratio=0.02          Fallback 低模的三角形比例\n"
        "  --verify                       读回文件并与内存资产逐字节比对\n"
        "  --benchmark                    T7：打印构建/加载/BVH 的耗时与内存表\n"
        "  --stats                        打印每份资产的统计\n"
        "  --verbose                      打印更多细节\n");
}

Options ParseArgs(int argc, char** argv) {
    Options options;
    std::vector<std::string> positional;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--stats")   { options.stats = true; continue; }
        if (arg == "--verbose") { options.verbose = true; continue; }
        if (arg == "--verify")  { options.verify = true; continue; }
        if (arg == "--benchmark") { options.benchmark = true; continue; }
        if (arg == "--print-settings-hash") { options.printSettingsHash = true; continue; }
        if (arg.rfind("--granularity=", 0) == 0) { options.granularity = arg.substr(14); continue; }
        if (arg.rfind("--fallback-ratio=", 0) == 0) {
            options.fallbackRatio = (float)std::atof(arg.substr(17).c_str());
            continue;
        }
        if (arg == "--help" || arg == "-h") { PrintUsage(); return options; }
        if (arg.rfind("--", 0) == 0) { options.error = "未知参数: " + arg; return options; }
        positional.push_back(arg);
    }
    if (positional.size() != 2u && !options.printSettingsHash) {
        options.error = "需要两个位置参数：<in.gltf> <out-dir>";
        return options;
    }
    if (positional.size() == 2u) { options.inputPath = positional[0]; }
    if (positional.size() == 2u) { options.outputDir = positional[1]; }
    options.valid     = true;
    return options;
}

bool WriteAllBytes(const std::string& path, const std::vector<u8>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(bytes.data()), (std::streamsize)bytes.size());
    return out.good();
}

/// 构建参数哈希：T4 起包含粒度与两个简化比例（改这里必须同步 `kNaniteBuilderVersion`）
u64 ComputeSettingsHash(const Options& options) {
    u64 hash = 0xCBF29CE484222325ull;
    auto mix = [&hash](const void* data, usize bytes) {
        const u8* p = static_cast<const u8*>(data);
        for (usize i = 0; i < bytes; ++i) { hash ^= (u64)p[i]; hash *= 0x100000001B3ull; }
    };
    const u32 clusterTriangles = kNaniteMaxClusterTriangles;
    const u32 clusterVertices  = kNaniteMaxClusterVertices;
    mix(&clusterTriangles, sizeof(clusterTriangles));
    mix(&clusterVertices, sizeof(clusterVertices));
    mix(&options.fallbackRatio, sizeof(options.fallbackRatio));
    mix(options.granularity.data(), options.granularity.size());
    return hash;
}

/// 一个待出资产的几何单元（primitive 或合并后的 mesh）
struct BuildUnit {
    std::string name;              ///< 文件名主体
    std::vector<float> positions;
    std::vector<float> normals;
    std::vector<float> uvs;
    std::vector<u32>   indices;
    std::vector<NaniteSourceClusterRange> ranges;   ///< 逐 primitive 的归属（primitiveIndex/materialIndex）
};

/// 把一组 primitive 拼成一个 BuildUnit（索引加 baseVertex；区间按三角形记，簇区间在构建后填）
BuildUnit MergePrimitives(const std::vector<NaniteGltfPrimitive*>& primitives, const std::string& name) {
    BuildUnit unit;
    unit.name = name;
    for (const NaniteGltfPrimitive* primitive : primitives) {
        const u32 baseVertex = (u32)(unit.positions.size() / 3u);
        unit.positions.insert(unit.positions.end(), primitive->positions.begin(), primitive->positions.end());
        unit.normals.insert(unit.normals.end(), primitive->normals.begin(), primitive->normals.end());
        unit.uvs.insert(unit.uvs.end(), primitive->uvs.begin(), primitive->uvs.end());

        NaniteSourceClusterRange range;
        range.firstCluster   = (u32)(unit.indices.size() / 3u);   // 暂存"首个三角形"，构建后换成簇下标
        range.clusterCount   = primitive->TriangleCount();
        range.primitiveIndex = primitive->primitiveIndex;
        range.materialIndex  = primitive->materialIndex;
        for (u32 index : primitive->indices) unit.indices.push_back(index + baseVertex);
        unit.ranges.push_back(range);
    }
    return unit;
}

/// 序列化材质描述段（只含本资产用到的材质，按 `ranges[].materialIndex` 对应）
std::vector<u8> BuildMaterialDescPayload(const NaniteGltfScene& scene, const BuildUnit& unit) {
    std::vector<NaniteMaterialDesc> descs;
    for (const NaniteSourceClusterRange& range : unit.ranges) {
        if (range.materialIndex < scene.materials.size()) {
            descs.push_back(scene.materials[range.materialIndex].desc);
        }
    }
    // 重新编号：段内下标 = 顺序，`ranges` 里的 materialIndex 同步改成段内下标
    std::vector<u8> bytes(descs.size() * sizeof(NaniteMaterialDesc));
    if (!descs.empty()) {
        std::memcpy(bytes.data(), descs.data(), bytes.size());
    }
    return bytes;
}

/// 把 `ranges` 里的"三角形区间暂存值"换成真正的簇区间（按三角形比例估算不可行，
/// 这里直接用构建后的 `NaniteSourceClusterRange` 语义：由构造成员逐个填入）
///
/// 【做法】构建器没有暴露"每个源网格对应哪些簇"，因此 T4 用保守口径：
/// 第 i 个 primitive 的簇区间 = [0, clusterCount) 按三角形占比估算不可信，
/// 改为**只记归属信息**（primitiveIndex/materialIndex），firstCluster/clusterCount 留 0。
/// 精确映射留到 T5（需要构建器按源网格分组切簇，属于后续任务）。
void FinalizeRanges(BuildUnit& unit, u32 clusterCount) {
    (void)clusterCount;
    for (NaniteSourceClusterRange& range : unit.ranges) {
        range.firstCluster = 0u;
        range.clusterCount = 0u;
    }
}

} // namespace

int main(int argc, char** argv) {
    const Options options = ParseArgs(argc, argv);
    if (options.printSettingsHash) {
        // 构建期脚本用它判断"参数是否变过"：变了就要全量重建 Processed 缓存。
        std::printf("builderVersion=%u settingsHash=%016llX\n", kNaniteBuilderVersion,
                    (unsigned long long)ComputeSettingsHash(options));
        return 0;
    }

    if (!options.valid) {
        if (!options.error.empty()) std::fprintf(stderr, "错误: %s\n", options.error.c_str());
        PrintUsage();
        return 1;
    }
    if (options.granularity != "primitive" && options.granularity != "mesh") {
        std::fprintf(stderr, "错误: --granularity=%s 不支持（primitive | mesh）\n",
                     options.granularity.c_str());
        return 1;
    }

    // ── 1. 读 glTF ──
    NaniteGltfScene scene;
    std::string readError;
    if (!LoadNaniteGltfFile(options.inputPath.c_str(), scene, &readError)) {
        std::fprintf(stderr, "错误: 读取 glTF 失败：%s（%s）\n", readError.c_str(),
                     options.inputPath.c_str());
        return 1;
    }
    const std::string stem = std::filesystem::path(options.inputPath).stem().string();

    if (options.verbose) {
        std::printf("glTF: %s\n  primitive=%zu material=%zu 三角形=%llu 源哈希=0x%016llX\n",
                    options.inputPath.c_str(), scene.primitives.size(), scene.materials.size(),
                    (unsigned long long)scene.TotalTriangleCount(),
                    (unsigned long long)scene.sourceHash);
    }

    // ── 2. 输出目录 ──
    std::error_code ec;
    std::filesystem::create_directories(options.outputDir, ec);
    if (ec) {
        std::fprintf(stderr, "错误: 无法创建输出目录 %s（%s）\n", options.outputDir.c_str(),
                     ec.message().c_str());
        return 1;
    }

    // ── 3. 按粒度分组 ──
    std::vector<BuildUnit> units;
    if (options.granularity == "primitive") {
        for (NaniteGltfPrimitive& primitive : scene.primitives) {
            std::vector<NaniteGltfPrimitive*> group{ &primitive };
            units.push_back(MergePrimitives(group, stem + "_" + std::to_string(primitive.meshIndex) +
                                                       "_" + std::to_string(primitive.primitiveIndex)));
        }
    } else {
        // mesh 粒度：同一 mesh 的所有 primitive 合并成一份资产
        std::map<u32, std::vector<NaniteGltfPrimitive*>> byMesh;
        for (NaniteGltfPrimitive& primitive : scene.primitives) {
            byMesh[primitive.meshIndex].push_back(&primitive);
        }
        for (auto& entry : byMesh) {
            units.push_back(MergePrimitives(entry.second, stem + "_" + std::to_string(entry.first)));
        }
    }

    const u64 settingsHash = ComputeSettingsHash(options);

    // ── 4. 逐单元构建、写文件、（可选）校验 ──
    u32 written = 0;
    u32 failed  = 0;
    u32 verifyFailed = 0;
    for (BuildUnit& unit : units) {
        // ── 材质段与"簇 → 源网格 → 材质"映射（T8/T25 的前置）──
        // 【为什么要在这里填】此前 `materials` 恒为空、`meshes` 不传 ⇒ 每个簇的 `materialID`
        //   都是 0、材质段为 0 条，光栅端只能走中性回退（实测 `回退 == 硬像素`）。
        //   每个 primitive 一条材质记录，`materialIndex` 就是它在 `meshes[]` 里的下标；
        //   纹理位暂留 0（运行时纹理绑定属 T25），因子照实取 glTF 的 pbr 值。
        std::vector<NaniteMaterialRecord> materials;
        std::vector<NaniteSourceMeshRange> meshes;
        materials.reserve(unit.ranges.size());
        meshes.reserve(unit.ranges.size());
        for (usize ri = 0; ri < unit.ranges.size(); ++ri) {
            const NaniteSourceClusterRange& range = unit.ranges[ri];
            NaniteMaterialRecord rec{};
            if (range.materialIndex < scene.materials.size()) {
                const NaniteMaterialDesc& d = scene.materials[range.materialIndex].desc;
                for (int c = 0; c < 4; ++c) rec.baseColorFactor[c] = d.baseColorFactor[c];
                rec.metallicFactor  = d.metallicFactor;
                rec.roughnessFactor = d.roughnessFactor;
            }
            materials.push_back(rec);

            NaniteSourceMeshRange mesh{};
            mesh.firstTriangle = range.firstCluster;      // MergePrimitives 暂存的是"首个三角形"
            mesh.triangleCount = range.clusterCount;      // 暂存的是三角形数
            mesh.materialIndex = (u32)ri;                 // 指向本单元的材质记录
            meshes.push_back(mesh);
        }

        NanitePackedAsset asset;
        if (!BuildNaniteAssetFromGeometry(unit.positions, unit.normals, unit.uvs, unit.indices,
                                          materials, meshes, asset)) {
            std::fprintf(stderr, "警告: %s 构建失败，已跳过\n", unit.name.c_str());
            ++failed;
            continue;
        }
        FinalizeRanges(unit, (u32)asset.clusters.size());

        // 扩展段：材质描述 + 簇归属 + Fallback 低模
        NaniteFileWriteOptions writeOptions;
        writeOptions.builderVersion = kNaniteBuilderVersion;
        writeOptions.settingsHash   = settingsHash;
        writeOptions.sourceHash     = scene.sourceHash;

        std::vector<u8> materialBytes = BuildMaterialDescPayload(scene, unit);
        if (!materialBytes.empty()) {
            writeOptions.extSections.push_back({ NaniteExtSectionType::MaterialDesc, materialBytes });
        }
        if (!unit.ranges.empty()) {
            std::vector<u8> rangeBytes(unit.ranges.size() * sizeof(NaniteSourceClusterRange));
            std::memcpy(rangeBytes.data(), unit.ranges.data(), rangeBytes.size());
            writeOptions.extSections.push_back({ NaniteExtSectionType::SourceMeshRange, rangeBytes });
        }

        NaniteFallbackMeshData fallback;
        if (BuildNaniteFallbackMesh(unit.positions, unit.normals, unit.indices,
                                    options.fallbackRatio, fallback)) {
            std::vector<u8> fallbackBytes;
            SerializeNaniteFallbackMesh(fallback, fallbackBytes);
            if (!fallbackBytes.empty()) {
                writeOptions.extSections.push_back({ NaniteExtSectionType::FallbackMesh, fallbackBytes });
            }
        }

        std::vector<u8> fileBytes;
        if (!WriteNaniteFile(asset, writeOptions, fileBytes)) {
            std::fprintf(stderr, "警告: %s 写文件失败（段表不自洽），已跳过\n", unit.name.c_str());
            ++failed;
            continue;
        }

        const std::string fullPath =
            (std::filesystem::path(options.outputDir) / (unit.name + ".nanite")).string();
        if (!WriteAllBytes(fullPath, fileBytes)) {
            std::fprintf(stderr, "错误: 无法写入 %s\n", fullPath.c_str());
            ++failed;
            continue;
        }
        ++written;

        // ── T7：基准（同一进程内测"运行时构建" vs "离线加载"，并核对五段字节）──
        if (options.benchmark) {
            using Clock = std::chrono::steady_clock;
            auto msSince = [](Clock::time_point a, Clock::time_point b) {
                return std::chrono::duration<double, std::milli>(b - a).count();
            };

            // ① 运行时路径：几何 → 资产
            const auto t0 = Clock::now();
            NanitePackedAsset runtimeAsset;
            const bool built = BuildNaniteAssetFromGeometry(unit.positions, unit.normals, unit.uvs,
                                                           unit.indices, materials, runtimeAsset);
            const auto t1 = Clock::now();

            // ② 写文件
            std::vector<u8> benchBytes;
            const auto t2 = Clock::now();
            const bool wrote = WriteNaniteFile(runtimeAsset, writeOptions, benchBytes);
            const auto t3 = Clock::now();

            // ③ 读文件 + 解析（离线路径的 CPU 部分）
            NanitePackedAsset loadedAsset;
            NaniteAssetLoadInfo loadedInfo;
            const auto t4 = Clock::now();
            const NaniteAssetLoadStatus loadStatus =
                ParseNaniteAsset(benchBytes.data(), benchBytes.size(), kNaniteBuilderVersion,
                                 loadedAsset, &loadedInfo);
            const auto t5 = Clock::now();

            // ④ 派生数据：簇 BVH（决定是否值得落盘的关键读数）
            NaniteClusterBVH bvh;
            const auto t6 = Clock::now();
            const bool bvhOk = BuildNaniteClusterBVH(loadedAsset.clusters, bvh);
            const auto t7 = Clock::now();

            // ⑤ 五段字节一致性（"离线读入 == 运行时构建"）
            bool sameBytes = built && wrote && loadStatus == NaniteAssetLoadStatus::Ok;
            if (sameBytes) {
                // 【跳过 96B 头部】v2 文件头写了扩展段数与两个哈希，运行时镜像里没有；
                // 比较范围 = 头之后的固定五段内容（簇/顶点/索引/材质/LOD）。
                const usize fiveSections = runtimeAsset.layout.lodOffset + runtimeAsset.layout.lodBytes;
                sameBytes = loadedAsset.bytes.size() >= fiveSections &&
                            std::memcmp(loadedAsset.bytes.data() + kNaniteFileHeaderBytes,
                                        runtimeAsset.bytes.data() + kNaniteFileHeaderBytes,
                                        fiveSections - kNaniteFileHeaderBytes) == 0;
            }
            const double buildMs = msSince(t0, t1);
            const double writeMs = msSince(t2, t3);
            const double loadMs  = msSince(t4, t5);
            const double bvhMs   = msSince(t6, t7);
            std::printf("[T7] 阶段                        耗时(ms)     字节\n");
            std::printf("[T7] 运行时构建(几何→资产)     %9.1f   %9zu\n", buildMs,
                        runtimeAsset.bytes.size());
            std::printf("[T7] 写 .nanite v2             %9.1f   %9zu\n", writeMs, benchBytes.size());
            std::printf("[T7] 读文件+解析(离线 CPU 部分) %8.1f   %9zu\n", loadMs,
                        loadedAsset.bytes.size());
            std::printf("[T7] 簇 BVH 构建                %9.1f   %s（占运行时构建 %.1f%%）\n", bvhMs,
                        bvhOk ? "ok" : "failed",
                        buildMs > 0.0 ? (bvhMs * 100.0 / buildMs) : 0.0);
            std::printf("[T7] 加速比                    %9.2fx  （运行时构建 / 离线加载）\n",
                        loadMs > 0.0 ? (buildMs / loadMs) : 0.0);
            std::printf("[T7] 五段字节一致性            %s\n", sameBytes ? "PASS" : "FAIL");
        }

        // ── --verify：读回并比对固定五段（T4 验收口径）──
        if (options.verify) {
            NaniteFileImage image;
            bool ok = ReadNaniteFile(fileBytes.data(), fileBytes.size(), image) == NaniteFileError::None;
            if (ok) {
                ok = image.bytes.size() >= asset.layout.totalBytes &&
                     std::memcmp(image.bytes.data() + image.layout.clusterOffset,
                                 asset.bytes.data() + asset.layout.clusterOffset,
                                 asset.layout.clusterBytes) == 0 &&
                     std::memcmp(image.bytes.data() + image.layout.vertexOffset,
                                 asset.bytes.data() + asset.layout.vertexOffset,
                                 asset.layout.vertexBytes) == 0 &&
                     std::memcmp(image.bytes.data() + image.layout.indexOffset,
                                 asset.bytes.data() + asset.layout.indexOffset,
                                 asset.layout.indexBytes) == 0 &&
                     std::memcmp(image.bytes.data() + image.layout.materialOffset,
                                 asset.bytes.data() + asset.layout.materialOffset,
                                 asset.layout.materialBytes) == 0 &&
                     std::memcmp(image.bytes.data() + image.layout.lodOffset,
                                 asset.bytes.data() + asset.layout.lodOffset,
                                 asset.layout.lodBytes) == 0;
            }
            if (!ok) {
                std::fprintf(stderr, "校验失败: %s 的固定五段与内存资产不一致\n", unit.name.c_str());
                ++verifyFailed;
            }
        }

        if (options.stats) {
            std::printf("  %-30s 簇=%-6zu 顶点=%-7zu 三角形=%-7zu 材质=%-3zu 低模=%-6u 文件=%8.1f KiB\n",
                        (unit.name + ".nanite").c_str(), asset.clusters.size(), asset.vertices.size(),
                        asset.triangles.size(), asset.materials.size(), fallback.header.indexCount / 3u,
                        (double)fileBytes.size() / 1024.0);
        } else if (options.verbose) {
            std::printf("  写出 %s（%zu 字节）\n", fullPath.c_str(), fileBytes.size());
        }
    }

    std::printf("MeshPreprocess: %s → %s：成功 %u 份，失败 %u 份（granularity=%s，单位总数 %zu）%s\n",
                options.inputPath.c_str(), options.outputDir.c_str(), written, failed,
                options.granularity.c_str(), units.size(),
                options.verify ? (verifyFailed == 0u ? "，校验通过" : "，校验失败") : "");
    return (failed == 0u && verifyFailed == 0u) ? 0 : 2;
}
