#include "Threading/RHIThread.h"

#include "RHI/RHI.h"
#include "RHI/ThreadAffinity.h"
#include "Core/Log.h"

namespace he::render {

bool RHIThread::Start(rhi::IRHIDevice* device) {
    if (m_Running.load(std::memory_order_acquire)) return true;
    if (!device) return false;

    m_Device = device;
    {
        std::lock_guard<std::mutex> lk(m_Mutex);
        m_Exit.store(false, std::memory_order_release);
        m_Queue.clear();
    }
    m_Running.store(true, std::memory_order_release);
    m_Thread = std::thread([this] { ThreadMain(); });

    // 路由必须在"线程已起但尚未收到提交"之前装上：此后渲染线程的 Submit/Present 才走队列
    device->SetSubmitRouter(this);
    HE_CORE_INFO("RHIThread: 已起（模式 2：RHI 线程 = 唯一提交者）");
    return true;
}

void RHIThread::Stop() {
    if (m_Thread.joinable()) {
        {
            std::lock_guard<std::mutex> lk(m_Mutex);
            m_Exit.store(true, std::memory_order_release);
        }
        m_Cv.notify_all();
        m_Thread.join();      // 线程退出前会把队列里剩余的提交/呈现全部执行完（见 ThreadMain 的排空条件）
    }
    m_Running.store(false, std::memory_order_release);
    if (m_Device) {
        m_Device->SetSubmitRouter(nullptr);   // 之后 Submit/Present 回到"就地执行"
        m_Device = nullptr;
    }
    HE_CORE_INFO("RHIThread: 已停（累计执行提交 {} 次）",
                 m_Submitted.load(std::memory_order_acquire));
}

void RHIThread::ThreadMain() {
    // 【归属】本线程是"提交者"而不是"拥有线程"：录制（Begin/End/Draw）仍断言拥有线程，
    // 只有 Submit/Present 允许由提交者发起（见 ThreadAffinity.h 的三级模型）。
    rhi::GetThreadAffinity().SetSubmitter();

    for (;;) {
        Item item;
        {
            std::unique_lock<std::mutex> lk(m_Mutex);
            m_Cv.wait(lk, [this] {
                return m_Exit.load(std::memory_order_acquire) || !m_Queue.empty();
            });
            if (m_Queue.empty()) {
                if (m_Exit.load(std::memory_order_acquire)) break;   // 队列已排空且被要求退出
                continue;
            }
            item = std::move(m_Queue.front());
            m_Queue.pop_front();
        }

        switch (item.kind) {
        case Item::Kind::Submit:
            if (m_Device && item.cmd) m_Device->ExecuteSubmit(item.cmd);
            m_Submitted.fetch_add(1, std::memory_order_acq_rel);
            break;
        case Item::Kind::Present:
            if (m_Device && item.swapChain)
                m_Device->ExecutePresent(item.swapChain, item.imageIndex, item.waitSem, item.vsync);
            break;
        case Item::Kind::Barrier:
            break;   // 纯同步点：能执行到这里 ⇒ 之前的提交/呈现都已落地
        }
        // 提交执行完 ⇒ 归还一个背压额度（唤醒可能正在等待的渲染线程）
        if (item.kind == Item::Kind::Submit) {
            {
                std::lock_guard<std::mutex> lk(m_Mutex);
                if (m_QueuedSubmits > 0u) --m_QueuedSubmits;
            }
            m_SpaceCv.notify_one();
        }
        if (item.onDone) item.onDone();
    }

    rhi::GetThreadAffinity().ClearSubmitter();
}

void RHIThread::Push(Item item) {
    {
        std::lock_guard<std::mutex> lk(m_Mutex);
        m_Queue.push_back(std::move(item));
    }
    m_Cv.notify_one();
}

bool RHIThread::RouteSubmit(rhi::IRHICommandList* cmdList) {
    // 未运行 / 未就绪 ⇒ 返回 false 让调用方就地执行（不丢命令、不死等）
    if (!m_Running.load(std::memory_order_acquire) || !cmdList) return false;

    // 【A-4 第二级背压】排队中的提交达到上限 ⇒ 在调用方（渲染线程）阻塞，直到 RHI 线程吃下至少一项。
    // 这样渲染线程至多领先 kMaxQueuedSubmits 帧 ⇒ 它不会在提交尚未落地时就复用命令缓冲/交换链图像。
    {
        std::unique_lock<std::mutex> lk(m_Mutex);
        m_SpaceCv.wait(lk, [this] {
            return m_QueuedSubmits < kMaxQueuedSubmits ||
                   !m_Running.load(std::memory_order_acquire);
        });
        if (!m_Running.load(std::memory_order_acquire)) return false;
        ++m_QueuedSubmits;
    }

    Item item;
    item.kind = Item::Kind::Submit;
    item.cmd  = cmdList;
    Push(std::move(item));
    return true;
}

bool RHIThread::RoutePresent(rhi::IRHISwapChain* swapChain, u32 imageIndex, void* waitSemaphore,
                             bool vsync) {
    if (!m_Running.load(std::memory_order_acquire) || !swapChain) return false;
    Item item;
    item.kind       = Item::Kind::Present;
    item.swapChain  = swapChain;
    item.imageIndex = imageIndex;
    item.waitSem    = waitSemaphore;
    item.vsync      = vsync;
    Push(std::move(item));
    return true;
}

void RHIThread::FlushAndWait() {
    if (!m_Running.load(std::memory_order_acquire)) return;

    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    Item barrier;
    barrier.kind   = Item::Kind::Barrier;
    barrier.onDone = [&] {
        {
            std::lock_guard<std::mutex> lk(m);
            done = true;
        }
        cv.notify_all();
    };
    Push(std::move(barrier));

    std::unique_lock<std::mutex> lk(m);
    cv.wait(lk, [&] { return done; });
}

} // namespace he::render
