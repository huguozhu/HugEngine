// ============================================================
// Vfs.cpp —— 资产/可写目录解析
//
// 只依赖 std::filesystem：Windows 与鸿蒙（NDK 的 libc++ 提供 filesystem）通用。
// 平台差异只剩下"根目录是谁"——由各平台入口调用 SetRoots 决定。
// ============================================================

#include "Platform/IVfs.h"

#include "Core/Log.h"

#include <filesystem>
#include <system_error>

// Windows 构建由 CMake 注入（与旧的 HUGE_CONTENT_DIR 同值）；
// 鸿蒙构建不定义，根目录由 NativeEntry 在解压 rawfile 后设置。
#ifndef HUGENGINE_CONTENT_DIR
    #define HUGENGINE_CONTENT_DIR ""
#endif

namespace he::vfs {
namespace {

String g_Assets;
String g_Writable;

const char* DefaultAssetsRoot() {
    return HUGENGINE_CONTENT_DIR;
}

// 把相对路径挂到某个根下
String Join(const String& root, const String& relative) {
    std::filesystem::path p(relative);
    if (p.is_absolute()) return relative;          // 调用方已给绝对路径
    if (root.empty())    return relative;          // 还没设置根（鸿蒙启动早期）
    return (std::filesystem::path(root) / p).lexically_normal().string();
}

} // namespace

void SetRoots(const String& assetsRoot, const String& writableRoot) {
    if (!assetsRoot.empty())   g_Assets   = assetsRoot;
    if (!writableRoot.empty()) g_Writable = writableRoot;
    HE_CORE_INFO("[VFS] 资产根: {}", g_Assets.empty() ? "(未设置)" : g_Assets);
    HE_CORE_INFO("[VFS] 可写根: {}", g_Writable.empty() ? "(未设置)" : g_Writable);
}

void ResetRoots() {
    g_Assets.clear();
    g_Writable.clear();
    g_Assets = DefaultAssetsRoot();
    g_Writable = DefaultAssetsRoot();
}

const String& AssetsRoot()   { return g_Assets; }
const String& WritableRoot() { return g_Writable; }

String Resolve(const String& relative) {
    if (g_Assets.empty()) g_Assets = DefaultAssetsRoot();   // 首次使用时初始化
    return Join(g_Assets, relative);
}

String ResolveWritable(const String& relative) {
    if (g_Writable.empty()) g_Writable = DefaultAssetsRoot();
    const String full = Join(g_Writable, relative);
    // 父目录不存在就建（配置/日志的目录可能还没创建）
    std::error_code ec;
    std::filesystem::path parent = std::filesystem::path(full).parent_path();
    if (!parent.empty() && !std::filesystem::exists(parent, ec)) {
        std::filesystem::create_directories(parent, ec);
        if (ec) HE_CORE_WARN("[VFS] 创建目录失败: {} ({})", parent.string(), ec.message());
    }
    return full;
}

bool Exists(const String& relative) {
    std::error_code ec;
    return std::filesystem::exists(Resolve(relative), ec);
}

} // namespace he::vfs
