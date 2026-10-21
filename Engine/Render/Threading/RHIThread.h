#pragma once

#include "Core/Types.h"
#include "RHI/SubmitRouter.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

// ============================================================
// RHIThread —— 模式 2 的第三根线程（方案 §12 附录 C 的 A-2 最小落地）
//
// 【职责】**唯一提交者**：渲染线程把 `device->Submit(...)` / `swapchain->Present(...)` 路由到本类
// 的队列，本线程按入队顺序调 `IRHIDevice::ExecuteSubmit` / `ExecutePresent`（真正的
// `vkQueueSubmit` / `vkQueuePresentKHR` 只在它身上发生）。
//
// 【为什么不放在 RenderThread 里】`RenderThread` 刻意保持 **RHI-free**（单测直接编译该 TU），
// 而本类要拿 `IRHIDevice`；因此独立成 TU，由调用方（样例的线程模式切换处）启停。
//
// 【本阶段边界】交接的是**已录制完成**的命令列表，而不是 A-1 的纯数据命令流；`AcquireNextImage`
// 仍在渲染线程（录制需要图像索引）。所以现在满足的是"唯一提交者"，不是完整的 A-1/A-3。
// ============================================================

namespace he::render {

class RHIThread final : public rhi::ISubmitRouter {
public:
    RHIThread() = default;
    ~RHIThread() override { Stop(); }

    /// 启动线程并把 device 的提交路由指向本对象（幂等）。失败返回 false（未起线程 ⇒ 直接就地表）。
    bool Start(rhi::IRHIDevice* device);

    /// 退出前：排空已排队的提交/呈现 → 停线程 → 解绑 device 的路由。
    void Stop();

    [[nodiscard]] bool IsRunning() const { return m_Running.load(std::memory_order_acquire); }
    /// 已执行的提交数（诊断/判据用：模式 2 下它应随帧增长）
    [[nodiscard]] u64 SubmittedCount() const { return m_Submitted.load(std::memory_order_acquire); }

    // --- ISubmitRouter ---
    bool RouteSubmit(rhi::IRHICommandList* cmdList, rhi::RHICommandList* stream) override;
    bool RoutePresent(rhi::IRHISwapChain* swapChain, u32 imageIndex, void* waitSemaphore,
                      bool vsync) override;
    void FlushAndWait() override;

private:
    struct Item {
        enum class Kind { Submit, Present, Barrier };
        Kind                 kind      = Kind::Barrier;
        rhi::IRHICommandList* cmd       = nullptr;
        rhi::RHICommandList*  stream    = nullptr;   // A-1：纯数据命令流（可为空）
        rhi::IRHISwapChain*   swapChain = nullptr;
        u32                   imageIndex = 0;      // 呈现用：渲染线程捕获
        void*                 waitSem    = nullptr; // 呈现用：渲染线程捕获（VkSemaphore 原样）
        bool                  vsync     = true;
        std::function<void()> onDone;              // FlushAndWait 用
    };

    void ThreadMain();
    void Push(Item item);

    /// 第二级背压（A-4）：排队中的提交数上限。渲染线程超过这个数时在 `RouteSubmit` 里阻塞，
    /// 直到 RHI 线程执行掉一项。取 `kMaxFramesInFlight - 1`（=2）：与"每飞行帧一份资源"的
    /// 深度一致，渲染线程至多领先 2 帧，既拿到并行度又保证不会无限跑在提交前面。
    /// 【为什么必须有它】没有背压时渲染线程会瞬间把整帧的提交全压进队列，然后立刻去复用
    /// 命令缓冲 / acquire 下一张交换链图像 —— 而 RHI 线程还没提交，于是命中
    /// VUID-vkQueueSubmit-pCommandBuffers-00070 与 present 系列 VUID（实测）。
    static constexpr u32 kMaxQueuedSubmits = 2;

    rhi::IRHIDevice*        m_Device = nullptr;
    std::thread             m_Thread;
    std::mutex              m_Mutex;
    std::condition_variable m_Cv;
    /// 背压用的第二个条件变量：RHI 线程每执行掉一项就通知生产端（与 m_Cv 分开，避免虚假唤醒耦合）
    std::condition_variable m_SpaceCv;
    u32                     m_QueuedSubmits = 0;   // 受 m_Mutex 保护（排队中的提交数）
    std::deque<Item>        m_Queue;
    std::atomic<bool>       m_Running{false};
    std::atomic<bool>       m_Exit{false};
    std::atomic<u64>        m_Submitted{0};
};

} // namespace he::render
