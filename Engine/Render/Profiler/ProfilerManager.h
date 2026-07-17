#pragma once

#include "RHI/RHI.h"
#include "RHI/CommandList.h"
#include "RHI/QueryPool.h"
#include "Core/Types.h"
#include <vector>
#include <string>
#include <string_view>
#include <memory>

namespace he::render {

// ============================================================
// PipelineStatistics — GPU 管线硬件计数器
//
// VS/PS 调用次数、三角形数、裁剪数等关键指标
// 每个 Pass 可独立查询（通过 BeginQuery/EndQuery）
// ============================================================
struct PipelineStatistics {
    u64 inputVertices        = 0;  // IA 阶段输入顶点数
    u64 inputPrimitives      = 0;  // IA 阶段输入图元数
    u64 vsInvocations        = 0;  // Vertex Shader 调用次数
    u64 fsInvocations        = 0;  // Fragment Shader 调用次数
    u64 clippingInvocations  = 0;  // 裁剪前图元数
    u64 clippingPrimitives   = 0;  // 裁剪后图元数

    // --- 辅助计算 ---
    /// 像素过度着色率（>1 表示大量像素被重复着色）
    float GetVsToFsRatio() const {
        return vsInvocations > 0 ? float(fsInvocations) / float(vsInvocations) : 0.0f;
    }
    /// 图元裁剪率（0-1，值越大裁剪越多）
    float GetPrimitiveCullRate() const {
        return clippingInvocations > 0
            ? 1.0f - float(clippingPrimitives) / float(clippingInvocations)
            : 0.0f;
    }
    /// 平均每顶点覆盖的片元数
    float GetAvgFragmentsPerVertex() const {
        return inputVertices > 0 ? float(fsInvocations) / float(inputVertices) : 0.0f;
    }

    // 将 6 个计数器的原始 u64 数组解析为 PipelineStatistics
    static PipelineStatistics FromRawArray(const u64* data) {
        PipelineStatistics s;
        s.inputVertices       = data[0];
        s.inputPrimitives     = data[1];
        s.vsInvocations       = data[2];
        s.clippingInvocations = data[3];
        s.clippingPrimitives  = data[4];
        s.fsInvocations       = data[5];
        return s;
    }
    // Pipeline Statistics 每个查询返回的 u64 值的数量
    static constexpr u32 kNumCounters = 6;
};

// ============================================================
// ScopeData — 单个 Profiler Scope 的数据
//
// 支持嵌套：PushGroup/PopGroup 构建多层 scope 树
// depth=0 为 Pass 级别，depth>0 为子 scope
// ============================================================
struct ScopeData {
    std::string name;
    float       gpuMs     = 0.0f;
    u32         depth     = 0;          // 嵌套深度（0 = Pass 顶层）
    PipelineStatistics stats;           // 管线统计数据（仅当启用时有效）
};

// ============================================================
// PassBudget — 帧预算条目
// ============================================================
struct PassBudget {
    std::string namePattern;   // Pass 名称前缀匹配（如 "GB*" 匹配所有 GBuffer 相关）
    float       budgetMs = 16.6f;
};

// ============================================================
// ProfilerManager — GPU 时间戳 + Pipeline Statistics + 嵌套 Scope
//
// 每 Pass 2 个 timestamp × N Passes × 3 帧槽位
// 帧 N 写入 timestamp，帧 N+2 读回结果（3 帧延迟保证 GPU 完成）
//
// 使用方式（RenderGraph 自动集成）:
//   BeginFrame(cmd) → BeginPass(cmd, 0, "GBuffer") → PushGroup(cmd,"Mesh") →
//   PopGroup(cmd) → EndPass(cmd, 0) → ... → EndFrame(device)
//
// 兼容性: 原有 2 参数 Initialize 签名通过默认参数保持兼容
// ============================================================
class ProfilerManager {
public:
    ProfilerManager() = default;
    ~ProfilerManager();

    // --- 初始化/关闭 ---
    // enablePipelineStats: 启用 Pipeline Statistics 查询（有额外 GPU 开销）
    // maxGroupDepth: PushGroup/PopGroup 最大嵌套深度（0 = 禁用嵌套 scope）
    void Initialize(rhi::IRHIDevice* device, u32 maxPasses, u32 maxFramesInFlight = 3,
                    bool enablePipelineStats = false, u32 maxGroupDepth = 4);
    void Shutdown();

    // --- 帧级控制 ---
    /// 每帧开始时调用（重置 query pool + 读回 N-2 帧结果）
    void BeginFrame(rhi::IRHICommandList* cmd);
    /// Pass 开始前插入 start timestamp + 可选 pipeline stats begin
    void BeginPass(rhi::IRHICommandList* cmd, u32 passIndex, StringView name);
    /// Pass 结束后插入 end timestamp + 可选 pipeline stats end
    void EndPass(rhi::IRHICommandList* cmd, u32 passIndex);
    /// 帧结束后推进槽位指针
    void EndFrame(rhi::IRHIDevice* device);

    // --- 嵌套 Scope 支持 ---
    /// 开始一个子 Scope（必须在 BeginPass/EndPass 之间调用）
    /// 自动调用 cmd->BeginDebugLabel(name) + 插入子 scope 时间戳
    void PushGroup(rhi::IRHICommandList* cmd, StringView name);
    /// 结束当前子 Scope
    void PopGroup(rhi::IRHICommandList* cmd);

    // --- 帧预算系统 ---
    /// 设置 Pass 预算（毫秒）。namePattern 支持前缀匹配
    void SetPassBudget(StringView namePattern, float budgetMs);
    /// 移除 Pass 预算
    void RemovePassBudget(StringView namePattern);
    /// 检查所有 Pass 的预算合规性。超出预算时返回警告信息
    std::vector<std::string> CheckBudgets() const;

    // --- 数据访问 ---
    const std::vector<ScopeData>& GetLastFrameScopes() const { return m_LastScopes; }
    float GetTotalFrameMs() const { return m_LastFrameMs; }
    bool HasPipelineStats(u32 passIndex) const;

    // 保留兼容旧 API（返回 flat 数据）
    struct PassProfile {
        std::string name;
        float       gpuMs = 0.0f;
    };
    const std::vector<PassProfile>& GetLastFrameData() const { return m_LastProfiledCompat; }

    // --- 状态控制 ---
    void SetEnabled(bool enabled) { m_Enabled = enabled; }
    bool IsEnabled() const { return m_Enabled; }
    bool IsPipelineStatsEnabled() const { return m_UsePipelineStats; }

private:
    struct PerFrame {
        std::unique_ptr<rhi::IRHIQueryPool> timestampPool;    // 时间戳查询池（每 Pass 2 个）
        std::unique_ptr<rhi::IRHIQueryPool> statsPool;        // Pipeline Statistics 查询池（可选）
        std::vector<u64>  timestamps;     // [2*passIdx+0]=start, [2*passIdx+1]=end
        std::vector<u64>  statsData;      // PipelineStatistics 原始数据（passIdx * kNumCounters）
        std::vector<std::string> names;   // Pass 名称
        u32 passCount = 0;
    };

    rhi::IRHIDevice* m_Device = nullptr;
    u32 m_MaxPasses = 0;
    u32 m_MaxFramesInFlight = 3;
    u32 m_FrameIndex = 0;
    float m_TimestampPeriod = 0.0f;   // 时间戳单位（ns → ms 转换因数）
    bool  m_Enabled = true;
    bool  m_UsePipelineStats = false;
    u32   m_MaxGroupDepth = 0;

    std::vector<PerFrame> m_Frames;
    std::vector<ScopeData> m_LastScopes;
    float m_LastFrameMs = 0.0f;

    // 兼容旧 API
    mutable std::vector<PassProfile> m_LastProfiledCompat;

    // 嵌套 Scope 栈（帧内暂态，每次 BeginFrame 清空）
    struct ScopeStackEntry {
        u32  passIndex;
        u32  depth;
        u32  queryIndex;  // 子 scope 的 timestamp query 索引
    };
    std::vector<ScopeStackEntry> m_ScopeStack;

    // 子 Scope 时间戳分配
    u32 m_GroupTimestampCount = 0;       // 当前帧已分配的子 scope timestamp 数量
    static constexpr u32 kMaxGroupTimestamps = 128; // 每帧最多子 scope

    // 帧预算表
    std::vector<PassBudget> m_Budgets;
};

} // namespace he::render
