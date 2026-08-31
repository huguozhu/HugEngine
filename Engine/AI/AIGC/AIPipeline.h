#pragma once

#include "Core/Types.h"
#include "AI/AIGC/AIGCProvider.h"
#include "AI/Runtime/InferenceScheduler.h"

#include <memory>
#include <mutex>
#include <vector>
#include <functional>
#include <atomic>

// ============================================================
// AIPipeline — 异步生成管线
//
// 用户 prompt → Enqueue(GenRequest)
//            → 后台线程（InferenceScheduler）调 IAIGCProvider.Generate
//            → 完成回调投递主线程（PostToMain）
//            → 主线程 Poll() 派发 onDone → 结果入「待接受队列」
//            → 用户在 GenerationQueuePanel 接受/拒绝
//            → 接受 = CommandHistory.Execute(GenerateSceneCommand)  ← 可撤销
//
// 职责：任务去重/合并、取消、失败重试、主线程回调派发。
// ============================================================

namespace he::ai::aigc {

class AIPipeline {
public:
    // provider：生成后端（不拥有）；scheduler：后台调度器（不拥有）
    AIPipeline(IAIGCProvider* provider, InferenceScheduler* scheduler);
    ~AIPipeline();

    /// 提交生成请求（后台异步执行）。
    /// 去重：相同 (kind, prompt) 的排队任务合并，直接返回已有任务 id。
    /// @return 任务 id；0 表示无法提交
    u64 Enqueue(const GenRequest& req, std::function<void(GenResult&&)> onDone);

    /// 取消排队/执行中的任务（执行中的无法中断，仅不再派发 onDone）
    void Cancel(u64 taskId);

    /// 主线程每帧调用：执行所有挂起的完成回调（经调度器投递）
    void Poll();

    /// 清空队列并等待后台任务结束（析构/关闭用）
    void Shutdown();

private:
    struct Task {
        u64 id;
        GenRequest req;
        std::function<void(GenResult&&)> onDone;
        std::atomic<bool> cancelled{false};   // 取消标志
        int  retriesLeft = 1;                 // 失败重试次数
        GenResult result;                     // 完成结果（主线程读取）
    };

    // 后台线程执行：调 provider 生成；失败重试；完成后投递主线程
    void RunTask(const std::shared_ptr<Task>& task);

    IAIGCProvider*       m_Provider  = nullptr;   // 生成后端（不拥有）
    InferenceScheduler*  m_Scheduler = nullptr;   // 后台调度器（不拥有）
    std::mutex           m_Mutex;                 // 保护队列与 id 分配
    std::vector<std::shared_ptr<Task>> m_Queue;   // 排队/执行中的任务（去重、取消、清理）
    u64                  m_NextId = 1;
};

} // namespace he::ai::aigc
