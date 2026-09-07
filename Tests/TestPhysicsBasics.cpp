// ============================================================
// Tests/TestPhysicsBasics.cpp — PhysicsSystem 刚体基础测试（T4）
//
// 覆盖：丢球自由下落符合 ½gt²、实体销毁 → body 回收（无泄漏）。
// 注：落地/静态碰撞由 T5（静态地面）覆盖。
// ============================================================

#include "doctest.h"

#include "Physics/Physics/RigidBodyComponent.h"
#include "Physics/Physics/PhysicsSystem.h"

#include "Scene/World.h"
#include "Scene/SceneGraph.h"
#include "Scene/Transform.h"

#include <cmath>

using namespace he;
using namespace he::physics;

TEST_CASE("PhysicsSystem 丢球自由下落符合 ½gt²") {
    World world;
    SceneGraph sg(world);

    Entity e = world.CreateEntity("Ball");
    auto* xf = world.AddComponent<TransformComponent>(e);
    xf->position = float3(0.0f, 10.0f, 0.0f);
    auto* rb = world.AddComponent<RigidBodyComponent>(e);
    rb->shape = 0; rb->radius = 0.5f; rb->isDynamic = true; rb->mass = 1.0f;

    // 模拟 0.5s（60fps × 30 帧）；物理固定步 1/120s 内部累计
    for (int i = 0; i < 30; ++i)
        PhysicsSystem::Update(world, sg, 1.0f / 60.0f);

    // 解析解 y = 10 - ½·9.81·0.5² ≈ 8.774（数值积分容差 ±0.2）
    float expectedY = 10.0f - 0.5f * 9.81f * 0.5f * 0.5f;
    CHECK(std::fabs(xf->position.y - expectedY) < 0.2f);
    CHECK(std::fabs(xf->position.x) < 0.05f);   // 水平不漂移
    CHECK(PhysicsSystem::HasBody(world, e) == true);
}

TEST_CASE("PhysicsSystem 实体销毁后 body 回收（无泄漏）") {
    World world;
    SceneGraph sg(world);

    Entity e = world.CreateEntity("Ball2");
    world.AddComponent<TransformComponent>(e);
    world.AddComponent<RigidBodyComponent>(e);
    PhysicsSystem::Update(world, sg, 1.0f / 60.0f);
    CHECK(PhysicsSystem::HasBody(world, e) == true);

    // 销毁实体 → 下一帧 Update 回收对应 body
    world.DestroyEntity(e);
    PhysicsSystem::Update(world, sg, 1.0f / 60.0f);
    CHECK(PhysicsSystem::HasBody(world, e) == false);
}
