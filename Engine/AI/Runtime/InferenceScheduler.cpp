#include "AI/Runtime/InferenceScheduler.h"

#include "Core/Log.h"

#include <utility>

namespace he::ai {

InferenceScheduler::InferenceScheduler() {
    // 启动单工作线程（MVP：推理任务串行执行，保证流式回调顺序）
    m_Worker = std::thread(&InferenceScheduler::WorkerLoop, this);
}

InferenceScheduler::~InferenceScheduler() {
    Shutdown();
}

void InferenceScheduler::Submit(InferencePriority prio, std::function<void()> task) {
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Queue.push(Task{prio, m_Seq++, std::move(task)});
    }
    m_CV.notify_one();
}

void InferenceScheduler::PostToMain(std::function<void()> cb) {
    std::lock_guard<std::mutex> lock(m_MainMutex);
    m_MainQueue.push_back(std::move(cb));
}

void InferenceScheduler::DrainMainThread() {
    // 把挂起回调一次性取出，避免长时间持锁阻塞推理线程 PostToMain
    std::vector<std::function<void()>> callbacks;
    {
        std::lock_guard<std::mutex> lock(m_MainMutex);
        callbacks.swap(m_MainQueue);
    }
    // 主线程按 FIFO 顺序执行（流式 token 保序的关键）
    for (auto& cb : callbacks)
        cb();
}

void InferenceScheduler::WaitIdle() {
    std::unique_lock<std::mutex> lock(m_Mutex);
    // 等待：任务队列为空 且 没有正在执行的任务。
    // 活跃计数与队列同锁保护：Worker「取任务」与「计数+1」在同一临界区，
    // 不会出现"已取走但未计数"的窗口导致误判空闲。
    m_IdleCV.wait(lock, [&] {
        return m_Queue.empty() && m_ActiveTasks == 0;
    });
}

void InferenceScheduler::Shutdown() {
    bool expected = true;
    if (!m_Running.compare_exchange_strong(expected, false)) return;  // 幂等
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Queue = {};   // 丢弃剩余任务
        m_ActiveTasks = 0;
    }
    m_CV.notify_all();
    if (m_Worker.joinable()) m_Worker.join();
}

void InferenceScheduler::WorkerLoop() {
    for (;;) {
        Task task;
        {
            std::unique_lock<std::mutex> lock(m_Mutex);
            m_CV.wait(lock, [&] { return !m_Running || !m_Queue.empty(); });
            if (!m_Running && m_Queue.empty()) break;   // 关闭且队列清空 → 退出

            // 取任务并在同一临界区把活跃计数 +1（保证 WaitIdle 不会误判）
            task = m_Queue.top();       // top() 为 const 引用，拷贝后 pop
            m_Queue.pop();
            ++m_ActiveTasks;
        }

        // 执行任务（锁外执行，不阻塞队列操作）
        if (task.fn) task.fn();

        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            --m_ActiveTasks;
            if (m_Queue.empty() && m_ActiveTasks == 0)
                m_IdleCV.notify_all();   // 空闲：唤醒 WaitIdle
        }
    }
}

} // namespace he::ai
