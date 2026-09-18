#pragma once

#include "Core/Types.h"
#include "Math/Math.h"
#include "Scene/Entity.h"
#include "Scene/CollisionComponent.h"   // CollisionShape（世界形状结构需要它）

// ============================================================
// CollisionSystem — CPU 碰撞检测（静态系统，物理与移动的前置）
//
// 覆盖：Overlap（三种形状全组合：AABB/Sphere/Capsule）、
//       Contains（点包含）、Raycast（射线最近命中）。
// 实体缺失 CollisionComponent 或 bEnabled=false 时一律跳过（容错）。
//
// 任务 26：把"世界空间形状提取"暴露成 `ExtractWorldShape`，供调试线框（CollisionDebugSystem）
// 复用**同一份形状语义** —— 线框画出来的必须就是检测用的那个形状，否则调试可视化会骗人。
// ============================================================

namespace he {
struct Entity;
class World;

/// 提取后的世界空间碰撞形状（任务 26 起对外公开）
struct CollisionWorldShape {
    CollisionShape shape = CollisionShape::AABB;
    float3 min = float3(0.0f), max = float3(0.0f);  // AABB 世界轴对齐范围（旋转后重轴，保守）
    float3 center = float3(0.0f);                  // Sphere 球心 / Capsule 段中点
    float  radius = 0.0f;                          // Sphere / Capsule 半径
    float3 segA = float3(0.0f), segB = float3(0.0f); // Capsule 段两端点
};

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

    /// 提取实体碰撞体的世界空间形状（缺失/禁用/无 Transform 时返回 false）
    /// 【谁在用】检测内部 + 任务 26 的调试线框（保证线框与真实碰撞体一致）
    static bool ExtractWorldShape(World& world, Entity e, CollisionWorldShape& out);
};

} // namespace he
