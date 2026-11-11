// ============================================================
//  Tools/MeshPreprocess/main.cpp
//
//  T3（《01.Nanite离线预处理与全Mesh管线方案》）：把 glTF 离线预处理成 `.nanite` v2。
//
//  用法：
//    MeshPreprocess <in.gltf> <out-dir> [--granularity=primitive|mesh]
//                   [--fallback-ratio=0.02] [--stats] [--verbose]
//
//  【当前阶段（T3）的边界】
//   · 按 primitive 出资产：一份 `.nanite` = 一个 glTF primitive（方案 §2 决定 0）；
//   · 材质只写 `MaterialDesc` 扩展段（bindless 字段留 0，运行时翻译，方案 §3.3）；
//   · `--granularity=mesh`（把一个 mesh 的多个 primitive 合并成一份资产）留到 T4；
//   · `--fallback-ratio` 目前只解析不消费（FallbackMesh 段在 T4 生成）。
//
//  【默认参数直接取自运行时常量】保证"默认参数下离线与运行时产物逐字节相等"：
//   簇大小 = `kNaniteMaxClusterTriangles`、顶点上限 = `kNaniteMaxClusterVertices`。
// ============================================================

#include "Nanite/NaniteTypes.h"
#include "Nanite/NaniteUpload.h"
#include "Nanite/NaniteGltfReader.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
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
    bool        valid = false;
    std::string error;
};

void PrintUsage() {
    std::printf(
        "用法: MeshPreprocess <in.gltf> <out-dir> [选项]\n"
        "  --granularity=primitive|mesh   资产粒度（T3 仅支持 primitive）\n"
        "  --fallback-ratio=0.02          Fallback 低模比例（T4 起生效）\n"
        "  --stats                        打印每份资产的统计\n"
        "  --verbose                      打印更多细节\n");
}

Options ParseArgs(int argc, char** argv) {
    Options options;
    std::vector<std::string> positional;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--stats") { options.stats = true; continue; }
        if (arg == "--verbose") { options.verbose = true; continue; }
        if (arg.rfind("--granularity=", 0) == 0) { options.granularity = arg.substr(14); continue; }
        if (arg.rfind("--fallback-ratio=", 0) == 0) {
            options.fallbackRatio = (float)std::atof(arg.substr(17).c_str());
            continue;
        }
        if (arg == "--help" || arg == "-h") { PrintUsage(); return options; }
        if (arg.rfind("--", 0) == 0) { options.error = "未知参数: " + arg; return options; }
        positional.push_back(arg);
    }
    if (positional.size() != 2u) {
        options.error = "需要两个位置参数：<in.gltf> <out-dir>";
        return options;
    }
    options.inputPath = positional[0];
    options.outputDir = positional[1];
    options.valid     = true;
    return options;
}

/// 把一段内存原样写成文件（工具侧直接用 ofstream：这是桌面 CLI，不需要 IVfs）
bool WriteAllBytes(const std::string& path, const std::vector<u8>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(bytes.data()), (std::streamsize)bytes.size());
    return out.good();
}

/// 构建参数哈希：T3 只有"簇大小/顶点上限/量化位数"这些**格式常量**参与。
/// T4 加入简化比例/cone 权重后仍在同一处扩展（改它必须同步 `kNaniteBuilderVersion`）。
u64 ComputeSettingsHash() {
    u64 hash = 0xCBF29CE484222325ull;
    auto mix = [&hash](const void* data, usize bytes) {
        const u8* p = static_cast<const u8*>(data);
        for (usize i = 0; i < bytes; ++i) { hash ^= (u64)p[i]; hash *= 0x100000001B3ull; }
    };
    const u32 clusterTriangles = kNaniteMaxClusterTriangles;
    const u32 clusterVertices  = kNaniteMaxClusterVertices;
    mix(&clusterTriangles, sizeof(clusterTriangles));
    mix(&clusterVertices, sizeof(clusterVertices));
    return hash;
}

} // namespace

int main(int argc, char** argv) {
    const Options options = ParseArgs(argc, argv);
    if (!options.valid) {
        if (!options.error.empty()) std::fprintf(stderr, "错误: %s\n", options.error.c_str());
        PrintUsage();
        return 1;
    }
    if (options.granularity != "primitive") {
        std::fprintf(stderr, "错误: --granularity=%s 暂不支持（T3 只支持 primitive，mesh 粒度见 T4）\n",
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
    if (options.verbose) {
        std::printf("glTF: %s\n  primitive=%zu material=%zu 三角形=%llu 源哈希=0x%016llX\n",
                    options.inputPath.c_str(), scene.primitives.size(), scene.materials.size(),
                    (unsigned long long)scene.TotalTriangleCount(),
                    (unsigned long long)scene.sourceHash);
    }

    // ── 2. 准备输出目录 ──
    std::error_code ec;
    std::filesystem::create_directories(options.outputDir, ec);
    if (ec) {
        std::fprintf(stderr, "错误: 无法创建输出目录 %s（%s）\n", options.outputDir.c_str(),
                     ec.message().c_str());
        return 1;
    }

    const std::string stem = std::filesystem::path(options.inputPath).stem().string();
    const u64 settingsHash = ComputeSettingsHash();

    // 材质描述段：整份 glTF 的材质表（与 primitive 的 materialIndex 对应）
    std::vector<u8> materialDescBytes(sizeof(NaniteMaterialDesc) * scene.materials.size());
    for (usize i = 0; i < scene.materials.size(); ++i) {
        std::memcpy(materialDescBytes.data() + i * sizeof(NaniteMaterialDesc),
                    &scene.materials[i].desc, sizeof(NaniteMaterialDesc));
    }

    // ── 3. 逐 primitive 构建并写文件 ──
    u32 written = 0;
    u32 failed  = 0;
    for (const NaniteGltfPrimitive& primitive : scene.primitives) {
        const std::vector<NaniteMaterialRecord> materials;   // 运行时材质段由 T5 回填
        NanitePackedAsset asset;
        if (!BuildNaniteAssetFromGeometry(primitive.positions, primitive.normals, primitive.uvs,
                                          primitive.indices, materials, asset)) {
            std::fprintf(stderr, "警告: primitive %s 构建失败，已跳过\n", primitive.name.c_str());
            ++failed;
            continue;
        }

        // 扩展段：材质描述 + 簇 → 源 primitive 归属
        NaniteSourceClusterRange range;
        range.firstCluster   = 0u;
        range.clusterCount   = (u32)asset.clusters.size();
        range.primitiveIndex = primitive.primitiveIndex;
        range.materialIndex  = primitive.materialIndex;
        std::vector<u8> rangeBytes(sizeof(NaniteSourceClusterRange));
        std::memcpy(rangeBytes.data(), &range, sizeof(range));

        NaniteFileWriteOptions writeOptions;
        writeOptions.builderVersion = kNaniteBuilderVersion;
        writeOptions.settingsHash   = settingsHash;
        writeOptions.sourceHash     = scene.sourceHash;
        if (!materialDescBytes.empty()) {
            writeOptions.extSections.push_back(
                { NaniteExtSectionType::MaterialDesc, materialDescBytes });
        }
        writeOptions.extSections.push_back({ NaniteExtSectionType::SourceMeshRange, rangeBytes });

        std::vector<u8> fileBytes;
        if (!WriteNaniteFile(asset, writeOptions, fileBytes)) {
            std::fprintf(stderr, "警告: primitive %s 写文件失败（段表不自洽），已跳过\n",
                         primitive.name.c_str());
            ++failed;
            continue;
        }

        const std::string fileName = stem + "_" + std::to_string(primitive.meshIndex) + "_" +
                                     std::to_string(primitive.primitiveIndex) + ".nanite";
        const std::string fullPath = (std::filesystem::path(options.outputDir) / fileName).string();
        if (!WriteAllBytes(fullPath, fileBytes)) {
            std::fprintf(stderr, "错误: 无法写入 %s\n", fullPath.c_str());
            ++failed;
            continue;
        }
        ++written;

        if (options.stats) {
            std::printf("  %-28s 簇=%-6zu 顶点=%-7zu 三角形=%-7zu 文件=%8.1f KiB\n",
                        fileName.c_str(), asset.clusters.size(), asset.vertices.size(),
                        asset.triangles.size(), (double)fileBytes.size() / 1024.0);
        } else if (options.verbose) {
            std::printf("  写出 %s（%zu 字节）\n", fullPath.c_str(), fileBytes.size());
        }
    }

    std::printf("MeshPreprocess: %s → %s：成功 %u 份，失败 %u 份（primitive 总数 %zu）\n",
                options.inputPath.c_str(), options.outputDir.c_str(), written, failed,
                scene.primitives.size());
    return failed == 0u ? 0 : 2;
}
