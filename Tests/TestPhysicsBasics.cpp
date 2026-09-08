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
#include "Scene/CollisionComponent.h"

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
    rb->shape = 0;
    rb->radius = 0.5f;
    rb->isDynamic = true;
    rb->mass = 1.0f;

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

TEST_CASE("PhysicsSystem 球落在静态碰撞盒上停住（T5）") {
    World world;
    SceneGraph sg(world);

    // 静态地面：CollisionComponent（AABB）+ Transform（顶部 y=0）
    Entity ground = world.CreateEntity("Ground");
    auto* gxf = world.AddComponent<TransformComponent>(ground);
    gxf->position = float3(0.0f, -0.5f, 0.0f);
    auto* gc = world.AddComponent<CollisionComponent>(ground);
    gc->shape = CollisionShape::AABB;
    gc->halfExtents = float3(100.0f, 0.5f, 100.0f);
    gc->bEnabled = true;

    // 动态球：从 y=5 下落
    Entity ball = world.CreateEntity("Ball");
    auto* bxf = world.AddComponent<TransformComponent>(ball);
    bxf->position = float3(0.0f, 5.0f, 0.0f);
    auto* rb = world.AddComponent<RigidBodyComponent>(ball);
    rb->shape = 0;
    rb->radius = 0.5f;
    rb->isDynamic = true;
    rb->mass = 1.0f;

    // 模拟 3 秒（180 帧）→ 球应落到地面（地面顶面 y=0 + 球半径 0.5 = 球心 y≈0.5）静止
    for (int i = 0; i < 180; ++i)
        PhysicsSystem::Update(world, sg, 1.0f / 60.0f);

    CHECK(std::fabs(bxf->position.y - 0.5f) < 0.15f);   // 球心停在半径高度（地面顶面上）
    CHECK(PhysicsSystem::HasBody(world, ground) == true);   // 地面注册为静态 body
}

TEST_CASE("PhysicsSystem 盒/胶囊刚体形状覆盖（下落）") {
    World world;
    SceneGraph sg(world);

    // 盒（shape=1）
    Entity box = world.CreateEntity("Box");
    auto* boxXf = world.AddComponent<TransformComponent>(box);
    boxXf->position = float3(-2.0f, 6.0f, 0.0f);
    auto* boxRb = world.AddComponent<RigidBodyComponent>(box);
    boxRb->shape = 1;
    boxRb->halfExtent = 0.5f;
    boxRb->isDynamic = true;
    boxRb->mass = 1.0f;

    // 胶囊（shape=2）
    Entity cap = world.CreateEntity("Capsule");
    auto* capXf = world.AddComponent<TransformComponent>(cap);
    capXf->position = float3(2.0f, 6.0f, 0.0f);
    auto* capRb = world.AddComponent<RigidBodyComponent>(cap);
    capRb->shape = 2;
    capRb->radius = 0.4f;
    capRb->height = 1.2f;
    capRb->isDynamic = true;
    capRb->mass = 1.0f;

    // 盒/胶囊 body 创建（shape 覆盖：CreateShape 0/1/2 均能建 body）
    for (int i = 0; i < 30; ++i)
        PhysicsSystem::Update(world, sg, 1.0f / 60.0f);

    CHECK(PhysicsSystem::HasBody(world, box) == true);   // 盒 body 创建
    CHECK(PhysicsSystem::HasBody(world, cap) == true);   // 胶囊 body 创建
}
