#include "Scene/ProjectileSystem.h"

#include "Scene/ProjectileMovementComponent.h"
#include "Scene/Transform.h"
#include "Scene/World.h"

#include <vector>

namespace he {

void ProjectileSystem::Update(World& world, f32 dt, float gravity) {
    if (dt <= 0.0f) return;

    // 本帧待销毁实体（帧尾统一销毁，避免在 ForEach 迭代中改桶导致越界）
    std::vector<Entity> pendingDestroy;

    world.ForEach<ProjectileMovementComponent>([&](Entity e, ProjectileMovementComponent& p) {
        auto* xform = world.GetComponent<TransformComponent>(e);
        if (!xform) return;

        // 1. 首帧初始化：速度 = initialSpeed × 实体前向
        if (!p.bInitialized) {
            p.velocity = xform->GetForward() * p.initialSpeed;
            p.bInitialized = true;
        }

        // 2. 追踪目标：每帧向目标方向转向（简化追踪，强度恒定）
        if (p.bHoming && p.homingTarget != kInvalidEntity) {
            auto* targetXform = world.GetComponent<TransformComponent>(Entity{p.homingTarget});
            if (targetXform) {
                float speed = glm::length(p.velocity);
                if (speed > 1e-4f) {
                    float3 toTarget = targetXform->position - xform->position;
                    if (glm::length(toTarget) > 1e-4f) {
                        float3 desired = glm::normalize(toTarget) * speed;
                        // 当前速度向期望方向混合（系数 0.1 = 快速转向但不过冲）
                        p.velocity = glm::normalize(glm::mix(p.velocity, desired, 0.1f)) * speed;
                    }
                }
            }
        }

        // 3. 重力积分（gravityScale=0 时直线飞行）
        if (p.gravityScale != 0.0f) {
            p.velocity.y -= gravity * p.gravityScale * dt;
        }

        // 4. 最大速度钳制（0 = 不限速）
        if (p.maxSpeed > 0.0f) {
            float speed = glm::length(p.velocity);
            if (speed > p.maxSpeed)
                p.velocity = p.velocity / speed * p.maxSpeed;
        }

        // 5. 位置积分
        xform->position += p.velocity * dt;

        // 6. 生命周期超时销毁（0 = 无限存活）
        p.lifeElapsed += dt;
        if (p.lifetime > 0.0f && p.lifeElapsed >= p.lifetime)
            pendingDestroy.push_back(e);
    });

    // 帧尾统一销毁超时抛射物（实体过期时触发 onHit 空调用不成立，仅做清理）
    for (Entity e : pendingDestroy) {
        world.DestroyEntity(e);
    }
}

} // namespace he
