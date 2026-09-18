#pragma once

// ============================================================
// GI/GITiming.h — GI 各源的 GPU 耗时读数（任务 29 / §9.2-Z）
//
// 为什么需要它：面板上一直有「SSGI 耗时 / DDGI 耗时 / IBL 耗时 / SSR 耗时」四行，
// 而 `GIDebugData::avgRenderTimeMs` **全仓没有一处赋值** —— 那四行恒显示 0.00 ms。
// 它长得像一个可用的性能读数，于是性能类任务（时间维分摊、pass 级剔除、DDGI march 换实现）
// 都会自然地去读它，然后得到一个恒为 0 的答案。测量读数错了，优化就无从判定。
//
// 【怎么读才安全】这里是本类最容易踩坑的地方，写清楚以免后来者重犯：
//   · 帧图会把 pass 录到**两条**命令列表上（图形一条、异步计算一条），两条各自推进自己的
//     飞行帧索引。因此"某组时间戳是否已经提交、是否已经执行完"从调用方看不出来。
//   · `IRHICommandList::GetQueryResults`带着 `WAIT_BIT`，对还没执行到的查询会**永久阻塞**
//     （第一版实现就是这样把进程挂死在第三帧的）。所以这里只用**不阻塞**的
//     `TryGetQueryResults`：拿不到就跳过这一帧，下一帧再试。
//   · 查询池只在"上一轮结果已成功读回"之后才复位 —— 读回成功本身就证明那批查询所属的提交
//     已经执行完，于是"复位一个仍在飞行中的查询"这件事从结构上不可能发生。
//   · 池子用**环形**分配（不是按飞行帧索引），因为多命令列表下没有单一可信的帧索引。
//
// 覆盖范围：**只计各源的主 pass**（本体开销的大头）。附属降噪 pass 暂不计入，
// 以免把读数当成"整个源的全部成本"。
// ============================================================

#include "RHI/RHI.h"
#include <memory>
#include <vector>

namespace he::render {

class GITimer {
public:
    /// 支持的最大源数（与帧图的 Provider 数同量级即可）
    static constexpr u32 kMaxSources = 32;
    /// 非"源"的公共项（当前只有 TLAS 构建）使用这个保留下标
    static constexpr u32 kCommonItemIdx = kMaxSources - 1;
    /// 每源一对时间戳（起 / 止）
    static constexpr u32 kStampsPerSource = 2;
    /// 环形池个数：足够让"写入 → 隔几帧读回"错开，又不至于占太多查询
    static constexpr u32 kRingSize = 6;

    bool Initialize(rhi::IRHIDevice* device, float timestampPeriodNs);
    void Shutdown();
    [[nodiscard]] bool IsReady() const { return !m_Ring.empty(); }

    /// 每帧开始调用一次（在命令列表 Begin 之后、录制任何 pass 之前）：
    /// 先试着读回当前环形槽上一轮的结果；**只有读回成功（或该槽从未使用）才复位并开始新一轮**，
    /// 否则本帧跳过计时（不写时间戳）。返回值：本帧是否开始了新的一轮测量。
    bool BeginFrame(rhi::IRHICommandList* cmd);

    /// 在某个源的主 pass 前后各调一次（idx 由帧图按 Provider 注册顺序给出）
    void Begin(rhi::IRHICommandList* cmd, u32 idx);
    void End(rhi::IRHICommandList* cmd, u32 idx);

    /// 滚动平均 / 峰值 / 最近一次原始读数（未测到过的源返回 0）
    [[nodiscard]] float AvgMs(u32 idx) const  { return idx < m_AvgMs.size()  ? m_AvgMs[idx]  : 0.0f; }
    [[nodiscard]] float PeakMs(u32 idx) const { return idx < m_PeakMs.size() ? m_PeakMs[idx] : 0.0f; }
    [[nodiscard]] float LastMs(u32 idx) const { return idx < m_LastMs.size() ? m_LastMs[idx] : 0.0f; }

private:
    /// 一个环形槽：一个查询池 + 它当前这一轮的写入进度
    struct Ring {
        std::unique_ptr<rhi::IRHIQueryPool> pool;
        bool used     = false;   // 写过时间戳
        bool resolved = false;   // 结果已成功读回 ⇒ 可以安全复位
        /// 本轮**实际写过**起始时间戳的源（只有这些源才去读它的结果：
        /// 未写过的查询永远不会有"可用"标志，按整池可用性判断会永远等不到结果）
        bool written[kMaxSources] = {};
        /// 本轮**量到了非零区间**的源。pass 被注册但内部直接返回（例如 IBL 不在脏时）
        /// 会写出两个相同的时间戳 —— 那种情况算"本帧没跑"，读数应向 0 衰减，
        /// 否则面板会把第一次烘焙的耗时永远挂着（实测 5 ms）。
        bool measured[kMaxSources] = {};
        bool closed[kMaxSources]  = {};   // 已写起始、待写结束
    };

    float m_TimestampPeriodNs = 1.0f;
    std::vector<Ring> m_Ring;
    u32   m_Next = 0;            // 本帧使用的环形槽
    bool  m_RoundOpen = false;   // 本帧是否真的开了新一轮（决定要不要写时间戳）

    std::vector<float> m_AvgMs;
    std::vector<float> m_PeakMs;
    std::vector<float> m_LastMs;
};

} // namespace he::render
