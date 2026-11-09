#pragma once

// ============================================================
// IVfs.h —— 资产/可写目录的平台无关解析
//
// 【为什么需要】样例原来直接拼 `HUGE_CONTENT_DIR`（编译期绝对路径，如
// D:/Source/HugEngine/Content/）。鸿蒙上：
//   · 只读资产在 rawfile 里，运行时解压到应用沙箱（路径未知，不能编译期写死）；
//   · 配置 / 日志 / imgui.ini 必须写进沙箱的可写目录，不能写进只读资产目录。
// 因此把"相对路径 → 绝对路径"收敛成两个根：
//   AssetsRoot   —— 只读资产（glTF / 纹理 / HDR）
//   WritableRoot —— 可写文件（Config / 日志 / ini）
// Windows 上两者都默认是 Content/（与改动前行为一致）；鸿蒙上由入口在解压后设置。
// ============================================================

#include "Core/Types.h"

namespace he::vfs {

/// 设置资产根与可写根（空字符串表示"不设置"，保持默认）。
/// Windows 默认由编译宏 HUGENGINE_CONTENT_DIR 提供；鸿蒙必须在启动时调用。
void SetRoots(const String& assetsRoot, const String& writableRoot);

/// 恢复成编译期默认根（测试/工具用；运行时一般不调用）
void ResetRoots();

const String& AssetsRoot();
const String& WritableRoot();

/// 只读资产：相对路径 → 绝对路径（已是绝对路径则原样返回）
String Resolve(const String& relative);

/// 可写文件：相对路径 → 绝对路径（父目录不存在时自动创建）
String ResolveWritable(const String& relative);

/// 只读资产是否存在（Windows 上是文件系统检查；鸿蒙上取决于解压结果）
bool Exists(const String& relative);

} // namespace he::vfs
