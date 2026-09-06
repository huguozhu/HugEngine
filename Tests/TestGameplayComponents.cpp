// ============================================================
// Tests/TestGameplayComponents.cpp — P1 玩法组件单元测试
//
// 覆盖 Phase A7 ProjectileMovement / A8 Health / A6 SpringArm：
//   抛物线积分、超时销毁、追踪转向；扣血/回血/死亡边界/无敌；
//   弹簧臂硬跟随/旋转滞后收敛/缺实体容错。
// ============================================================

#include "doctest.h"

#include "Scene/World.h"
#include "Scene/SceneGraph.h"
#include "Scene/Transform.h"
#include "Scene/ProjectileMovementComponent.h"
#include "Scene/ProjectileSystem.h"
#include "Scene/HealthComponent.h"
#include "Scene/DamageSystem.h"
#include "Scene/SpringArmComponent.h"
#include "Scene/SpringArmSystem.h"

#include <cmath>

using namespace he;

namespace {
constexpr float kSqrt2_2 = 0.70710678f;  // sin45°/cos45°
}

// ============================================================
// A7 ProjectileMovement
// ============================================================

TEST_CASE("ProjectileSystem 抛物线积分符合半隐式欧拉解析解") {
    World world;

    Entity e = world.CreateEntity("Ball");
    auto* xform = world.AddComponent<TransformComponent>(e);
    xform->position = float3(0.0f, 2.0f, -4.0f);
    // 仰角 45°（绕 X 轴 +45°：前向 -Z 抬升为 (0, +√2/2, -√2/2)）
    xform->rotation = glm::angleAxis(glm::radians(45.0f), float3(1.0f, 0.0f, 0.0f));
    auto* proj = world.AddComponent<ProjectileMovementComponent>(e);
    proj->initialSpeed = 8.0f;
    proj->gravityScale = 1.0f;
    proj->lifetime     = 0.0f;   // 无限存活，避免干扰积分断言

    const float dt = 0.1f;
    const int   N  = 10;         // 共 1.0 秒
    const float g  = 9.8f;
    for (int i = 0; i < N; ++i) ProjectileSystem::Update(world, dt, g);

    // 半隐式欧拉（实现为每帧先重力后积分）：
    // y(N) = y0 + v0y·N·dt − g·dt²·N(N+1)/2（首帧速度即为 v0y − g·dt）
    const float v0y = 8.0f * kSqrt2_2;
    const float yExpected = 2.0f + v0y * N * dt - 0.5f * g * dt * dt * N * (N + 1);
    const float zExpected = -4.0f - v0y * N * dt;   // v0z = −8·√2/2

    CHECK(xform->position.y == doctest::Approx(yExpected).epsilon(0.001));
    CHECK(xform->position.z == doctest::Approx(zExpected).epsilon(0.001));
    CHECK(xform->position.x == doctest::Approx(0.0f));   // 无横向速度

    // 速度符合 v(N) = v0 − g·N·dt（y 分量）
    CHECK(proj->velocity.y == doctest::Approx(v0y - g * N * dt).epsilon(0.001));
}

TEST_CASE("ProjectileSystem 生命周期超时自动销毁实体") {
    World world;
    Entity e = world.CreateEntity("Shell");
    world.AddComponent<TransformComponent>(e);
    auto* proj = world.AddComponent<ProjectileMovementComponent>(e);
    proj->lifetime = 0.6f;

    for (int i = 0; i < 5; ++i) {          // 0.5 秒（未到期）
        ProjectileSystem::Update(world, 0.1f);
        CHECK(world.IsValid(e));           // 到期前仍存活
    }
    ProjectileSystem::Update(world, 0.1f); // 第 6 帧累计 0.6 秒 → 超时
    CHECK_FALSE(world.IsValid(e));         // 已被销毁
}

TEST_CASE("ProjectileSystem 追踪模式向目标转向") {
    World world;

    // 目标：静止在 +X 方向
    Entity target = world.CreateEntity("Target");
    world.AddComponent<TransformComponent>(target);
    world.GetComponent<TransformComponent>(target)->position = float3(10.0f, 0.0f, 0.0f);

    // 抛射物：朝 -Z 飞出（与目标方向垂直），开启追踪
    Entity e = world.CreateEntity("Missile");
    world.AddComponent<TransformComponent>(e);
    auto* proj = world.AddComponent<ProjectileMovementComponent>(e);
    proj->initialSpeed = 5.0f;
    proj->gravityScale = 0.0f;        // 关重力，隔离追踪效果
    proj->lifetime     = 0.0f;
    proj->bHoming      = true;
    proj->homingTarget = target.id;

    for (int i = 0; i < 10; ++i) ProjectileSystem::Update(world, 0.1f);

    // 速度应显著转向目标（+X 分量由 0 转正）
    CHECK(proj->velocity.x > 0.0f);
    // 速度大小不变（转向不改变速率）
    CHECK(glm::length(proj->velocity) == doctest::Approx(5.0f).epsilon(0.01));
}

// ============================================================
// A8 Health / DamageSystem
// ============================================================

TEST_CASE("DamageSystem 扣血/回血/钳制/死亡边界") {
    World world;
    Entity e = world.CreateEntity("Guard");
    auto* h = world.AddComponent<HealthComponent>(e);
    h->maxHealth = 100.0f;

    int deathCount = 0;
    h->onDeath = [&](Entity) { ++deathCount; };

    // 扣血 30 → 70，未死亡
    CHECK_FALSE(DamageSystem::ApplyDamage(world, e, 30.0f));
    CHECK(h->currentHealth == doctest::Approx(70.0f));

    // 过量伤害钳制到 0，触发死亡回调一次
    CHECK(DamageSystem::ApplyDamage(world, e, 999.0f));
    CHECK(h->currentHealth == doctest::Approx(0.0f));
    CHECK(h->IsDead());
    CHECK(deathCount == 1);

    // 死亡后再次扣血不重复触发死亡回调
    DamageSystem::ApplyDamage(world, e, 10.0f);
    CHECK(deathCount == 1);
    CHECK(h->currentHealth == doctest::Approx(0.0f));

    // 负伤害 = 治疗；回血钳制到 maxHealth
    DamageSystem::ApplyDamage(world, e, -30.0f);
    CHECK(h->currentHealth == doctest::Approx(30.0f));
    CHECK_FALSE(h->IsDead());
    DamageSystem::Heal(world, e, 999.0f);
    CHECK(h->currentHealth == doctest::Approx(100.0f));

    // 受伤回调收到实际伤害值
    float lastDamage = 0.0f;
    h->onDamaged = [&](Entity, float d) { lastDamage = d; };
    DamageSystem::ApplyDamage(world, e, 15.0f);
    CHECK(lastDamage == doctest::Approx(15.0f));

    // ResetHealth 重置到满血
    DamageSystem::ResetHealth(world, e);
    CHECK(h->currentHealth == doctest::Approx(100.0f));
}

TEST_CASE("DamageSystem 无敌免疫伤害但治疗仍生效") {
    World world;
    Entity e = world.CreateEntity("Boss");
    auto* h = world.AddComponent<HealthComponent>(e);
    h->bInvincible = true;
    h->currentHealth = 50.0f;

    // 无敌：伤害无效
    CHECK_FALSE(DamageSystem::ApplyDamage(world, e, 30.0f));
    CHECK(h->currentHealth == doctest::Approx(50.0f));

    // 无敌：治疗仍生效
    DamageSystem::Heal(world, e, 20.0f);
    CHECK(h->currentHealth == doctest::Approx(70.0f));
}

TEST_CASE("DamageSystem 无 HealthComponent 实体容错") {
    World world;
    Entity e = world.CreateEntity("NoHealth");
    world.AddComponent<TransformComponent>(e);
    // 无 Health 组件：伤害/治疗/重置均不崩溃，返回未死亡
    CHECK_FALSE(DamageSystem::ApplyDamage(world, e, 10.0f));
    DamageSystem::Heal(world, e, 10.0f);
    DamageSystem::ResetHealth(world, e);
}

// ============================================================
// A6 SpringArm
// ============================================================

TEST_CASE("SpringArmSystem 硬跟随合成相机位置（rotationLagSpeed=0）") {
    World world;

    // 目标在原点，默认朝向（前向 = -Z）
    Entity target = world.CreateEntity("Pawn");
    world.AddComponent<TransformComponent>(target);

    // 相机实体（初始在原点，由弹簧臂写入）
    Entity cam = world.CreateEntity("Cam");
    world.AddComponent<TransformComponent>(cam);

    Entity armEnt = world.CreateEntity("SpringArm");
    world.AddComponent<TransformComponent>(armEnt);
    auto* arm = world.AddComponent<SpringArmComponent>(armEnt);
    arm->targetEntity     = target.id;
    arm->cameraEntity     = cam.id;
    arm->targetOffset     = float3(0.0f, 1.5f, 0.0f);
    arm->armLength        = 5.0f;
    arm->rotationLagSpeed = 0.0f;   // 硬跟随

    SpringArmSystem::Update(world, 0.016f);

    // 相机位置 = 锚点(0,1.5,0) − 前向(0,0,−1)×5 = (0, 1.5, 5)
    auto* camXform = world.GetComponent<TransformComponent>(cam);
    CHECK(camXform->position.x == doctest::Approx(0.0f));
    CHECK(camXform->position.y == doctest::Approx(1.5f));
    CHECK(camXform->position.z == doctest::Approx(5.0f));
    // 相机朝向 = 目标朝向（bUsePawnControlRotation 默认 true）
    CHECK(glm::dot(camXform->GetForward(), float3(0.0f, 0.0f, -1.0f))
          == doctest::Approx(1.0f));
}

TEST_CASE("SpringArmSystem 旋转滞后：先滞后再收敛") {
    World world;

    Entity target = world.CreateEntity("Pawn");
    world.AddComponent<TransformComponent>(target);

    Entity cam = world.CreateEntity("Cam");
    world.AddComponent<TransformComponent>(cam);

    Entity armEnt = world.CreateEntity("SpringArm");
    world.AddComponent<TransformComponent>(armEnt);
    auto* arm = world.AddComponent<SpringArmComponent>(armEnt);
    arm->targetEntity     = target.id;
    arm->cameraEntity     = cam.id;
    arm->armLength        = 5.0f;
    arm->rotationLagSpeed = 8.0f;

    // 先硬跟随一次，让相机就位于 (0,0,5) 附近
    arm->rotationLagSpeed = 0.0f;
    SpringArmSystem::Update(world, 0.016f);
    auto* camXform = world.GetComponent<TransformComponent>(cam);
    CHECK(camXform->position.z == doctest::Approx(5.0f));
    arm->rotationLagSpeed = 8.0f;

    // 目标瞬移到 +X 10 米：单帧只移动一小步（滞后，不瞬移）
    world.GetComponent<TransformComponent>(target)->position = float3(10.0f, 0.0f, 0.0f);
    SpringArmSystem::Update(world, 0.016f);
    CHECK(camXform->position.x > 0.0f);
    CHECK(camXform->position.x < 10.0f);

    // 多帧后收敛到目标后方 (10, 0, 5)
    for (int i = 0; i < 300; ++i) SpringArmSystem::Update(world, 0.016f);
    CHECK(camXform->position.x == doctest::Approx(10.0f).epsilon(0.01));
    CHECK(camXform->position.z == doctest::Approx(5.0f).epsilon(0.01));
}

TEST_CASE("SpringArmSystem 目标/相机实体缺失时容错") {
    World world;
    Entity armEnt = world.CreateEntity("SpringArm");
    world.AddComponent<TransformComponent>(armEnt);
    auto* arm = world.AddComponent<SpringArmComponent>(armEnt);
    arm->targetEntity = 999;   // 不存在的目标
    arm->cameraEntity = 998;   // 不存在的相机

    // 缺失实体：不崩溃、不写入任何实体
    SpringArmSystem::Update(world, 0.016f);
    CHECK(true);
}
