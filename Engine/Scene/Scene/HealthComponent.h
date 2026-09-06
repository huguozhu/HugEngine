#pragma once

#include "Scene/Component.h"
#include "Scene/Entity.h"

#include <functional>

// ============================================================
// HealthComponent — 生命值（对应 UE5 UHealthComponent 等玩法数据组件）
//
// 纯数据组件：最大生命/当前生命/无敌标记 + 受伤/死亡事件回调。
// 数值操作统一走 DamageSystem（ApplyDamage / Heal / ResetHealth），
// 保证钳制 [0, maxHealth] 与回调触发语义一致。
// ============================================================

namespace he {

class HealthComponent : public Component {
    HE_COMPONENT()
public:
    float maxHealth     = 100.0f;   // 最大生命值
    float currentHealth = 100.0f;   // 当前生命值（始终钳制在 [0, maxHealth]）
    bool  bInvincible   = false;    // 无敌：免疫一切伤害（治疗仍生效）

    // --- 事件回调（由 DamageSystem 触发）---
    std::function<void(Entity, float)> onDamaged;  // 受伤（实体, 实际伤害值）
    std::function<void(Entity)>        onDeath;    // 死亡（生命值降到 0 时触发一次）

    bool IsDead() const { return currentHealth <= 0.0f; }
};

} // namespace he
