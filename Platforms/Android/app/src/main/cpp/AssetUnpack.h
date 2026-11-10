#pragma once

#include <android/asset_manager.h>

#include <string>

// ============================================================
// AssetUnpack.h —— APK 内 assets/content → 应用私有目录（沙箱）
//
// 为什么不在 native 里直接读 assets：
//   引擎的 VFS 是"文件路径"语义（glTF 里还有相对路径、stb 要读整文件），
//   而 AAssetManager 只能顺序读，且不支持目录遍历式的随机访问。
//   与鸿蒙 rawfile 的做法保持一致：启动时一次性解压到 filesDir/content。
//
// 【版本标记】只看目录是否存在会导致换资产后沙箱不更新，因此写 `.unpack_version`，
// 版本号与代码里的 kContentVersion 不一致就重解压。
// ============================================================

namespace he::android {

/// 把 assets/content 递归解压到 destDir（并写版本文件）。返回 false 表示有文件失败。
bool UnpackAssetsToSandbox(AAssetManager* assets, const char* assetDir,
                           const std::string& destDir, const char* version);

} // namespace he::android
