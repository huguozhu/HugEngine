#pragma once

#include "Core/Types.h"

// ============================================================
// DamageSystem — 生命值操作（静态系统，玩法数值底座）
//
// 所有 HealthComponent 的数值修改统一走本系统：
//   ApplyDamage：扣血（负数=治疗）；bInvincible 免疫；死亡触发 onDeath
//   Heal：回血（钳制到 maxHealth）
//   ResetHealth：重置到满血
// 回调触发顺序：onDamaged → （若死亡）onDeath。
// ============================================================

namespace he {
struct Entity;
class World;

class DamageSystem {
public:
    /// 造成伤害。@return 是否死亡（本次伤害后生命值 <= 0）
    static bool ApplyDamage(World& world, Entity e, float damage);

    /// 治疗（伤害为负时 ApplyDamage 内部也走此路径）
    static void Heal(World& world, Entity e, float amount);

    /// 重置到满血
    static void ResetHealth(World& world, Entity e);
};

} // namespace he
