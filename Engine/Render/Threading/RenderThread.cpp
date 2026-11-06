#include "Threading/RenderThread.h"

#include "RHI/RHI.h"   // 【设备兜底】未调用 SetDevice 的样例退回进程级 RHI 设备（rhi::GetDevice）

#include <chrono>
#include <functional>

namespace he::render {

std::atomic<size_t> RenderThread::s_RenderThreadHash{0};

bool RenderThread::IsCurrent() {
    const size_t hash = s_RenderThreadHash.load(std::memory_order_acquire);
    if (hash == 0) return true;      // 壳模式：没有第二根线程，"当前线程即渲染线程"
    return (std::hash<std::thread::id>{}(std::this_thread::get_id()) + 1u) == hash;
}

bool RenderThread::Start() {
    if (m_Running.load(std::memory_order_acquire)) return true;   // 幂等
    m_Exit.store(false, std::memory_order_release);
    m_Running.store(true, std::memory_order_release);
    m_Thread = std::thread([this] {
        // 线程 id 必须由**新线程自己**登记（否则比较的是调用者）
        s_RenderThreadHash.store(std::hash<std::thread::id>{}(std::this_thread::get_id()) + 1u,
                                 std::memory_order_release);
        // 【T2.2】启动钩子：在消费任何帧**之前**、于渲染线程上执行一次。
        // 样例借此把 RHI 归属认领到渲染线程（`he::rhi::GetThreadAffinity().Claim()`）——
        // 认领必须在新线程里做（记录的是当前线程 id），而本层保持 RHI-free ⇒ 由调用方注入。
        if (m_StartHook) m_StartHook();
        ThreadMain();
    });
    return true;
}

void RenderThread::ThreadMain() {
    while (!m_Exit.load(std::memory_order_acquire)) {
        if (PumpOnce()) continue;                                  // 有活就连续干，不睡

        // 空闲：先按配置自旋（低延迟场景），再短休眠（不烧核）
        const u32 spinUs = m_SpinWaitUs.load(std::memory_order_relaxed);
        if (spinUs > 0u) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::microseconds(spinUs);
            while (!m_Exit.load(std::memory_order_relaxed) && std::chrono::steady_clock::now() < deadline) {
                if (PumpOnce()) break;
                std::this_thread::yield();
            }
        } else {
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    }
    PumpAll();   // 退出前排空：已提交的帧不能丢（否则游戏线程会永远等票据回收）
}

void RenderThread::Stop() {
    if (!m_Running.load(std::memory_order_acquire)) return;
    m_Exit.store(true, std::memory_order_release);
    if (m_Thread.joinable()) m_Thread.join();
    m_Running.store(false, std::memory_order_release);
    s_RenderThreadHash.store(0, std::memory_order_release);         // 回到壳语义
    // 停止钩子：线程已结束 ⇒ 由调用线程撤销归属认领（否则收尾期的设备销毁/WaitIdle 会违规）
    if (m_StopHook) m_StopHook();
}

bool RenderThread::PumpOnce() {
    RenderFrameCommands frame;
    if (!m_Queue.TryConsumeFrame(frame)) return false;

    RenderThreadContext ctx;
    ctx.frameIndex = frame.frameIndex;
    ctx.frameSlot  = frame.frameSlot;
    // 【T2.6】本次泵帧的 RHI 出口。
    // 【设备兜底】样例可以显式 `SetDevice`（06.GILab / 02.Cube 都这么做）；漏登记的样例（03/04/05/07）
    // 在壳模式泵帧时会把 device 为空的本上下文发布到线程局部 ⇒ 各 pass 的 ResB/ResT 恒为 nullptr，
    // 随后解引用空指针。这里退回进程级 RHI 设备（每个样例创建完设备都会 `rhi::SetDevice`），
    // 语义与"没有上下文时回退到各 pass 自己的 m_Device"一致。
    ctx.device     = m_Device ? m_Device : rhi::GetDevice();

    // 【T2.6】把"本帧上下文"发布到线程局部，供各 pass 的解析 helper 回退使用；
    //命令全部执行完后清除，避免下一帧/其它路径拿到过期上下文。
    SetCurrentFrameContext(&ctx);
    for (RenderCommand& cmd : frame.commands) {
        if (cmd) cmd(ctx);                 // 命令为空（默认构造的 std::function）时跳过
        ++m_ExecutedCommands;
    }
    SetCurrentFrameContext(nullptr);

    // 壳模式：执行完即视为"该帧已完成" ⇒ 立刻回收票据，让游戏线程不会因为壳而卡住
    m_Queue.RetireFrame(FrameTicket{frame.frameIndex, frame.frameSlot});
    ++m_ExecutedFrames;
    return true;
}

u32 RenderThread::PumpAll() {
    u32 frames = 0;
    while (PumpOnce()) ++frames;
    return frames;
}

u64 FrameScheduler::SubmitAndPump() {
    const FrameTicket ticket = m_Queue.SubmitFrameBlocking();
    // 真线程模式：交给渲染线程执行并回收票据（**不**在调用线程执行）；壳模式：就地执行（阶段 0 语义）
    if (!m_RenderThread.IsRunning()) m_RenderThread.PumpOnce();
    return ticket.frameIndex;
}

u64 FrameScheduler::SubmitAndWait(bool& timedOut, u32 timeoutMs) {
    const FrameTicket ticket = m_Queue.SubmitFrameBlocking();
    // 壳模式：就地执行并回收（语义与 SubmitAndPump 一致）
    if (!m_RenderThread.IsRunning()) m_RenderThread.PumpOnce();
    timedOut = !m_Queue.WaitFrameRetired(ticket.frameIndex, timeoutMs);
    return ticket.frameIndex;
}

bool FrameScheduler::TrySubmitAndPump() {
    FrameTicket ticket{};
    if (!m_Queue.TrySubmitFrame(ticket)) return false;
    if (!m_RenderThread.IsRunning()) m_RenderThread.PumpOnce();
    return true;
}

} // namespace he::render
