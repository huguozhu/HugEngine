// ============================================================
// Tests/TestCharacterMovement.cpp — Phase B3 CharacterMovement 单元测试
//
// 覆盖：重力下落与落地吸附、跳跃高度（能量守恒）、走/跑速度、
//       空中水平移动、无地面持续下落、空中二段跳禁止、容错。
// ============================================================

#include "doctest.h"

#include "Scene/World.h"
#include "Scene/Transform.h"
#include "Scene/CollisionComponent.h"
#include "Scene/CollisionSystem.h"
#include "Scene/CharacterMovementComponent.h"
#include "Scene/MovementSystem.h"

using namespace he;

namespace {
constexpr float kDt = 0.016f;   // 60fps 固定步长

// 地面：AABB 碰撞体（无视觉网格，仅碰撞）
Entity MakeGround(World& w, const char* name, const float3& pos, const float3& halfExtents) {
    Entity e = w.CreateEntity(name);
    auto* xf = w.AddComponent<TransformComponent>(e);
    xf->position = pos;
    auto* cc = w.AddComponent<CollisionComponent>(e);
    cc->shape       = CollisionShape::AABB;
    cc->halfExtents = halfExtents;
    return e;
}

// 角色：胶囊碰撞体（半径 0.4 高 1.6 → 半高 0.8）+ 移动组件
Entity MakeCharacter(World& w, const char* name, const float3& pos) {
    Entity e = w.CreateEntity(name);
    auto* xf = w.AddComponent<TransformComponent>(e);
    xf->position = pos;
    auto* cc = w.AddComponent<CollisionComponent>(e);
    cc->shape  = CollisionShape::Capsule;
    cc->radius = 0.4f;
    cc->height = 1.6f;
    w.AddComponent<CharacterMovementComponent>(e);
    return e;
}

// 步进若干帧
void Step(World& w, int frames) {
    for (int i = 0; i < frames; ++i) MovementSystem::Update(w, kDt);
}
} // namespace

TEST_CASE("MovementSystem 重力下落并贴地吸附") {
    World world;
    Entity ground = MakeGround(world, "Ground", float3(0, 0, 0), float3(10, 0.5f, 10));
    Entity hero = MakeCharacter(world, "Hero", float3(0, 3.0f, 0));

    auto* xf = world.GetComponent<TransformComponent>(hero);
    auto* cm = world.GetComponent<CharacterMovementComponent>(hero);

    // 初始悬空
    CHECK(cm->bOnGround == false);
    Step(world, 120);   // 1.92 秒，足够落地

    // 落地：地面顶 0.5 + 半高 0.8 = 1.3
    CHECK(cm->bOnGround == true);
    CHECK(xf->position.y == doctest::Approx(1.3f).epsilon(0.01));
    CHECK(cm->velocity.y == doctest::Approx(0.0f));

    // 持续站立：位置不再下落
    float y = xf->position.y;
    Step(world, 30);
    CHECK(xf->position.y == doctest::Approx(y));
}

TEST_CASE("MovementSystem 跳跃高度符合能量守恒") {
    World world;
    MakeGround(world, "Ground", float3(0, 0, 0), float3(10, 0.5f, 10));
    Entity hero = MakeCharacter(world, "Hero", float3(0, 1.5f, 0));

    auto* xf = world.GetComponent<TransformComponent>(hero);
    auto* cm = world.GetComponent<CharacterMovementComponent>(hero);
    Step(world, 60);   // 先落地站稳（y=1.3）
    REQUIRE(cm->bOnGround == true);

    // 起跳：初速 = √(2·g·h) ≈ 3.96；同帧重力先减一次（实现顺序：跳后立即积分重力）
    cm->bWantsJump = true;
    MovementSystem::Update(world, kDt);
    CHECK(cm->bOnGround == false);
    CHECK(cm->bWantsJump == false);   // 边沿触发已消费
    CHECK(cm->velocity.y == doctest::Approx(std::sqrt(2.0f * 9.8f * 0.8f) - 9.8f * kDt).epsilon(0.001));

    // 跟踪空中最高点 → 近似 1.3 + 0.8 = 2.1（半隐式欧拉 ±0.1）
    float maxY = xf->position.y;
    int frames = 0;
    while (!cm->bOnGround && frames < 300) {
        MovementSystem::Update(world, kDt);
        maxY = std::max(maxY, xf->position.y);
        ++frames;
    }
    CHECK(cm->bOnGround == true);   // 落回地面
    CHECK(maxY == doctest::Approx(2.1f).epsilon(0.05));
    CHECK(xf->position.y == doctest::Approx(1.3f).epsilon(0.01));
}

TEST_CASE("MovementSystem 走/跑速度与水平移动") {
    World world;
    MakeGround(world, "Ground", float3(0, 0, 0), float3(10, 0.5f, 10));
    Entity hero = MakeCharacter(world, "Hero", float3(0, 1.3f, 0));

    auto* xf = world.GetComponent<TransformComponent>(hero);
    auto* cm = world.GetComponent<CharacterMovementComponent>(hero);
    Step(world, 10);   // 稳定落地

    // 步行：速度 = walkSpeed，方向 X
    cm->inputDirection = float2(1.0f, 0.0f);
    float x0 = xf->position.x;
    MovementSystem::Update(world, kDt);
    CHECK(cm->velocity.x == doctest::Approx(1.5f));
    CHECK(cm->velocity.z == doctest::Approx(0.0f));
    CHECK(xf->position.x == doctest::Approx(x0 + 1.5f * kDt).epsilon(0.001));

    // 跑步：速度 = runSpeed
    cm->bRunning = true;
    MovementSystem::Update(world, kDt);
    CHECK(cm->velocity.x == doctest::Approx(4.5f));

    // 斜向输入长度 >1 归一化（不超速）
    cm->inputDirection = float2(1.0f, 1.0f);
    MovementSystem::Update(world, kDt);
    float speed = glm::length(float3(cm->velocity.x, 0, cm->velocity.z));
    CHECK(speed == doctest::Approx(4.5f).epsilon(0.001));
}

TEST_CASE("MovementSystem 空中水平移动与二段跳禁止") {
    World world;
    MakeGround(world, "Ground", float3(0, 0, 0), float3(10, 0.5f, 10));
    Entity hero = MakeCharacter(world, "Hero", float3(0, 1.3f, 0));

    auto* xf = world.GetComponent<TransformComponent>(hero);
    auto* cm = world.GetComponent<CharacterMovementComponent>(hero);
    Step(world, 10);

    // 起跳后空中横向移动
    cm->bWantsJump = true;
    cm->inputDirection = float2(0.0f, 1.0f);
    MovementSystem::Update(world, kDt);
    float z0 = xf->position.z;
    MovementSystem::Update(world, kDt);
    CHECK(cm->bOnGround == false);
    CHECK(xf->position.z > z0);   // 空中沿 Z 移动

    // 空中二段跳：速度不重置、不上升跳
    float vy = cm->velocity.y;
    cm->bWantsJump = true;
    MovementSystem::Update(world, kDt);
    CHECK(cm->bWantsJump == false);   // 消费
    CHECK(cm->velocity.y < vy);       // 仍在下落（无第二段起跳）
}

TEST_CASE("MovementSystem 无地面持续下落") {
    World world;
    Entity hero = MakeCharacter(world, "Hero", float3(0, 3.0f, 0));

    auto* xf = world.GetComponent<TransformComponent>(hero);
    auto* cm = world.GetComponent<CharacterMovementComponent>(hero);

    float y0 = xf->position.y;
    Step(world, 60);
    CHECK(cm->bOnGround == false);
    CHECK(xf->position.y < y0 - 1.0f);   // 持续下落
}

TEST_CASE("MovementSystem 无碰撞体用默认半高且容错") {
    World world;
    MakeGround(world, "Ground", float3(0, 0, 0), float3(10, 0.5f, 10));

    // 无 CollisionComponent 的角色：默认半高 0.5 → 落地 y = 1.0
    Entity hero = world.CreateEntity("Hero");
    auto* xf = world.AddComponent<TransformComponent>(hero);
    xf->position = float3(0, 2.0f, 0);
    world.AddComponent<CharacterMovementComponent>(hero);

    Step(world, 120);
    auto* cm = world.GetComponent<CharacterMovementComponent>(hero);
    CHECK(cm->bOnGround == true);
    CHECK(xf->position.y == doctest::Approx(1.0f).epsilon(0.01));

    // 无 Transform 的移动组件：Update 不崩溃
    Entity ghost = world.CreateEntity("Ghost");
    world.AddComponent<CharacterMovementComponent>(ghost);
    MovementSystem::Update(world, kDt);
}
