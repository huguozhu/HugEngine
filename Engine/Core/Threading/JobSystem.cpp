#include "Threading/JobSystem.h"
#include "Core/Log.h"

#include <thread>
#include <algorithm>
#include <vector>

namespace he {

std::unique_ptr<JobSystem> JobSystem::s_Instance;

JobSystem::JobSystem()
    : JobSystem(std::thread::hardware_concurrency()) {
}

JobSystem::JobSystem(u32 threadCount)
    : m_ThreadCount(threadCount > 0 ? threadCount : 1) {
    m_Taskflow = std::make_unique<tf::Taskflow>("HugEngine");
    m_Executor = std::make_unique<tf::Executor>(m_ThreadCount);

    HE_CORE_INFO("JobSystem initialized with {} worker threads", m_ThreadCount);
}

JobSystem::~JobSystem() {
    if (m_Executor) {
        m_Executor->wait_for_all();
    }
    HE_CORE_INFO("JobSystem destroyed");
}

JobSystem& JobSystem::Instance() {
    return *s_Instance;
}

void JobSystem::Initialize(u32 threadCount) {
    s_Instance = std::make_unique<JobSystem>(threadCount);
}

void JobSystem::Shutdown() {
    s_Instance.reset();
}

void JobSystem::Submit(std::function<void()> job) {
    m_Executor->silent_async(std::move(job));
}

void JobSystem::ParallelFor(u32 count, std::function<void(u32 index)> body) {
    // 用 ParallelInvoke 避免 taskflow for_each_index 模板在 MSVC 2026 的链接问题
    std::vector<std::function<void()>> tasks(count);
    for (u32 i = 0; i < count; ++i)
        tasks[i] = [=] { body(i); };
    ParallelInvoke(tasks);
}

void JobSystem::ParallelForChunked(u32 count, u32 chunkSize,
                                   std::function<void(u32 start, u32 end)> body) {
    u32 numChunks = (count + chunkSize - 1) / chunkSize;
    std::vector<std::function<void()>> tasks(numChunks);
    for (u32 i = 0; i < numChunks; ++i) {
        tasks[i] = [=] {
            u32 start = i * chunkSize;
            u32 end   = std::min(start + chunkSize, count);
            body(start, end);
        };
    }
    ParallelInvoke(tasks);
}

void JobSystem::ParallelInvoke(std::span<std::function<void()>> tasks) {
    for (auto& task : tasks) {
        m_Executor->silent_async(std::move(task));
    }
    m_Executor->wait_for_all();
}

void JobSystem::WaitAll() {
    m_Executor->wait_for_all();
}

bool JobSystem::IsWorkerThread() const {
    // Taskflow doesn't expose this directly; approximate
    return false;
}

} // namespace he
