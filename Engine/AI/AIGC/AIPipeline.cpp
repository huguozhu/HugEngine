#include "AI/AIGC/AIPipeline.h"

#include "Core/Log.h"

#include <utility>

namespace he::ai::aigc {

AIPipeline::AIPipeline(IAIGCProvider* provider, InferenceScheduler* scheduler)
    : m_Provider(provider), m_Scheduler(scheduler) {}

AIPipeline::~AIPipeline() {
    Shutdown();
}

u64 AIPipeline::Enqueue(const GenRequest& req, std::function<void(GenResult&&)> onDone) {
    if (!m_Provider || !m_Scheduler) return 0;

    std::lock_guard<std::mutex> lock(m_Mutex);

    // 去重/合并：相同 (kind, prompt) 的排队任务直接复用
    for (auto& t : m_Queue) {
        if (t->req.kind == req.kind && t->req.prompt == req.prompt) {
            HE_CORE_INFO("[AIPipeline] 相同请求已在队列中，合并（task={}）", t->id);
            return t->id;
        }
    }

    // 创建任务并入队
    auto task = std::make_shared<Task>();
    task->id   = m_NextId++;
    task->req  = req;
    task->onDone = std::move(onDone);
    m_Queue.push_back(task);

    // 提交后台执行（低优先车道：不阻塞渲染耦合推理）
    m_Scheduler->Submit(InferencePriority::Background,
                        [this, task] { RunTask(task); });
    HE_CORE_INFO("[AIPipeline] 已提交生成任务 task={}", task->id);
    return task->id;
}

void AIPipeline::Cancel(u64 taskId) {
    std::lock_guard<std::mutex> lock(m_Mutex);
    for (auto& t : m_Queue) {
        if (t->id == taskId) {
            t->cancelled.store(true);
            HE_CORE_INFO("[AIPipeline] 已取消任务 task={}", taskId);
            return;
        }
    }
}

void AIPipeline::Poll() {
    // 主线程派发完成回调（FIFO 保序；回调内会清理队列）
    if (m_Scheduler) m_Scheduler->DrainMainThread();
}

void AIPipeline::Shutdown() {
    std::lock_guard<std::mutex> lock(m_Mutex);
    // 标记全部取消，等待后台任务自然结束（WaitIdle 由调用方/析构后调度器负责）
    for (auto& t : m_Queue) t->cancelled.store(true);
    m_Queue.clear();
}

// ============================================================
// 后台执行
// ============================================================

void AIPipeline::RunTask(const std::shared_ptr<Task>& task) {
    // 已取消：不再执行生成
    if (task->cancelled.load()) {
        // 仍投递一个"结束"回调，让主线程清理队列（不派发 onDone）
        m_Scheduler->PostToMain([this, task] {
            std::lock_guard<std::mutex> lock(m_Mutex);
            for (usize i = 0; i < m_Queue.size(); ++i) {
                if (m_Queue[i]->id == task->id) { m_Queue.erase(m_Queue.begin() + i); break; }
            }
        });
        return;
    }

    // 调生成后端（同步阻塞于后台线程）
    m_Provider->Generate(task->req, [this, task](GenResult&& r) {
        // 失败且还有重试次数 → 重新提交后台执行
        if (!r.success && task->retriesLeft > 0) {
            --task->retriesLeft;
            HE_CORE_WARN("[AIPipeline] 生成失败（将重试 {} 次）：{}", task->retriesLeft, r.error);
            m_Scheduler->Submit(InferencePriority::Background,
                                [this, task] { RunTask(task); });
            return;
        }
        // 完成 → 结果暂存，投递主线程派发
        task->result = std::move(r);
        m_Scheduler->PostToMain([this, task] {
            // 从队列移除（先于 onDone，保证去重状态一致）
            {
                std::lock_guard<std::mutex> lock(m_Mutex);
                for (usize i = 0; i < m_Queue.size(); ++i) {
                    if (m_Queue[i]->id == task->id) { m_Queue.erase(m_Queue.begin() + i); break; }
                }
            }
            // 派发完成回调（已取消的任务不派发）
            if (task->onDone && !task->cancelled.load())
                task->onDone(std::move(task->result));
        });
    });
}

} // namespace he::ai::aigc
