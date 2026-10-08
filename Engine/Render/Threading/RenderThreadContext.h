#pragma once

#include "Core/Types.h"

#include <cstdint>

// ============================================================
// RenderThreadContext —— 渲染线程上下文（渲染线程化方案 B §4.3 / 阶段 0 T0.3）
//
// 游戏线程入队的每条命令在**渲染线程**上执行时拿到这个上下文。它的定位是"渲染线程的身份 +
// 帧信息"，并且（T2.6 起）是**渲染层调用 RHI 的唯一出口** —— 命令载荷里不允许出现裸
// `IRHIDevice*`，否则将来把执行位置换到 RHI 线程（§12 附录 C）时要重写整个渲染层。
//
// 【本阶段为何没有 RHI 方法】阶段 0 的壳只验证"命令交接 + 帧票据 + 背压"，不改变执行位置，
// 因此上下文只携带帧信息；T2.6 会把设备/交换链的访问收口到这里（并保持
// `Engine/Render/Threading/*` 这一组文件 **RHI-free** 直到那一步，便于单测直接编译它们）。
// ============================================================

namespace he::render {

struct RenderThreadContext {
    u64 frameIndex = 0;   // 单调递增的帧号（渲染线程视角）
    u32 frameSlot  = 0;   // 帧槽位（0 .. kMaxFramesInFlight-1），用于按帧轮换的资源

    /// 该帧是否被标记为"本帧有实际渲染工作"（空帧可跳过剔除等重活）
    bool hasWork = true;
};

} // namespace he::render
