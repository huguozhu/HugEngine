#pragma once

#include "Core/Types.h"

#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <functional>
#include <atomic>

// ============================================================
// InferenceScheduler — 推理调度器
//
// 在 JobSystem 之上补充两条能力：
// 1. 优先级车道：渲染耦合推理（NRC/超分）走「帧内高优先」，
//    LLM 智能体走「低优先/后台」，互不阻塞；
// 2. 流式回调投递：token 按序投递到主线程（PostToMain + DrainMainThread），
//    禁止推理线程直接改 World。
// ============================================================

namespace he::ai {

// 优先级车道（值越小优先级越高）
enum class InferencePriority {
    Frame      = 0,   // 帧内高优先：渲染耦合推理（NRC/超分）
    Normal     = 1,   // 普通推理
    Background = 2,   // 后台低优先：LLM 智能体等
};

class InferenceScheduler {
public:
    InferenceScheduler();
    ~InferenceScheduler();

    // 提交任务到指定优先级车道；任务在后台工作线程执行
    void Submit(InferencePriority prio, std::function<void()> task);

    // 把回调投递到主线程队列（任何线程可调，线程安全）。
    // 回调不在调用线程执行，而是等主线程 DrainMainThread() 时按序执行。
    void PostToMain(std::function<void()> cb);

    // 主线程每帧调用：执行队列中所有挂起回调（FIFO 顺序）
    void DrainMainThread();

    // 等待所有已提交任务执行完毕（测试/关闭用）
    void WaitIdle();

    // 停止工作线程并清理（幂等）
    void Shutdown();

private:
    // 工作线程主循环：取最高优先级任务执行
    void WorkerLoop();

    struct Task {
        InferencePriority prio = InferencePriority::Normal;
        u64               seq  = 0;              // 同优先级 FIFO
        std::function<void()> fn;
        // priority_queue 默认大顶堆：priority 小的先出
        bool operator<(const Task& o) const {
            if (prio != o.prio) return prio > o.prio;
            return seq > o.seq;
        }
    };

    std::priority_queue<Task> m_Queue;      // 任务队列（按优先级 + FIFO）
    std::mutex                m_Mutex;      // 统一保护队列 + 活跃计数 + 关闭标志
    std::condition_variable   m_CV;         // 任务到达通知（工作线程等待）
    std::condition_variable   m_IdleCV;     // 空闲通知（WaitIdle 等待）
    std::thread               m_Worker;
    std::atomic<bool>         m_Running{true};
    u64                       m_Seq = 0;
    u32                       m_ActiveTasks = 0;   // 正在执行的任务数（m_Mutex 保护）

    // 主线程回调队列（PostToMain / DrainMainThread）
    std::mutex                          m_MainMutex;
    std::vector<std::function<void()>>  m_MainQueue;
};

} // namespace he::ai
