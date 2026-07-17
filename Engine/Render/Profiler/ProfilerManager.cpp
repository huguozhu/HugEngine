// Profiler/ProfilerManager.cpp — GPU 时间戳 + Pipeline Statistics Profiler
//
// 核心设计:
//   3 帧延迟读回（帧 N 写入 → 帧 N+2 读回），确保 GPU 完成查询写入
//   每个 Pass: 2 个 timestamp（start + end）
//   可选: Pipeline Statistics（6 个硬件计数器，独立 query pool）
//   嵌套 Scope: PushGroup/PopGroup 通过额外 query 插槽实现（最多 128 个/帧）
//
#include "ProfilerManager.h"
#include "Core/Log.h"
#include <cstring>
#include <algorithm>

namespace he::render {

ProfilerManager::~ProfilerManager() { Shutdown(); }

// ============================================================
// Initialize — 分配多帧查询池
// ============================================================
void ProfilerManager::Initialize(rhi::IRHIDevice* device, u32 maxPasses, u32 maxFramesInFlight,
                                 bool enablePipelineStats, u32 maxGroupDepth) {
    m_Device            = device;
    m_MaxPasses         = maxPasses;
    m_MaxFramesInFlight = maxFramesInFlight;
    m_UsePipelineStats  = enablePipelineStats;
    m_MaxGroupDepth     = maxGroupDepth;
    m_TimestampPeriod   = device->GetTimestampPeriod() * 1e-6f;  // ns → ms

    m_Frames.resize(maxFramesInFlight);
    for (u32 i = 0; i < maxFramesInFlight; ++i) {
        // 时间戳查询池：每 Pass 2 个 + 每子 Scope 额外槽位
        u32 totalTimestamps = maxPasses * 2 + kMaxGroupTimestamps * 2;
        m_Frames[i].timestampPool = device->CreateQueryPool(totalTimestamps, rhi::QueryType::Timestamp);
        m_Frames[i].timestamps.resize(totalTimestamps);

        // Pipeline Statistics 查询池：可选，每 Pass 1 个
        if (m_UsePipelineStats) {
            m_Frames[i].statsPool = device->CreateQueryPool(maxPasses, rhi::QueryType::PipelineStatistics);
            m_Frames[i].statsData.resize(maxPasses * PipelineStatistics::kNumCounters);
        }
        m_Frames[i].names.resize(maxPasses);
    }
    m_LastScopes.resize(maxPasses);
    m_LastProfiledCompat.resize(maxPasses);
    m_FrameIndex = 0;
    m_GroupTimestampCount = 0;

    HE_CORE_INFO("ProfilerManager 初始化: {} passes, {} frames, pipelineStats={}, groupDepth={}",
                 maxPasses, maxFramesInFlight, enablePipelineStats, maxGroupDepth);
}

void ProfilerManager::Shutdown() {
    m_Frames.clear();
    m_LastScopes.clear();
    m_ScopeStack.clear();
    m_Budgets.clear();
    m_Device = nullptr;
}

// ============================================================
// BeginFrame — 重置查询池 + 读回 N-2 帧结果
// ============================================================
void ProfilerManager::BeginFrame(rhi::IRHICommandList* cmd) {
    if (!m_Device || !m_Enabled) return;

    auto& frame = m_Frames[m_FrameIndex];
    frame.passCount = 0;
    m_GroupTimestampCount = 0;
    m_ScopeStack.clear();

    // 重置当前帧 query pool
    cmd->ResetQueryPool(frame.timestampPool.get());
    if (m_UsePipelineStats && frame.statsPool) {
        cmd->ResetQueryPool(frame.statsPool.get());
    }

    // 读回 2 帧前的结果（延迟 2 帧确保 GPU 已完成）
    u32 rbIdx = (m_FrameIndex + m_MaxFramesInFlight - 2) % m_MaxFramesInFlight;
    auto& rbFrame = m_Frames[rbIdx];

    if (rbFrame.passCount > 0) {
        // 读回时间戳
        u32 tsCount = rbFrame.passCount * 2;
        cmd->GetQueryResults(rbFrame.timestampPool.get(), 0, tsCount,
                             rbFrame.timestamps.data());

        // 读回 Pipeline Statistics
        if (m_UsePipelineStats && rbFrame.statsPool) {
            u32 statsCount = rbFrame.passCount * PipelineStatistics::kNumCounters;
            cmd->GetQueryResults(rbFrame.statsPool.get(), 0, statsCount,
                                 rbFrame.statsData.data());
        }

        // 解析为 ScopeData
        float totalMs = 0.0f;
        for (u32 i = 0; i < rbFrame.passCount && i < m_MaxPasses; ++i) {
            u64 start = rbFrame.timestamps[i * 2];
            u64 end   = rbFrame.timestamps[i * 2 + 1];
            float ms  = float(end - start) * m_TimestampPeriod;

            m_LastScopes[i].name   = rbFrame.names[i].empty() ? "?" : rbFrame.names[i];
            m_LastScopes[i].gpuMs  = ms;
            m_LastScopes[i].depth  = 0;

            // 解析 PipelineStatistics（如果有）
            if (m_UsePipelineStats && rbFrame.statsPool) {
                u32 offset = i * PipelineStatistics::kNumCounters;
                m_LastScopes[i].stats = PipelineStatistics::FromRawArray(
                    &rbFrame.statsData[offset]);
            } else {
                m_LastScopes[i].stats = PipelineStatistics{};
            }

            // 兼容旧 API
            m_LastProfiledCompat[i].name  = m_LastScopes[i].name;
            m_LastProfiledCompat[i].gpuMs = ms;

            totalMs += ms;
        }
        m_LastFrameMs = totalMs;

        // 标记未使用的槽位
        for (u32 i = rbFrame.passCount; i < m_MaxPasses; ++i) {
            m_LastScopes[i].gpuMs = -1.0f;
            m_LastProfiledCompat[i].gpuMs = -1.0f;
        }
    }
}

// ============================================================
// BeginPass — 写入 start timestamp + 可选的 stats begin
// ============================================================
void ProfilerManager::BeginPass(rhi::IRHICommandList* cmd, u32 passIndex, StringView name) {
    if (!m_Enabled) return;
    auto& frame = m_Frames[m_FrameIndex];
    if (passIndex >= m_MaxPasses) return;

    // 写入 start timestamp
    cmd->WriteTimestamp(frame.timestampPool.get(), passIndex * 2);

    // 开始 Pipeline Statistics 查询
    if (m_UsePipelineStats && frame.statsPool) {
        cmd->BeginQuery(frame.statsPool.get(), passIndex);
    }

    frame.names[passIndex] = name;
    frame.passCount = std::max(frame.passCount, passIndex + 1);
}

// ============================================================
// EndPass — 结束 stats query + 写入 end timestamp
// ============================================================
void ProfilerManager::EndPass(rhi::IRHICommandList* cmd, u32 passIndex) {
    if (!m_Enabled) return;
    auto& frame = m_Frames[m_FrameIndex];
    if (passIndex >= m_MaxPasses) return;

    // 结束 Pipeline Statistics 查询（在 timestamp 之前）
    if (m_UsePipelineStats && frame.statsPool) {
        cmd->EndQuery(frame.statsPool.get(), passIndex);
    }

    // 写入 end timestamp
    cmd->WriteTimestamp(frame.timestampPool.get(), passIndex * 2 + 1);
}

// ============================================================
// EndFrame — 推进帧槽位
// ============================================================
void ProfilerManager::EndFrame(rhi::IRHIDevice* device) {
    (void)device;
    m_FrameIndex = (m_FrameIndex + 1) % m_MaxFramesInFlight;
}

// ============================================================
// PushGroup / PopGroup — 嵌套 Scope 支持
// ============================================================
void ProfilerManager::PushGroup(rhi::IRHICommandList* cmd, StringView name) {
    if (!m_Enabled || m_MaxGroupDepth == 0) return;
    auto& frame = m_Frames[m_FrameIndex];
    u32 depth = static_cast<u32>(m_ScopeStack.size());

    if (depth >= m_MaxGroupDepth) {
        HE_CORE_WARN("ProfilerManager::PushGroup: 超出最大嵌套深度 {} (scope='{}')",
                     m_MaxGroupDepth, name);
        return;
    }

    // 分配子 scope 的时间戳索引（在 Pass 的 2 个基础 timestamp 之后）
    u32 baseIdx = m_MaxPasses * 2 + m_GroupTimestampCount * 2;
    if (m_GroupTimestampCount >= kMaxGroupTimestamps) {
        HE_CORE_WARN("ProfilerManager::PushGroup: 超出最大子 scope 数量 {} (scope='{}')",
                     kMaxGroupTimestamps, name);
        return;
    }

    // 记录到 scope 栈
    ScopeStackEntry entry;
    entry.passIndex  = frame.passCount;  // 当前正在录制的 Pass
    entry.depth      = depth;
    entry.queryIndex = baseIdx;
    m_ScopeStack.push_back(entry);

    // 写入子 scope start timestamp
    cmd->WriteTimestamp(frame.timestampPool.get(), baseIdx);

    // 同时插入 Debug Label
    cmd->BeginDebugLabel(name.data());

    m_GroupTimestampCount++;
}

void ProfilerManager::PopGroup(rhi::IRHICommandList* cmd) {
    if (!m_Enabled || m_ScopeStack.empty()) return;

    // 结束 Debug Label
    cmd->EndDebugLabel();

    // 写入子 scope end timestamp
    ScopeStackEntry& entry = m_ScopeStack.back();
    u32 endIdx = entry.queryIndex + 1;
    cmd->WriteTimestamp(m_Frames[m_FrameIndex].timestampPool.get(), endIdx);

    m_ScopeStack.pop_back();
}

// ============================================================
// SetPassBudget / CheckBudgets — 帧预算系统
// ============================================================
void ProfilerManager::SetPassBudget(StringView namePattern, float budgetMs) {
    // 先删除同名规则
    RemovePassBudget(namePattern);
    m_Budgets.push_back({std::string(namePattern), budgetMs});
    HE_CORE_INFO("ProfilerManager: 设置预算 '{}' = {:.2f}ms", namePattern, budgetMs);
}

void ProfilerManager::RemovePassBudget(StringView namePattern) {
    m_Budgets.erase(
        std::remove_if(m_Budgets.begin(), m_Budgets.end(),
            [&](const PassBudget& b) { return b.namePattern == namePattern; }),
        m_Budgets.end());
}

std::vector<std::string> ProfilerManager::CheckBudgets() const {
    std::vector<std::string> warnings;
    for (auto& scope : m_LastScopes) {
        if (scope.gpuMs < 0) continue;
        for (auto& budget : m_Budgets) {
            // 前缀匹配
            if (scope.name.size() >= budget.namePattern.size() &&
                scope.name.compare(0, budget.namePattern.size(), budget.namePattern) == 0) {
                float ratio = scope.gpuMs / budget.budgetMs;
                if (ratio > 0.8f) {
                    char buf[256];
                    snprintf(buf, sizeof(buf),
                        "[Profiler] '%s' 超预算: %.2fms/%.2fms (%.0f%%)",
                        scope.name.c_str(), scope.gpuMs, budget.budgetMs, ratio * 100);
                    warnings.push_back(buf);
                }
            }
        }
    }
    return warnings;
}

// ============================================================
// HasPipelineStats — 查询指定 Pass 是否有有效统计数据
// ============================================================
bool ProfilerManager::HasPipelineStats(u32 passIndex) const {
    if (passIndex >= m_LastScopes.size()) return false;
    auto& s = m_LastScopes[passIndex];
    if (s.gpuMs < 0) return false;
    return s.stats.vsInvocations > 0 || s.stats.fsInvocations > 0;
}

} // namespace he::render
