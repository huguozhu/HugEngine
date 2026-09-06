// ============================================================
// MovementSystem.cpp — 角色移动积分 + 地面检测
// ============================================================

#include "Scene/MovementSystem.h"

#include "Scene/CharacterMovementComponent.h"
#include "Scene/CollisionComponent.h"
#include "Scene/CollisionSystem.h"
#include "Scene/Transform.h"
#include "Scene/World.h"

#include <algorithm>
#include <cmath>

namespace he {

namespace {

// 地面检测容差（米）：射线最大距离 = 半高 + 容差，允许轻微下落贴地
constexpr float kGroundTolerance = 0.2f;

// 从实体碰撞体推导角色半高（无碰撞体时用默认 0.5）
float GetHalfHeight(World& world, Entity e) {
    auto* cc = world.GetComponent<CollisionComponent>(e);
    if (!cc) return 0.5f;
    switch (cc->shape) {
    case CollisionShape::Capsule: return cc->height * 0.5f;
    case CollisionShape::Sphere:  return cc->radius;
    case CollisionShape::AABB:    return cc->halfExtents.y;
    }
    return 0.5f;
}

} // namespace

void MovementSystem::Update(World& world, f32 dt) {
    if (dt <= 0.0f) return;

    world.ForEach<CharacterMovementComponent>([&](Entity e, CharacterMovementComponent& m) {
        auto* xf = world.GetComponent<TransformComponent>(e);
        if (!xf) return;
        float halfHeight = GetHalfHeight(world, e);

        // 1. 水平速度：期望方向（长度 >1 归一化）× 速度（MVP 直接赋值，无加速度平滑）
        float2 wish = m.inputDirection;
        float wishLenSq = wish.x * wish.x + wish.y * wish.y;
        if (wishLenSq > 1.0f) wish /= std::sqrt(wishLenSq);
        float speed = m.bRunning ? m.runSpeed : m.walkSpeed;
        m.velocity.x = wish.x * speed;
        m.velocity.z = wish.y * speed;

        // 2. 跳跃：边沿触发消费；仅地面可起跳（空中请求忽略，无二段跳）
        if (m.bWantsJump) {
            m.bWantsJump = false;
            if (m.bOnGround) {
                // 初速 = √(2·g·h)（能量守恒，跳跃高度近似 jumpHeight）
                m.velocity.y = std::sqrt(2.0f * m.gravity * std::max(m.jumpHeight, 0.0f));
                m.bOnGround = false;
            }
        }

        // 3. 重力积分（空中）
        if (!m.bOnGround) {
            m.velocity.y -= m.gravity * dt;
        }

        // 4. 位置积分
        xf->position += m.velocity * dt;

        // 5. 地面检测：下落/静止时向下射线（排除自身碰撞体）
        if (m.velocity.y <= 0.0f) {
            Entity hit;
            float t = 0.0f;
            if (CollisionSystem::Raycast(world, xf->position, float3(0, -1, 0),
                                         halfHeight + kGroundTolerance,
                                         hit, t, e.id)) {
                // 命中地面：贴地吸附（把半高误差修正回精确贴地）
                m.bOnGround = true;
                xf->position.y += (halfHeight - t);
                m.velocity.y = 0.0f;
            } else {
                m.bOnGround = false;
            }
        }
    });
}

} // namespace he
