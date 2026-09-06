#pragma once

#include "Scene/Component.h"
#include "Scene/Entity.h"
#include "Math/Math.h"

#include <functional>
#include <vector>

// ============================================================
// AbilityComponent — 技能组件（简化 GAS，对应 UE5 UAbilitySystemComponent）
//
// 技能注册 / 冷却 / 释放 / 消耗：
//   - skills：技能定义列表（名称唯一）
//   - Cast：扣资源 + 进冷却 + 触发 onCast 回调（游戏逻辑挂载点）
//   - 冷却由 AbilitySystem::Update 每帧递减（cooldownRemaining 与 skills 下标对齐）
// 智能体对接：Action op "CastAbility"（CompileAction 编译为可撤销命令）。
// ============================================================

namespace he {

/// 技能定义（静态数据）
struct SkillDef {
    String name     = "";      // 技能名（唯一）
    float  cooldown = 1.0f;    // 冷却时间（秒）
    float  cost     = 0.0f;    // 消耗资源
};

class AbilityComponent : public Component {
    HE_COMPONENT()
public:
    // --- 技能列表（运行时注册）---
    std::vector<SkillDef> skills;

    // --- 资源 ---
    float resource    = 100.0f;   // 当前资源
    float maxResource = 100.0f;   // 资源上限

    // --- 施放回调（游戏逻辑挂载：如生成火球实体 + 抛射物移动）---
    std::function<void(Entity caster, const SkillDef& skill, const float3& target)> onCast;

    // --- 运行时冷却剩余（AbilitySystem::Update 递减；与 skills 下标对齐）---
    std::vector<float> cooldownRemaining;

    // --- 技能管理 ---
    int  AddSkill(const String& name, float cooldown, float cost);
    int  FindSkill(const String& name) const;

    /// 是否可施放：资源足够且冷却结束
    bool CanCast(int index) const {
        if (index < 0 || index >= (int)skills.size()) return false;
        return resource >= skills[index].cost && cooldownRemaining[index] <= 0.0f;
    }

    /// 施放技能：扣资源 + 进冷却 + 触发 onCast。
    /// @return 是否施放成功（不可施放时无副作用）
    bool Cast(int index, const float3& target);
};

} // namespace he
