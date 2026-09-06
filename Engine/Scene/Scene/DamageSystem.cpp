#include "Scene/DamageSystem.h"

#include "Scene/HealthComponent.h"
#include "Scene/World.h"
#include "Core/Log.h"

#include <algorithm>

namespace he {

namespace {
// 把生命值钳制到 [0, maxHealth]（maxHealth 为负时视为 0 血）
void ClampHealth(HealthComponent& h) {
    h.currentHealth = std::clamp(h.currentHealth, 0.0f, std::max(h.maxHealth, 0.0f));
}
} // namespace

bool DamageSystem::ApplyDamage(World& world, Entity e, float damage) {
    auto* h = world.GetComponent<HealthComponent>(e);
    if (!h) {
        HE_CORE_WARN("[DamageSystem] 实体 {} 无 HealthComponent，伤害被忽略", e.id);
        return false;
    }

    // 负伤害 = 治疗（统一入口，调用方无需区分）
    if (damage < 0.0f) {
        Heal(world, e, -damage);
        return h->IsDead();
    }

    // 无敌免疫伤害（治疗不受影响）
    if (h->bInvincible) return false;

    bool wasDead = h->IsDead();
    h->currentHealth -= damage;
    ClampHealth(*h);

    // 受伤回调（含实际造成的伤害）
    if (h->onDamaged) h->onDamaged(e, damage);

    // 死亡回调（只在本次伤害导致死亡时触发一次，不重复触发）
    if (!wasDead && h->IsDead() && h->onDeath) h->onDeath(e);

    return h->IsDead();
}

void DamageSystem::Heal(World& world, Entity e, float amount) {
    auto* h = world.GetComponent<HealthComponent>(e);
    if (!h) {
        HE_CORE_WARN("[DamageSystem] 实体 {} 无 HealthComponent，治疗被忽略", e.id);
        return;
    }
    h->currentHealth += amount;
    ClampHealth(*h);
}

void DamageSystem::ResetHealth(World& world, Entity e) {
    auto* h = world.GetComponent<HealthComponent>(e);
    if (!h) {
        HE_CORE_WARN("[DamageSystem] 实体 {} 无 HealthComponent，重置被忽略", e.id);
        return;
    }
    h->currentHealth = h->maxHealth;
}

} // namespace he
