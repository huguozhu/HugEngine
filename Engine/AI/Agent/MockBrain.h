#pragma once

#include "AI/Agent/Brain.h"

// ============================================================
// MockBrain — 固定计划大脑（演示/回放/测试用）
//
// 不依赖 LLM：Decide 始终返回构造时给定的动作计划。
// 用于：AgentSystem 的"Mock"策略、单元测试、无网络演示。
// ============================================================

namespace he::ai {

class MockBrain : public IBrain {
public:
    explicit MockBrain(ActionPlan plan) : m_Plan(std::move(plan)) {}

    /// 返回固定的动作计划（忽略观察与记忆）
    ActionPlan Decide(const String&, const MemoryComponent*) override { return m_Plan; }

private:
    ActionPlan m_Plan;   // 固定计划
};

} // namespace he::ai
