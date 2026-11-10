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

} // namespace he::android
