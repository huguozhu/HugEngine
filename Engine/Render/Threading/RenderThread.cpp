#include "Threading/RenderThread.h"

namespace he::render {

bool RenderThread::PumpOnce() {
    RenderFrameCommands frame;
    if (!m_Queue.TryConsumeFrame(frame)) return false;

    RenderThreadContext ctx;
    ctx.frameIndex = frame.frameIndex;
    ctx.frameSlot  = frame.frameSlot;

    for (RenderCommand& cmd : frame.commands) {
        if (cmd) cmd(ctx);                 // 命令为空（默认构造的 std::function）时跳过
        ++m_ExecutedCommands;
    }

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
    m_RenderThread.PumpOnce();
    return ticket.frameIndex;
}

bool FrameScheduler::TrySubmitAndPump() {
    FrameTicket ticket{};
    if (!m_Queue.TrySubmitFrame(ticket)) return false;
    m_RenderThread.PumpOnce();
    return true;
}

} // namespace he::render
