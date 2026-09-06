// ============================================================
// AbilityComponent.cpp — 技能注册/查找/施放
// ============================================================

#include "Scene/AbilityComponent.h"

namespace he {

int AbilityComponent::AddSkill(const String& name, float cooldown, float cost) {
    // 同名技能直接复用（幂等注册）
    int existing = FindSkill(name);
    if (existing >= 0) return existing;
    skills.push_back({ name, cooldown, cost });
    cooldownRemaining.push_back(0.0f);
    return (int)skills.size() - 1;
}

int AbilityComponent::FindSkill(const String& name) const {
    for (usize i = 0; i < skills.size(); ++i)
        if (skills[i].name == name) return (int)i;
    return -1;
}

bool AbilityComponent::Cast(int index, const float3& target) {
    if (!CanCast(index)) return false;

    // 扣资源 + 进冷却（不可施放时无副作用）
    resource -= skills[index].cost;
    cooldownRemaining[index] = skills[index].cooldown;

    // 施放回调（游戏逻辑挂载点；caster = 所属实体）
    if (onCast) onCast(GetEntity(), skills[index], target);
    return true;
}

} // namespace he
