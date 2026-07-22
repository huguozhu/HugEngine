#pragma once

// ============================================================
// PSOPrecompileManager — PSO 后台预热管理器
//
// 在后台线程中创建 PSO 变体到独立 VkPipelineCache，完成后
// vkMergePipelineCaches 合并到主缓存，使主线程后续
// CreatePipelineState 调用享受驱动端编译缓存加速（~2ms vs ~50ms）。
//
// 线程安全模型：
//   - QueuePSO() 可在主线程任意时刻调用（内部加锁）
//   - StartPrecompile() 启动后台线程，拥有独立 VkPipelineCache
//   - MergeCache() 在主线程调用 vkMergePipelineCaches（Vulkan 保证线程安全）
//   - GetProgress() 读取原子计数器，无锁
// ============================================================

#include "Core/Types.h"

#include <vulkan/vulkan.h>

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

namespace he::rhi {

struct PipelineStateDesc;

class PSOPrecompileManager {
public:
    PSOPrecompileManager();
    ~PSOPrecompileManager();

    /// 初始化：保存设备和主缓存句柄
    void Initialize(VkDevice device, VkPhysicalDevice physical,
                    VkPipelineCache mainCache);
    /// 关闭：等待线程完成 + 销毁 worker cache
    void Shutdown();

    /// 注册一个 PSO 到预热队列（线程安全）
    void QueuePSO(const PipelineStateDesc& desc);

    /// 启动后台预热线程
    /// 线程从主缓存派生独立 VkPipelineCache，逐项编译队列中的 PSO
    void StartPrecompile();

    /// 将 worker 线程的编译结果合并到主缓存（主线程调用）
    /// 合并后主缓存包含 worker 的所有编译数据，后续 vkCreate*Pipelines 加速
    void MergeCache();

    /// 预热进度（0.0 ~ 1.0）
    float GetProgress() const {
        u32 total = m_TotalCount.load(std::memory_order_acquire);
        if (total == 0) return 1.0f;
        return static_cast<float>(m_CompiledCount.load(std::memory_order_acquire)) / static_cast<float>(total);
    }

    /// 是否已完成所有预热
    bool IsDone() const {
        return m_CompiledCount.load(std::memory_order_acquire) >=
               m_TotalCount.load(std::memory_order_acquire);
    }

    /// 是否有排队的 PSO（尚未启动预热或正在进行中）
    bool HasWork() const { return m_TotalCount.load() > 0; }

private:
    /// 后台线程入口
    void WorkerThreadFunc();

    VkDevice         m_Device       = VK_NULL_HANDLE;
    VkPhysicalDevice m_Physical      = VK_NULL_HANDLE;
    VkPipelineCache  m_MainCache    = VK_NULL_HANDLE;
    VkPipelineCache  m_WorkerCache  = VK_NULL_HANDLE;

    // PSO 描述符队列（主线程写入 → worker 线程读取）
    std::vector<PipelineStateDesc> m_Queue;
    mutable std::mutex             m_QueueMutex;

    // 进度追踪
    std::atomic<u32>m_CompiledCount{0};
    std::atomic<u32>m_TotalCount{0};

    // 线程控制
    std::thread       m_WorkerThread;
    std::atomic<bool> m_Running{false};
    std::atomic<bool> m_StopRequested{false};
};

} // namespace he::rhi
