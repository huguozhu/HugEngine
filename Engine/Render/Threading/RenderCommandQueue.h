#pragma once

#include "Core/Types.h"
#include "Threading/RenderThreadContext.h"

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <vector>

// ============================================================
// RenderCommandQueue —— 游戏线程 → 渲染线程的命令队列（方案 B §4.1 / 阶段 0 T0.3、T0.4）
//
// 设计约束（对应 §3 的三条铁律）：
//   1. 载荷**按值捕获**（铁律 2）：命令交接后游戏线程不得再改它捕获的数据。队列本身不做深拷贝
//      （那需要知道载荷类型），所以"按值捕获"是**入队方的纪律**，由 T2.4 的样例改造执行；
//   2. **整帧批量交接**：一帧一次锁（`PublishFrame`），而不是每条命令一次锁；
//   3. **有界**：在飞帧数达到上限时（默认 `kMaxFramesInFlight`），`SubmitFrame` 提供阻塞与非阻塞
//      两种语义 —— 非阻塞版供单测与"丢帧"策略使用，阻塞版是游戏线程唯一的合法等待点（背压）。
//
// 【阶段 0 的执行位置】本阶段不起线程：`RenderThread` 的壳在**当前线程**消费队列（帧序与改前一致）。
// 真正起线程是阶段 2（T2.1/T2.2）。因此本文件**不依赖 RHI**，单测可以直接编译它。
//
// 【为什么命令用 std::function】阶段 0 只有"整帧一次"的交接量（每帧数十条），`std::function` 的
// 间接调用与堆分配都可忽略；§12 附录 C 的 A-1 明确要求将来改成"类型擦除 + 内联存储"的 RHICommand
// 载荷（每帧数千条时才需要）。T0.6 会落下那份契约的头文件，本文件到时候只换载荷类型。
// ============================================================

namespace he::render {

using RenderCommand = std::function<void(RenderThreadContext&)>;

/// 一帧的命令束（游戏线程收集 → 渲染线程一次性取走）
struct RenderFrameCommands {
    u64                     frameIndex = 0;
    u32                     frameSlot  = 0;
    std::vector<RenderCommand> commands;
};

/// 帧票据：渲染线程消费完一帧后回收，游戏线程据此解除背压（方案 §4.3）
struct FrameTicket {
    u64 frameIndex = 0;   // 与 RenderFrameCommands::frameIndex 对应
    u32 frameSlot  = 0;
};

class RenderCommandQueue {
public:
    /// 在飞帧上限：默认与 RHI 的 `kMaxFramesInFlight` 对齐（= 3）
    explicit RenderCommandQueue(u32 maxInFlight = 3u);

    // --- 游戏线程侧 ---

    /// 开始收集本帧命令：拿到帧号与槽位（帧号单调递增，槽位在 [0, maxInFlight) 内轮转）
    void BeginFrame();

    /// 入队一条命令。`fn` 在渲染线程执行；**禁止捕获 World / SceneGraph 引用或任何会被
    /// 游戏线程继续修改的数据**（铁律 2）。
    void Enqueue(RenderCommand fn);

    /// 结束收集并发布给渲染线程（一次移动，不拷贝）
    void PublishFrame();

    /// 发布一帧并等待"在飞帧数 < 上限"（背压点）。返回本帧票据。
    /// **这是游戏线程唯一允许的等待位置**（方案 §3）。
    [[nodiscard]] FrameTicket SubmitFrameBlocking();

    /// 非阻塞提交：在飞帧数已达上限时返回 false（不发布），供"丢帧"策略与单测使用。
    [[nodiscard]] bool TrySubmitFrame(FrameTicket& outTicket);

    // --- 渲染线程侧 ---

    /// 取出下一帧待执行命令；无待处理帧时返回 false（由调用方决定等待/休眠）
    [[nodiscard]] bool TryConsumeFrame(RenderFrameCommands& out);

    /// 阻塞等待待处理帧（条件变量，不自旋）。`timeoutMs` 为 0 表示无限等待。
    [[nodiscard]] bool WaitConsumeFrame(RenderFrameCommands& out, u32 timeoutMs = 0u);

    /// 消费完一帧后回收票据（在飞帧数减一，唤醒可能被背压阻塞的游戏线程）
    void RetireFrame(const FrameTicket& ticket);

    /// **等待指定帧被消费（回收）**：阻塞直到 `frameIndex` 已被 `RetireFrame`，或超时。
    /// 【用途（T2.2 步骤 (c)）】严格握手：游戏线程发布本帧后等它真正做完（含呈现），
    /// 才继续下一帧的采集/装配 —— 这样"把每帧 RHI 调用搬进渲染命令"的改造可以**逐项验证**
    /// （每搬一项，转储都应保持逐位一致），而不必先实现完整的帧流水线。
    /// 【与 `SubmitFrameBlocking` 的区别】后者只保证"在飞帧数 < 上限"（背压），
    /// 不保证**本帧**已执行完 —— 严格握手必须用本函数。
    /// @param timeoutMs 0 = 无限等待
    /// @return 是否已确认该帧被回收
    [[nodiscard]] bool WaitFrameRetired(u64 frameIndex, u32 timeoutMs = 0u);

    // --- 观测 ---

    [[nodiscard]] u32  InFlightFrameCount() const;
    [[nodiscard]] u32  PendingFrameCount() const;
    [[nodiscard]] u32  MaxInFlight() const { return m_MaxInFlight; }
    /// 累计已发布帧数 / 已回收帧数（验收与单测用来核对"帧不丢、不重"）
    [[nodiscard]] u64  PublishedFrameCount() const;
    [[nodiscard]] u64  RetiredFrameCount() const;
    [[nodiscard]] u64  DroppedFrameCount() const { return m_Dropped; }
    /// 背压等待次数（T5.1 的"等待（背压）"计时会用它做交叉验证）
    [[nodiscard]] u64  BackpressureWaitCount() const;

    /// 清空所有未消费帧（仅在停止/异常路径使用；不回收票据）
    void Clear();

private:
    /// 收集态转发布态的内部实现（调用方必须持锁或自己保证互斥）
    void PublishLocked();

    mutable std::mutex      m_Mutex;
    std::condition_variable m_Cv;

    u32                     m_MaxInFlight = 3u;
    u32                     m_NextSlot    = 0u;

    // 收集中的帧（游戏线程写，渲染线程看不到）
    bool                    m_Collecting   = false;
    u64                     m_CollectFrame = 0;
    u32                     m_CollectSlot  = 0;
    std::vector<RenderCommand> m_Collecting_Commands;

    // 已发布、待消费的帧（整帧批量，FIFO）
    std::deque<RenderFrameCommands> m_Pending;

    u64 m_NextFrameIndex   = 0;
    u32 m_InFlight          = 0;   // 已发布但未 Retire 的帧数
    u64 m_Published         = 0;
    u64 m_Retired           = 0;
    /// 已退休到的帧号（帧按 FIFO 消费 ⇒ 退休也有序）。供严格握手 `WaitFrameRetired` 判断。
    u64 m_LastRetiredFrame  = 0;
    u64 m_Dropped           = 0;
    u64 m_BackpressureWaits = 0;
};

} // namespace he::render
