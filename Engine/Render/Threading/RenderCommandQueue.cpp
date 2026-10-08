#include "Threading/RenderCommandQueue.h"

#include <utility>

namespace he::render {

RenderCommandQueue::RenderCommandQueue(u32 maxInFlight)
    : m_MaxInFlight(maxInFlight == 0u ? 1u : maxInFlight) {
}

void RenderCommandQueue::BeginFrame() {
    std::lock_guard<std::mutex> lock(m_Mutex);
    // 上一次 BeginFrame 没有 PublishFrame 就再次 Begin：视为丢弃半成品（不静默累积，计入丢弃数）
    if (m_Collecting) {
        ++m_Dropped;
        m_Collecting_Commands.clear();
    }
    m_Collecting   = true;
    m_CollectFrame = m_NextFrameIndex;
    m_CollectSlot  = m_NextSlot;
    m_NextFrameIndex++;
    m_NextSlot = (m_NextSlot + 1u) % m_MaxInFlight;
}

void RenderCommandQueue::Enqueue(RenderCommand fn) {
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (!m_Collecting) return;              // 未 BeginFrame 就 Enqueue：忽略（避免污染上一帧）
    m_Collecting_Commands.push_back(std::move(fn));
}

void RenderCommandQueue::PublishLocked() {
    RenderFrameCommands frame;
    frame.frameIndex = m_CollectFrame;
    frame.frameSlot  = m_CollectSlot;
    frame.commands   = std::move(m_Collecting_Commands);
    m_Collecting_Commands.clear();
    m_Collecting = false;

    m_Pending.push_back(std::move(frame));
    ++m_InFlight;                           // 已发布但尚未 Retire ⇒ 占用一个在飞名额
    ++m_Published;
}

void RenderCommandQueue::PublishFrame() {
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (!m_Collecting) return;
    PublishLocked();
    m_Cv.notify_all();                      // 唤醒可能在 WaitConsumeFrame 上休眠的消费者
}

bool RenderCommandQueue::TrySubmitFrame(FrameTicket& outTicket) {
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (!m_Collecting) return false;
    if (m_InFlight >= m_MaxInFlight) {      // 在飞已满 ⇒ 非阻塞语义下拒绝发布（调用方可丢帧）
        ++m_Dropped;
        m_Collecting_Commands.clear();
        m_Collecting = false;
        return false;
    }
    outTicket = FrameTicket{m_CollectFrame, m_CollectSlot};
    PublishLocked();
    m_Cv.notify_all();
    return true;
}

FrameTicket RenderCommandQueue::SubmitFrameBlocking() {
    std::unique_lock<std::mutex> lock(m_Mutex);
    FrameTicket ticket{};
    if (!m_Collecting) return ticket;

    // 背压点：等在飞帧数降到上限之下。这里是游戏线程**唯一**允许等待的位置（方案 §3）。
    if (m_InFlight >= m_MaxInFlight) {
        ++m_BackpressureWaits;
        m_Cv.wait(lock, [this] { return m_InFlight < m_MaxInFlight; });
    }
    ticket = FrameTicket{m_CollectFrame, m_CollectSlot};
    PublishLocked();
    m_Cv.notify_all();
    return ticket;
}

bool RenderCommandQueue::TryConsumeFrame(RenderFrameCommands& out) {
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (m_Pending.empty()) return false;
    out = std::move(m_Pending.front());
    m_Pending.pop_front();
    return true;
}

bool RenderCommandQueue::WaitConsumeFrame(RenderFrameCommands& out, u32 timeoutMs) {
    std::unique_lock<std::mutex> lock(m_Mutex);
    if (timeoutMs == 0u) {
        m_Cv.wait(lock, [this] { return !m_Pending.empty(); });
    } else if (!m_Cv.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                              [this] { return !m_Pending.empty(); })) {
        return false;                       // 超时：由调用方决定继续等待还是退出（休眠策略见 T2.1）
    }
    out = std::move(m_Pending.front());
    m_Pending.pop_front();
    return true;
}

void RenderCommandQueue::RetireFrame(const FrameTicket& ticket) {
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        (void)ticket;                       // 阶段 0 不做逐票校验；帧序号校验留给 T2.6 的 fence 对账
        if (m_InFlight > 0u) --m_InFlight;
        ++m_Retired;
    }
    m_Cv.notify_all();                      // 唤醒被背压阻塞的游戏线程
}

u32 RenderCommandQueue::InFlightFrameCount() const {
    std::lock_guard<std::mutex> lock(m_Mutex);
    return m_InFlight;
}

u32 RenderCommandQueue::PendingFrameCount() const {
    std::lock_guard<std::mutex> lock(m_Mutex);
    return static_cast<u32>(m_Pending.size());
}

u64 RenderCommandQueue::PublishedFrameCount() const {
    std::lock_guard<std::mutex> lock(m_Mutex);
    return m_Published;
}

u64 RenderCommandQueue::RetiredFrameCount() const {
    std::lock_guard<std::mutex> lock(m_Mutex);
    return m_Retired;
}

u64 RenderCommandQueue::BackpressureWaitCount() const {
    std::lock_guard<std::mutex> lock(m_Mutex);
    return m_BackpressureWaits;
}

void RenderCommandQueue::Clear() {
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_Pending.clear();
    m_Collecting_Commands.clear();
    m_Collecting = false;
    m_InFlight   = 0u;
    m_NextSlot   = 0u;
}

} // namespace he::render
