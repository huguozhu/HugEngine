#pragma once

#include "Core/Types.h"
#include "Math/Math.h"
#include "Scene/Entity.h"

// ============================================================
// CollisionSystem — CPU 碰撞检测（静态系统，物理与移动的前置）
//
// 覆盖：Overlap（三种形状全组合：AABB/Sphere/Capsule）、
//       Contains（点包含）、Raycast（射线最近命中）。
// 实体缺失 CollisionComponent 或 bEnabled=false 时一律跳过（容错）。
// ============================================================

namespace he {
struct Entity;
class World;

class CollisionSystem {
public:
    /// 两个实体的碰撞体积是否重叠（缺失/禁用返回 false）
    static bool Overlap(World& world, Entity a, Entity b);

    /// 世界空间点是否位于实体碰撞体积内
    static bool Contains(World& world, Entity e, const float3& point);

    /// 射线检测：返回沿方向 dir 最近的命中实体与距离 t（0~maxDistance）。
    /// @param ignore 跳过的实体（如角色地面检测时排除自身碰撞体）
    /// @param outNormal 命中点法线（可空；供坡度/弹射角判断）
    /// @return 是否命中；无命中时 outHit/outT 不变
    static bool Raycast(World& world, const float3& origin, const float3& dir,
                        float maxDistance, Entity& outHit, float& outT,
                        EntityID ignore = kInvalidEntity, float3* outNormal = nullptr);
};

} // namespace he
