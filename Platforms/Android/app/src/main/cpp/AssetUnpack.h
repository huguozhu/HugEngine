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
//
// 【为什么以清单为主】AAssetManager 的目录枚举只返回文件、不返回子目录，
// 递归解包在 `gltf/Sponza/glTF/...` 这类嵌套结构处会静默断掉（真机实测：
// files/content 解出来是空的）。所以构建期由 sync-assets.ps1 生成
// `asset_manifest.txt`（每行一个相对路径），运行期按清单逐个拷贝。
// ============================================================

namespace he::android {

/// 把 assets/content 递归解压到 destDir（并写版本文件）。返回 false 表示有文件失败。
/// 【注意】AAssetManager 的目录枚举不返回子目录，嵌套资产会漏；优先用下面的清单解包。
bool UnpackAssetsToSandbox(AAssetManager* assets, const char* assetDir,
                           const std::string& destDir, const char* version);

/// 按构建期生成的清单解包（清单每行是相对 assets 根的路径，如 content/gltf/...）。
/// 不依赖目录枚举，任意深度的资产都能拷出来。返回 false 表示有文件失败。
bool UnpackAssetsFromManifest(AAssetManager* assets, const char* manifestAssetPath,
                              const std::string& destRoot);

} // namespace he::android
