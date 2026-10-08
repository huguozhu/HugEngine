#pragma once

#include "Core/Types.h"
#include "Threading/RenderCommandQueue.h"

#include <atomic>
#include <cstddef>
#include <thread>

// ============================================================
// RenderThread —— 渲染线程（阶段 0 为**壳**，阶段 2 / T2.1 起可**真起线程**）
//
// 【两种形态，同一套接口】
//   · **壳模式**（`Start()` 未调用）：`PumpOnce/PumpAll` 在**调用线程**上执行，`IsCurrent()` 恒为 true。
//     阶段 0 的判据（28 个转储目标逐位一致、帧时间不退化）就是在壳模式下测的。
//   · **真线程模式**（`Start()` 之后）：由 `m_Thread` 里的 `ThreadMain()` 循环消费队列；`IsCurrent()`
//     比较线程 id；`Stop()` 先**排空**再 join（已提交的帧不能丢，否则游戏线程会永远等票据回收）。
//
// 【空闲策略（T2.1）】先按 `SetSpinWaitUs()` 自旋，然后短休眠（200us）轮询。
// 用条件变量唤醒更优雅，但那要队列暴露 cv（T2.6 的 `RenderThreadContext` 一起做）；此处先用休眠轮询，
// 并把 `spinWaitUs` 与 `EngineConfig::renderThreadSpinWaitUs` 对齐（0 = 不自旋，空闲不烧核）。
// ============================================================

namespace he::render {

class RenderThread {
public:
    explicit RenderThread(RenderCommandQueue& queue) : m_Queue(queue) {}
    ~RenderThread() { Stop(); }                       // 析构兜底：绝不留下未 join 的线程

    /// 启动渲染线程（幂等）。返回是否已在运行。
    bool Start();

    /// 停止：置退出标志 → 排空待处理帧 → join。幂等。
    void Stop();

    [[nodiscard]] bool IsRunning() const { return m_Running.load(std::memory_order_acquire); }

    /// 空闲自旋时长（微秒；0 = 不自旋，直接休眠）。与 `EngineConfig::renderThreadSpinWaitUs` 对应。
    void SetSpinWaitUs(u32 us) { m_SpinWaitUs.store(us, std::memory_order_relaxed); }

    /// 取出并执行**一帧**待处理命令；没有待处理帧时返回 false
    bool PumpOnce();

    /// 排空所有待处理帧（加载期 / 退出期用）；返回执行的帧数
    u32 PumpAll();

    /// 当前线程是否是渲染线程。
    /// 【两种形态】真线程模式：比较线程 id；**壳模式（未启动）恒为 true** —— 阶段 0 还没有第二根线程，
    /// "当前线程就是渲染线程"，因此 T0.1 的 `HE_ASSERT_RENDER_THREAD()` 语义在壳模式下不变。
    [[nodiscard]] static bool IsCurrent();

    [[nodiscard]] u64 ExecutedFrameCount()   const { return m_ExecutedFrames; }
    [[nodiscard]] u64 ExecutedCommandCount() const { return m_ExecutedCommands; }

private:
    void ThreadMain();

    RenderCommandQueue& m_Queue;
    std::thread         m_Thread;
    std::atomic<bool>   m_Running{false};
    std::atomic<bool>   m_Exit{false};
    std::atomic<u32>    m_SpinWaitUs{0};
    u64                 m_ExecutedFrames   = 0;   // 仅渲染线程写（x64 上 u64 读写原子，统计用）
    u64                 m_ExecutedCommands = 0;

    /// 渲染线程 id 的哈希 + 1（0 = 无渲染线程/壳模式）。用哈希而不是 `atomic<thread::id>`：
    /// `std::thread::id` 的比较需要读一致快照，哈希成 `size_t` 后可用普通原子量。
    static std::atomic<size_t> s_RenderThreadHash;
};

/// 帧节奏辅助（T0.4 / T2.1）：把"发布 → 执行 → 回收票据"收在一处，供样例（T2.4）与单测复用。
/// · **壳模式**（渲染线程未启动）：发布后**立刻**在本线程执行并回收 ⇒ 在飞帧数 0/1，不会触发背压；
/// · **真线程模式**（T2.1 起）：只发布，由渲染线程异步执行并回收 ⇒ 背压真正生效（在飞帧数受
///   `RenderCommandQueue` 的上限约束，这正是 T0.4 先把骨架与可观测计数做出来的原因）。
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
