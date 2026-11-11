// ============================================================
// AssetUnpack.cpp —— APK assets/content → filesDir/content（递归拷贝）
//
// AAssetManager 的目录遍历是"一次取名字列表"的接口（AAssetManager_openDir +
// AAssetDir_getNextFileName），子目录需要自己拼路径递归进去。这里按这个模型实现，
// 与鸿蒙的 UnpackRawfileToSandbox 一一对应。
// ============================================================

#include "AssetUnpack.h"

#include "Core/Log.h"

#include <cstdio>
#include <filesystem>       // 建沙箱目录
#include <fstream>
#include <memory>
#include <sys/types.h>      // off64_t（AAsset_getLength64 的返回类型）
#include <vector>

namespace he::android {
namespace {

/// 递归解压一个目录；返回失败文件数
int UnpackDir(AAssetManager* assets, const std::string& assetPath, const std::string& destDir) {
    int failures = 0;

    AAssetDir* dir = AAssetManager_openDir(assets, assetPath.c_str());
    if (!dir) {
        HE_CORE_WARN("[Android] assets 目录打开失败: {}", assetPath);
        return 1;
    }

    // 先把名字收全：AAssetDir_getNextFileName 返回的是内部指针，
    // 递归时会破坏当前迭代器，必须先拷贝出来。
    std::vector<std::string> names;
    while (const char* name = AAssetDir_getNextFileName(dir)) {
        names.emplace_back(name);
    }
    AAssetDir_close(dir);

    for (const std::string& name : names) {
        const std::string assetFile = assetPath.empty() ? name : (assetPath + "/" + name);
        const std::string destFile  = destDir + "/" + name;

        // 尝试当文件打开；失败说明是子目录，递归进去
        AAsset* file = AAssetManager_open(assets, assetFile.c_str(), AASSET_MODE_BUFFER);
        if (!file) {
            std::filesystem::create_directories(destFile);
            failures += UnpackDir(assets, assetFile, destFile);
            continue;
        }

        const off64_t size = AAsset_getLength64(file);
        std::vector<char> buffer(static_cast<size_t>(size));
        const int read = AAsset_read(file, buffer.data(), buffer.size());
        AAsset_close(file);

        if (read != static_cast<int>(size)) {
            HE_CORE_ERROR("[Android] 读取 assets 失败: {}（{}/{}）", assetFile, read, size);
            ++failures;
            continue;
        }

        std::ofstream out(destFile, std::ios::binary | std::ios::trunc);
        if (!out) {
            HE_CORE_ERROR("[Android] 写入沙箱失败: {}", destFile);
            ++failures;
            continue;
        }
        out.write(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    }

    return failures;
}

} // namespace

bool UnpackAssetsToSandbox(AAssetManager* assets, const char* assetDir,
                           const std::string& destDir, const char* version) {
    if (!assets || !assetDir) {
        HE_CORE_ERROR("[Android] 解压资产参数非法");
        return false;
    }

    std::filesystem::create_directories(destDir);

    const int failures = UnpackDir(assets, assetDir, destDir);
    if (failures != 0) {
        HE_CORE_ERROR("[Android] 解压资产有 {} 个文件失败: {}", failures, destDir);
        return false;
    }

    // 版本文件最后写：中途崩溃时下次会重新解压
    std::ofstream vf(destDir + "/.unpack_version", std::ios::trunc);
    if (vf) vf << (version ? version : "0");

    HE_CORE_INFO("[Android] assets/{} → {} 解压完成（版本 {}）", assetDir, destDir,
                 version ? version : "0");
    return true;
}


// ============================================================
// 按清单解包（移动端主路径）
//
// AAssetManager 的目录枚举（AAssetDir_getNextFileName）只列文件、不列子目录，
// 逐目录递归在深层结构上会静默漏文件。清单由构建期脚本生成，内容是相对
// assets 根的路径（content/...），这里逐行打开并写到 destRoot 下同名位置。
// ============================================================
bool UnpackAssetsFromManifest(AAssetManager* assets, const char* manifestAssetPath,
                              const std::string& destRoot) {
    if (!assets || !manifestAssetPath) {
        HE_CORE_ERROR("[Android] 清单解包参数非法");
        return false;
    }

    AAsset* manifest = AAssetManager_open(assets, manifestAssetPath, AASSET_MODE_BUFFER);
    if (!manifest) {
        HE_CORE_ERROR("[Android] 资产清单不存在: {}", manifestAssetPath);
        return false;
    }
    const off64_t size = AAsset_getLength64(manifest);
    std::string text(static_cast<size_t>(size), '\0');
    const int read = AAsset_read(manifest, text.data(), text.size());
    AAsset_close(manifest);
    if (read != static_cast<int>(size)) {
        HE_CORE_ERROR("[Android] 读取资产清单失败: {}/{}", read, size);
        return false;
    }

    int copied  = 0;
    int failures = 0;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string rel = text.substr(pos, end - pos);
        pos = end + 1;
        if (!rel.empty() && rel.back() == '\r') rel.pop_back();
        if (rel.empty()) continue;

        AAsset* file = AAssetManager_open(assets, rel.c_str(), AASSET_MODE_BUFFER);
        if (!file) {
            HE_CORE_ERROR("[Android] 清单里的资产打不开: {}", rel);
            ++failures;
            continue;
        }

        const off64_t fileSize = AAsset_getLength64(file);
        std::vector<char> buffer(static_cast<size_t>(fileSize));
        const int fileRead = AAsset_read(file, buffer.data(), buffer.size());
        AAsset_close(file);
        if (fileRead != static_cast<int>(fileSize)) {
            HE_CORE_ERROR("[Android] 读取资产失败: {}（{}/{}）", rel, fileRead, fileSize);
            ++failures;
            continue;
        }

        const std::string dest = destRoot + "/" + rel;
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(dest).parent_path(), ec);
        std::ofstream out(dest, std::ios::binary | std::ios::trunc);
        if (!out) {
            HE_CORE_ERROR("[Android] 写入沙箱失败: {}", dest);
            ++failures;
            continue;
        }
        out.write(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        ++copied;
    }

    HE_CORE_INFO("[Android] 按清单解包完成: {} 个文件{}", copied,
                 failures ? "（有失败）" : "");
    return failures == 0;
}

} // namespace he::android
