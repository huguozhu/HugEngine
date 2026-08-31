#pragma once

#include "Scene/Component.h"
#include "Core/Types.h"
#include "Containers/Array.h"

// ============================================================
// GoalComponent — 智能体目标组件
//
// 目标列表 + 优先级，供 IBrain 决策时参考。
// MVP：简单目标队列（按优先级降序），实现状态标记。
// ============================================================

namespace he::ai {

/// 单个目标
struct Goal {
    String description;   // 目标描述（自然语言）
    f32    priority  = 0.0f;   // 优先级（越大越优先）
    bool   achieved  = false;  // 是否已达成
};

/// 目标组件：挂在智能体实体上
class GoalComponent : public he::Component {
    HE_COMPONENT()
public:
    /// 添加目标（自动按优先级降序插入）
    void AddGoal(const String& description, f32 priority);

    /// 标记第 index 个目标为已达成
    void MarkAchieved(usize index);

    /// 当前最高优先级未达成目标（无则返回 nullptr）
    const Goal* GetCurrentGoal() const;

    const TArray<Goal>& GetGoals() const { return m_Goals; }
    usize GetActiveGoalCount() const;

private:
    TArray<Goal> m_Goals;   // 按优先级降序
};

} // namespace he::ai
