// ============================================================

#include "Core/Platform.h"   // HE_PLATFORM_*：必须在使用 #if 之前可见
// RawfileUnpack.cpp —— 把 rawfile 里的资产解压到应用沙箱

//

// 为什么不用 ArkTS 侧解压：

//   rawfile 是打包进 hap 的只读资源，ArkTS 侧能读但需要逐层遍历 + 写文件；

//   放在 native 侧可以直接用 NDK 的 RawFile API 递归拷贝，且与 VFS 的根目录设置

//   在同一处完成（vfs::SetRoots），时序清楚。

// ============================================================


#include "RawfileUnpack.h"

#if HE_PLATFORM_OHOS

#include "Core/Log.h"

#include <rawfile/raw_file.h>
#include <rawfile/raw_file_manager.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace he::ohos {
namespace {

namespace fs = std::filesystem;

/// 单个 rawfile → 沙箱的拷贝

bool CopyRawFile(NativeResourceManager* mgr, const std::string& rawPath, const fs::path& outPath) {
    RawFile* file = OH_ResourceManager_OpenRawFile(mgr, rawPath.c_str());
    if (!file) return false;

    const long size = OH_ResourceManager_GetRawFileSize(file);
    std::vector<char> buffer(static_cast<size_t>(size > 0 ? size : 0));

    long read = 0;
    if (size > 0) {
        read = OH_ResourceManager_ReadRawFile(file, buffer.data(), static_cast<size_t>(size));
    }
    OH_ResourceManager_CloseRawFile(file);

    if (read != size) {
        HE_CORE_WARN("[OHOS] 读取 rawfile 不完整: {}（{}/{}）", rawPath, read, size);
        return false;
    }

    std::ofstream out(outPath, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        HE_CORE_ERROR("[OHOS] 写沙箱失败: {}", outPath.string());
        return false;
    }
    if (!buffer.empty()) out.write(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    return out.good();
}

/// 递归解压 directory 下所有条目

void UnpackDir(NativeResourceManager* mgr, const std::string& rawDir, const fs::path& outDir) {
    RawDir* dir = OH_ResourceManager_OpenRawDir(mgr, rawDir.c_str());
    if (!dir) {
        HE_CORE_WARN("[OHOS] 打开 rawfile 目录失败: {}", rawDir);
        return;
    }

    const int count = OH_ResourceManager_GetRawFileCount(dir);
    for (int i = 0; i < count; ++i) {
        const char* name = OH_ResourceManager_GetRawFileName(dir, i);
        if (!name) continue;

        const std::string rawChild = rawDir + "/" + name;
        const fs::path outChild    = outDir / name;

        // 先试探"是目录"：能打开成 RawDir 就走递归
        if (RawDir* sub = OH_ResourceManager_OpenRawDir(mgr, rawChild.c_str())) {
            OH_ResourceManager_CloseRawDir(sub);
            std::error_code ec;
            fs::create_directories(outChild, ec);
            UnpackDir(mgr, rawChild, outChild);
        } else {
            std::error_code ec;
            fs::create_directories(outDir, ec);
            CopyRawFile(mgr, rawChild, outChild);
        }
    }
    OH_ResourceManager_CloseRawDir(dir);
}

} // namespace

bool UnpackRawfileToSandbox(NativeResourceManager* mgr, const std::string& rawDir, const std::string& sandboxDir) {
    if (!mgr) {
        HE_CORE_WARN("[OHOS] 没有 ResourceManager：跳过 rawfile 解压（资产将不可用）");
        return false;
    }
    std::error_code ec;
    fs::create_directories(sandboxDir, ec);
    HE_CORE_INFO("[OHOS] 解压 rawfile '{}' → '{}'", rawDir, sandboxDir);
    UnpackDir(mgr, rawDir, sandboxDir);
    return true;
}

} // namespace he::ohos

#endif // HE_PLATFORM_OHOS
