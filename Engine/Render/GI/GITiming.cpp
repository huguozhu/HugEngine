// GI/GITiming.cpp — GI 各源的 GPU 耗时读数（任务 29 / §9.2-Z）
#include "GI/GITiming.h"
#include "Core/Log.h"
#include <algorithm>
#include <cstdlib>

namespace he::render {

bool GITimer::Initialize(rhi::IRHIDevice* device, float timestampPeriodNs) {
    if (!device) return false;
    m_TimestampPeriodNs = (timestampPeriodNs > 0.0f) ? timestampPeriodNs : 1.0f;

    m_Ring.clear();
    for (u32 i = 0; i < kRingSize; ++i) {
        Ring r;
        r.pool = device->CreateQueryPool(kMaxSources * kStampsPerSource, rhi::QueryType::Timestamp);
        if (!r.pool) {
            HE_CORE_WARN("GITiming: 查询池创建失败（环形槽 {}），GI 耗时读数不可用", i);
            m_Ring.clear();
            return false;
        }
        m_Ring.push_back(std::move(r));
    }
    m_Next = 0;
    m_RoundOpen = false;
    m_AvgMs.assign(kMaxSources, 0.0f);
    m_PeakMs.assign(kMaxSources, 0.0f);
    m_LastMs.assign(kMaxSources, 0.0f);
    HE_CORE_INFO("GITiming: 已启用 GPU 耗时读数（{} 个环形查询池 × {} 源，时间戳周期 {:.1f} ns）",
                 kRingSize, kMaxSources, m_TimestampPeriodNs);
    return true;
}

void GITimer::Shutdown() {
    m_Ring.clear();
    m_AvgMs.clear();
    m_PeakMs.clear();
    m_LastMs.clear();
    m_Next = 0;
    m_RoundOpen = false;
}

bool GITimer::BeginFrame(rhi::IRHICommandList* cmd) {
    m_RoundOpen = false;
    if (!IsReady() || !cmd) return false;

    Ring& r = m_Ring[m_Next];

    if (r.used) {
        if (!r.resolved) {
            // 只读**本轮实际写过**的源，而且**逐源**判断可用性。
            // 这一步是关键：整池一次性读会把"从未写过的查询"也算进来，它们的可用标志永远是 0，
            // 于是"整池可用"永远为假 —— 读数就会一直停在 0（第一版实现正是这样）。
            bool allResolved = true;
            for (u32 s = 0; s < kMaxSources; ++s) {
                if (!r.written[s]) continue;
                u64 d[2] = { 0, 0 };
                if (!cmd->TryGetQueryResults(r.pool.get(), s * kStampsPerSource,
                                             kStampsPerSource, d)) {
                    allResolved = false;      // 这一源的结果还没执行到：整轮留到下一帧再试
                    continue;
                }
                if (d[1] > d[0]) {
                    const float ms = float(double(d[1] - d[0]) * double(m_TimestampPeriodNs) / 1e6);
                    m_LastMs[s] = ms;
                    m_AvgMs[s]  = (m_AvgMs[s] > 0.0f) ? (m_AvgMs[s] * 0.9f + ms * 0.1f) : ms;
                    m_PeakMs[s] = std::max(m_PeakMs[s], ms);
                    r.measured[s] = true;
                }
            }
            if (!allResolved) {
                // 拿不到就跳过这一帧（不阻塞、也不复位），下一帧再试同一个环形槽
                m_Next = (m_Next + 1u) % kRingSize;
                return false;
            }
            // 【读数要跟着"现在还有没有在跑"走】本轮没量到非零区间的源，其平均值向 0 衰减。
            // 覆盖两种情形：pass 未注册（源没启用）与 pass 注册了但内部直接返回
            // （IBL 不在脏时）。否则像 IBL 这种"只在脏时烘焙"的源会永远把第一次烘焙的
            // 耗时（实测约 5 ms）挂在面板上，看起来像每帧都在付这个成本。
            const float kDecay = 0.9f;
            for (u32 s = 0; s < kMaxSources; ++s) {
                if (!r.measured[s] && m_AvgMs[s] > 0.0f) {
                    m_AvgMs[s] *= kDecay;
                    if (m_AvgMs[s] < 0.001f) { m_AvgMs[s] = 0.0f; m_PeakMs[s] = 0.0f; }
                }
            }
            r.resolved = true;
        }
        // 结果已读回 ⇒ 这批查询所属的提交已经执行完 ⇒ 复位安全
        cmd->ResetQueryPool(r.pool.get());
    } else {
        // 首次使用也要复位：查询池创建后处于未初始化状态
        cmd->ResetQueryPool(r.pool.get());
    }

    r.used = true;
    r.resolved = false;
    std::fill(std::begin(r.written), std::end(r.written), false);
    std::fill(std::begin(r.measured), std::end(r.measured), false);
    std::fill(std::begin(r.closed),  std::end(r.closed),  false);
    m_RoundOpen = true;
    return true;
}

void GITimer::Begin(rhi::IRHICommandList* cmd, u32 idx) {
    if (!m_RoundOpen || !cmd || idx >= kMaxSources) return;
    Ring& r = m_Ring[m_Next];
    cmd->WriteTimestamp(r.pool.get(), idx * kStampsPerSource + 0);
    r.written[idx] = true;
    r.closed[idx]  = true;
}

void GITimer::End(rhi::IRHICommandList* cmd, u32 idx) {
    if (!m_RoundOpen || !cmd || idx >= kMaxSources) return;
    Ring& r = m_Ring[m_Next];
    if (!r.closed[idx]) return;                  // 没记起始就不记结束
    cmd->WriteTimestamp(r.pool.get(), idx * kStampsPerSource + 1);
    r.closed[idx] = false;
}

} // namespace he::render
