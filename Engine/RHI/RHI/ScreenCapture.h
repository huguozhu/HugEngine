#pragma once

// ============================================================
// RHI/ScreenCapture.h — 交换链按帧截图（D3D12 / Vulkan 共用）
//
// 【为什么要抽到 RHI 层】截图原先只写在 `D3D12SwapChain.cpp` 里（WIC 写 PNG +
// 自己解析 `HE_CAPTURE_FRAME`），Vulkan 后端没有对应通路，导致"两后端同帧对比"
// 的验收做不了。这里把**与后端无关**的三件事集中到一处：
//   ① 解析 `HE_CAPTURE_FRAME`（第 N 帧截图，<=0 关闭）；
//   ② 生成默认输出路径（<exe 目录>/capture_frameN.png，两后端文件名严格一致）；
//   ③ 把 RGBA8 像素写成 PNG（WIC，无第三方依赖）。
// 各后端只负责"把自己后缓冲/交换链图像的内容读回成 RGBA8 像素"这一段，
// 读回方式按图形 API 各自实现（见 D3D12SwapChain / VulkanSwapChain 的 CaptureIfRequested）。
// ============================================================

#include "RHI/Types.h"

#include <string>

namespace he::rhi {

/// 解析环境变量 `HE_CAPTURE_FRAME` 指定的截图帧号：n > 0 表示第 n 帧截图，其余（含未设置）表示不截图。
/// 只读一次环境变量并在进程内缓存；各后端各自打印自己的日志文案（保持原有日志）。
int CaptureFrameFromEnv();

/// 默认截图路径：`<exe 所在目录>/capture_frame<frameIndex>.png`（UTF-8）。
/// 【与 D3D12 原实现的关系】原实现直接用相对文件名 `capture_frameN.png`（= 当前工作目录），
/// 而样例的启动方式就是 `cd build\bin\Debug` 后运行 ⇒ 两者指向同一个文件；这里显式解析
/// exe 目录，从别处启动时也仍落在同一处，且两个后端不会因 CWD 不同而写散。
std::string DefaultCapturePath(u64 frameIndex);

/// 把 RGBA8 像素（每像素 4 字节、行紧排、无行填充）写成 PNG。成功返回 true。
/// 失败时打印一条错误日志（不抛异常）。
bool SaveRgbaToPng(const char* path, const unsigned char* rgba, u32 width, u32 height);

} // namespace he::rhi
