#pragma once

#include "Core/Types.h"

// MSVC 2026: <chrono> 需显式包含，taskflow 依赖但未自行引入
#include <chrono>

#include <taskflow/taskflow.hpp>

#include <functional>
#include <span>

// ============================================================
// Job System — thin wrapper over Taskflow for engine workloads
// ============================================================

namespace he {

class JobSystem {
public:
    JobSystem();
    explicit JobSystem(u32 threadCount);
    ~JobSystem();

    // Global singleton
    static JobSystem& Instance();
    static void Initialize(u32 threadCount = 0);  // 0 = auto-detect
    static void Shutdown();

    // --- Fire-and-forget ---
    void Submit(std::function<void()> job);

    // --- Parallel-for over a range ---
    // Splits [0, count) across worker threads
    void ParallelFor(u32 count, std::function<void(u32 index)> body);

    // --- Parallel-for with chunking ---
    void ParallelForChunked(u32 count, u32 chunkSize,
                            std::function<void(u32 start, u32 end)> body);

    // --- Run multiple tasks in parallel, wait for all ---
    void ParallelInvoke(std::span<std::function<void()>> tasks);

    // --- Async task with future ---
    template<typename T>
    std::future<T> Async(std::function<T()> task) {
        return m_Executor->async(std::move(task));
    }

    // --- Wait for all pending work ---
    void WaitAll();

    // --- Query ---
    u32  GetThreadCount() const { return m_ThreadCount; }
    bool IsWorkerThread() const;

private:
    u32                                 m_ThreadCount = 0;
    std::unique_ptr<tf::Taskflow>       m_Taskflow;
    std::unique_ptr<tf::Executor>       m_Executor;

    static std::unique_ptr<JobSystem>   s_Instance;
};

// --- Convenience macros ---
// Parallel for over a container
template<typename Container, typename Func>
void ParallelForEach(Container& container, Func&& func) {
    auto count = static_cast<u32>(std::size(container));
    if (count > 1024) { // Only parallelize large workloads
        JobSystem::Instance().ParallelFor(count, [&](u32 i) {
            func(container[i]);
        });
    } else {
        for (u32 i = 0; i < count; ++i) {
            func(container[i]);
        }
    }
}

} // namespace he
