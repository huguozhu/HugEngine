#pragma once

#include "Core/Types.h"
#include "Threading/RenderCommandQueue.h"

// ============================================================
// RenderThread —— 渲染线程（阶段 0 为**壳**，方案 B §4.3 / T0.3、T0.4）
//
// 【阶段 0 的形态】不起线程：`PumpOnce/PumpAll` 在**调用线程**（= 游戏线程）上把待执行帧取出来顺序执行，
// 因此"产出 → 队列 → 消费者执行"这条路径与帧序都与改前一致（`cmp_dumps` 应逐像素相同）。
// 真正起线程是阶段 2：T2.1 增加线程主体与休眠策略，T2.2 把设备与交换链搬进去；
// **本类的公开接口在阶段 2 不变**，样例侧（T2.4）只需从"壳模式"切到"真线程模式"。
//
// 【为什么 `IsCurrent()` 现在恒为 true】阶段 0 还没有第二根线程，"当前线程就是渲染线程"，
// 于是 T0.1 的 `HE_ASSERT_RENDER_THREAD()` 语义不变（否则阶段 0 会满屏误报）。
// 阶段 2 起改为比较 `std::this_thread::get_id()`。
// ============================================================

namespace he::render {

class RenderThread {
public:
    explicit RenderThread(RenderCommandQueue& queue) : m_Queue(queue) {}

    /// 取出并执行**一帧**待处理命令；没有待处理帧时返回 false
    bool PumpOnce();

    /// 排空所有待处理帧（加载期 / 退出期用）；返回执行的帧数
    u32 PumpAll();

    /// 当前线程是否是渲染线程：阶段 0 恒为 true（见文件头说明）
    [[nodiscard]] static bool IsCurrent() { return true; }

    [[nodiscard]] u64 ExecutedFrameCount()   const { return m_ExecutedFrames; }
    [[nodiscard]] u64 ExecutedCommandCount() const { return m_ExecutedCommands; }

private:
    RenderCommandQueue& m_Queue;
    u64 m_ExecutedFrames   = 0;
    u64 m_ExecutedCommands = 0;
};

/// 帧节奏辅助（T0.4）：把"发布 → 执行 → 回收票据"收在一处，供样例（T2.4）与单测复用。
/// 阶段 0 的壳语义：发布后**立刻**在本线程执行并回收 ⇒ 在飞帧数永远是 0/1，不会触发背压；
/// 阶段 2 起由真线程异步回收，背压才真正生效（这正是 T0.4 要先把骨架和可观测计数做出来的原因）。
class FrameScheduler {
public:
    FrameScheduler(RenderCommandQueue& queue, RenderThread& renderThread)
        : m_Queue(queue), m_RenderThread(renderThread) {}

    /// 阻塞提交一帧（背压点在队列里），随后在壳模式下立即执行并回收票据。
    /// 返回该帧号。
    u64 SubmitAndPump();

    /// 非阻塞提交（在飞满时返回 false 并丢弃本帧），同样立即执行并回收。
    [[nodiscard]] bool TrySubmitAndPump();

private:
    RenderCommandQueue& m_Queue;
    RenderThread&       m_RenderThread;
};

} // namespace he::render
