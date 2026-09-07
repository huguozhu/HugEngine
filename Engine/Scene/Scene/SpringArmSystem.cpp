#include "Scene/SpringArmSystem.h"

#include "Scene/SpringArmComponent.h"
#include "Scene/CollisionComponent.h"
#include "Scene/CollisionSystem.h"
#include "Scene/Transform.h"
#include "Scene/World.h"

#include <cmath>
#include <algorithm>

namespace he {

void SpringArmSystem::Update(World& world, f32 dt) {
    if (dt <= 0.0f) return;

    world.ForEach<SpringArmComponent>([&](Entity, SpringArmComponent& arm) {
        // 目标与相机实体必须都存在（缺失时跳过，等待场景就绪）
        auto* target = world.GetComponent<TransformComponent>(Entity{arm.targetEntity});
        auto* cam    = world.GetComponent<TransformComponent>(Entity{arm.cameraEntity});
        if (!target || !cam) return;

        // 锚点 = 目标位置 + 局部偏移（随目标旋转）
        float3 anchor     = target->position + target->rotation * arm.targetOffset;
        // 臂方向 = 目标后方（相机悬于目标后方的方向）
        float3 armDir     = -target->GetForward();

        // 碰撞缩臂：从锚点沿臂方向射线检测场景碰撞体，命中则把相机拉到碰撞物前（防穿墙）
        float effectiveLength = arm.armLength;
        Entity hitE;
        float  hitT = 0.0f;
        // 排除目标自身（臂起点处的碰撞体不视为遮挡）
        if (CollisionSystem::Raycast(world, anchor, armDir, arm.armLength, hitE, hitT,
                                     arm.targetEntity)) {
            effectiveLength = std::max(hitT - 0.1f, 0.1f);   // 留 0.1m 余量，避免贴面
        }

        // 期望相机位置 = 锚点 + 臂方向 × 有效臂长（碰撞缩短后）
        float3 desiredPos = anchor + armDir * effectiveLength;

        // 旋转滞后系数：rotationLagSpeed=0 → 硬跟随（k=1）；越大收敛越快
        float k = 1.0f;
        if (arm.rotationLagSpeed > 0.0f)
            k = 1.0f - std::exp(-arm.rotationLagSpeed * dt);

        // 位置始终带滞后跟随（与朝向滞后共用系数，保证视口稳定）
        cam->position = glm::mix(cam->position, desiredPos, k);

        // 朝向：bUsePawnControlRotation=true 时相机朝向跟随目标（简化版 UE 行为）；
        // false 时保持相机自身朝向（仅位置跟随）
        if (arm.bUsePawnControlRotation) {
            cam->rotation = glm::slerp(cam->rotation, target->rotation, k);
        }
    });
}

} // namespace he
