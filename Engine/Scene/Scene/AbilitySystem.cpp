// ============================================================
// AbilitySystem.cpp — 技能冷却计时
// ============================================================

#include "Scene/AbilitySystem.h"

#include "Scene/AbilityComponent.h"
#include "Scene/World.h"

#include <algorithm>

namespace he {

void AbilitySystem::Update(World& world, f32 dt) {
    if (dt <= 0.0f) return;

    world.ForEach<AbilityComponent>([&](Entity, AbilityComponent& ability) {
        // 冷却剩余递减并钳制到 0（0 = 可施放）
        for (float& cd : ability.cooldownRemaining) {
            cd = std::max(0.0f, cd - dt);
        }
    });
}

} // namespace he
