#pragma once

#include "Core/Types.h"
#include "Threading/RenderCommandQueue.h"

#include <atomic>
#include <cstddef>
#include <functional>
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

    /// 线程启动钩子（T2.2）：在**渲染线程**上、进入消费循环之前调用一次。
    /// 【为什么需要它】RHI 的归属（`he::rhi::ThreadAffinity`）按"当前线程"认领，而本层刻意保持
    /// **RHI-free**（便于单测直接编译）⇒ 认领动作由调用方通过这个钩子注入（样例里一行
    /// `GetThreadAffinity().Claim()`）。这也是将来 T2.6 初始化 `RenderThreadContext` 的挂点。
    void SetThreadStartHook(std::function<void()> hook) { m_StartHook = std::move(hook); }

    /// 线程停止钩子（T2.2）：在渲染线程**结束之后**（`Stop()` 里 join 之后）由调用线程执行一次。
    /// 与启动钩子对称：启动时认领 RHI 归属、停止时撤销（恢复"未 Claim ⇒ 不设限"的语义），
    /// 否则设备销毁/`WaitIdle` 这些仍在游戏线程做的收尾动作会撞上归属断言。
    void SetThreadStopHook(std::function<void()> hook) { m_StopHook = std::move(hook); }

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
    std::function<void()> m_StartHook;   // 渲染线程启动钩子（T2.2：由调用方注入 RHI 归属认领）
    std::function<void()> m_StopHook;    // 渲染线程停止钩子（T2.2：撤销归属认领）

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

    /// **严格握手提交（T2.2 步骤 (c)）**：阻塞提交一帧后，等它**真正被消费完**（含呈现）再返回。
    /// 真线程模式：不在调用线程执行，由渲染线程执行并回收 ⇒ 这里等 `WaitFrameRetired`。
    /// 壳模式：`PumpOnce` 就地执行并回收 ⇒ 立刻满足。
    /// 【为什么要它】"把每帧 RHI 调用搬进渲染命令"要逐项验证（每搬一项转储都应逐位一致），
    /// 严格握手让每帧串行，从而把"执行位置搬了但语义没变"这件事单独测出来。
    /// @return 该帧号；`timedOut` 置位表示等待超时（调用方据此告警，不静默继续）
    u64 SubmitAndWait(bool& timedOut, u32 timeoutMs = 0u);

    /// 非阻塞提交（在飞满时返回 false 并丢弃本帧），同样立即执行并回收。
    [[nodiscard]] bool TrySubmitAndPump();

private:
    RenderCommandQueue& m_Queue;
    RenderThread&       m_RenderThread;
};

} // namespace he::render
