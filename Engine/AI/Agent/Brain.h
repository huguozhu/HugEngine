#pragma once

#include "Core/Types.h"
#include "Containers/Array.h"
#include "AI/WorldModel/Action.h"

// ============================================================
// Brain.h — 大脑策略抽象
//
// LLM / RL / 行为树可插拔：Decide 依据观察 + 记忆产出动作计划，
// 动作经 CompileAction 编译为 he::Command（可撤销）。
// ============================================================

namespace he::ai {

class MemoryComponent;

/// 一次决策产生的动作计划（可包含多个动作）
struct ActionPlan {
    TArray<Action> actions;
};

/// 大脑策略抽象 —— LLM / RL / 行为树 可插拔
class IBrain {
public:
    virtual ~IBrain() = default;

    /// 依据观察（语义快照 JSON）+ 记忆，产出动作计划
    /// @param observationJson WorldModel::Snapshot 输出
    /// @param memory 实体记忆（可空）
    virtual ActionPlan Decide(const String& observationJson, const MemoryComponent* memory) = 0;
};

} // namespace he::ai
