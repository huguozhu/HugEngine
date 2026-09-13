#pragma once

// ============================================================
// 06.GILab/CrashHandler.h — 崩溃处理器（诊断基建，对应计划 Wave 0.8）
//
// 目的：把"偶发崩溃只能靠 WER 的错误偏移反查"变成"崩溃时直接打出完整调用栈"。
//
// 背景：06.GILab 曾出现约 1/55 的偶发访问违例（0xC0000005），WER 只给出
// 模块内偏移（0x38216b），需要手工用 /MAP 反查才定位到
// VulkanTexture::GetImageView()。有了本处理器，下次崩溃可直接读到
// "函数名 + 源文件:行号" 的调用栈，定位成本从"猜"变成"读"。
//
// 同时会写出 minidump（.dmp），可用 Visual Studio 直接打开查看崩溃现场。
//
// 注意：处理器在记录完成后返回 EXCEPTION_CONTINUE_SEARCH，**故意让系统默认
// 处理（WER）继续执行**——这样既拿到我们自己的调用栈，也保留 WER 的
// APPCRASH 事件记录，两种证据都不丢。
// ============================================================

namespace he::sample {

/// 安装崩溃处理器
/// logPath：崩溃日志路径（追加写入）；传 nullptr 表示只输出到 stdout
/// dumpPath：minidump 输出路径；传 nullptr 表示不写 dump
void InstallCrashHandler(const char* logPath, const char* dumpPath);

} // namespace he::sample
