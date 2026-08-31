#include "AI/Agent/GoalComponent.h"

#include <algorithm>

namespace he::ai {

void GoalComponent::AddGoal(const String& description, f32 priority) {
    m_Goals.push_back({description, priority, false});
    // 按优先级降序（新目标插到合适位置）
    std::sort(m_Goals.begin(), m_Goals.end(),
              [](const Goal& a, const Goal& b) { return a.priority > b.priority; });
}

void GoalComponent::MarkAchieved(usize index) {
    if (index < m_Goals.size())
        m_Goals[index].achieved = true;
}

const Goal* GoalComponent::GetCurrentGoal() const {
    // 列表已按优先级降序，取第一个未达成的
    for (auto& g : m_Goals)
        if (!g.achieved) return &g;
    return nullptr;
}

usize GoalComponent::GetActiveGoalCount() const {
    usize n = 0;
    for (auto& g : m_Goals)
        if (!g.achieved) ++n;
    return n;
}

} // namespace he::ai
