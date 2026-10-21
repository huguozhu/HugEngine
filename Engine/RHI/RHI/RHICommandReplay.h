#pragma once

#include "Core/Types.h"

// ============================================================
// RHICommandReplay —— 命令流 → RHI 调用的**翻译层**（方案 §12 的 A-1 另一半）
//
// 【分工】`RHICommandList`（`RHI/RHICommandList.h`）是**记录端**：渲染线程只往它里面 append
// 纯数据载荷（类型擦除 + 内联存储 + arena），**零 Vulkan 调用**。本文件是**执行端**：在 RHI 线程上
// 把命令流逐条翻译成 `IRHICommandList` 的调用（`SetPipeline`/`SetDescriptorSet`/`DrawIndexed`…）。
//
// 【为什么单独一个文件】`RHICommandList.h` 刻意 RHI-free（单测直接编译它）；把 `IRHICommandList`
// 拉进去会破坏那条纪律，也会让记录端的单测被迫链接 RHI/Vulkan。
//
// 【容错约定】未知类型、载荷长度不匹配、arena 越界 ⇒ **跳过该条并继续**（返回实际翻译的条数）。
// 执行端不允许因为一条坏命令就 abort：命令流是跨线程交接的数据，不是内存里的强类型对象。
// ============================================================

namespace he::rhi {

class RHICommandList;    // 记录端（纯数据）
class IRHICommandList;   // 执行端（后端命令列表）

/// 逐条翻译 `stream` 到 `cmd`（只在执行端/RHI 线程上调用）。
/// @return 成功翻译的命令条数（<= `stream.CommandCount()`）
u32 ReplayCommandStream(const RHICommandList& stream, IRHICommandList& cmd);

} // namespace he::rhi
