// ============================================================
// Tests/TestCollision.cpp — Phase B5 CollisionSystem 单元测试
//
// 覆盖：AABB/Sphere/Capsule 三形状全组合 Overlap、
//       Contains 点包含、Raycast 最近命中与容错。
// ============================================================

#include "doctest.h"

#include "Scene/World.h"
#include "Scene/Transform.h"
#include "Scene/CollisionComponent.h"
#include "Scene/CollisionSystem.h"

using namespace he;

namespace {
// 在指定位置创建带碰撞体积的实体
Entity MakeShape(World& world, const char* name, CollisionShape shape,
                 const float3& pos, const float3& halfExtents,
                 float radius, float height) {
    Entity e = world.CreateEntity(name);
    auto* xf = world.AddComponent<TransformComponent>(e);
    xf->position = pos;
    auto* cc = world.AddComponent<CollisionComponent>(e);
    cc->shape       = shape;
    cc->halfExtents = halfExtents;
    cc->radius      = radius;
    cc->height      = height;
    return e;
}
} // namespace

TEST_CASE("CollisionSystem AABB-AABB 重叠/分离/贴边") {
    World world;
    Entity a = MakeShape(world, "A", CollisionShape::AABB, float3(0, 0, 0),
                         float3(1, 1, 1), 0, 0);
    Entity b = MakeShape(world, "B", CollisionShape::AABB, float3(1.5f, 0, 0),
                         float3(1, 1, 1), 0, 0);   // 间隙 0.5 → 重叠
    Entity c = MakeShape(world, "C", CollisionShape::AABB, float3(2.5f, 0, 0),
                         float3(1, 1, 1), 0, 0);   // 间隙 0.5 → 分离
    Entity d = MakeShape(world, "D", CollisionShape::AABB, float3(2.0f, 0, 0),
                         float3(1, 1, 1), 0, 0);   // 恰好贴边（面接触）

    CHECK(CollisionSystem::Overlap(world, a, b) == true);
    CHECK(CollisionSystem::Overlap(world, a, c) == false);
    CHECK(CollisionSystem::Overlap(world, a, d) == true);   // 边界接触视为重叠
    CHECK(CollisionSystem::Overlap(world, b, a) == true);   // 对称
}

TEST_CASE("CollisionSystem 球体与混合形状重叠") {
    World world;
    Entity s1 = MakeShape(world, "S1", CollisionShape::Sphere, float3(0, 0, 0),
                          float3(0), 1.0f, 0);
    Entity s2 = MakeShape(world, "S2", CollisionShape::Sphere, float3(1.8f, 0, 0),
                          float3(0), 1.0f, 0);   // 球心距 1.8 < 2 → 重叠
    Entity s3 = MakeShape(world, "S3", CollisionShape::Sphere, float3(3.0f, 0, 0),
                          float3(0), 1.0f, 0);   // 球心距 3 > 2 → 分离
    Entity box = MakeShape(world, "Box", CollisionShape::AABB, float3(2.0f, 0, 0),
                           float3(0.5f), 0, 0);  // 盒子 [1.5,2.5]

    CHECK(CollisionSystem::Overlap(world, s1, s2) == true);
    CHECK(CollisionSystem::Overlap(world, s1, s3) == false);
    // 球 s2 中心 (1.8,0,0)，盒最近点 (1.8,0,0) → 距离 0 < 1 → 重叠
    CHECK(CollisionSystem::Overlap(world, s2, box) == true);
    // 球 s3 中心 (3,0,0)，盒最近点 (2.5,0,0) → 距离 0.5 < 1 → 重叠
    CHECK(CollisionSystem::Overlap(world, s3, box) == true);
    CHECK(CollisionSystem::Overlap(world, box, s1) == false);   // 盒 [1.5,2.5] 离球 0.5 > 1
}

TEST_CASE("CollisionSystem 胶囊重叠与混合形状") {
    World world;
    // 胶囊 A：垂直，高 2（段半长 0.5），半径 0.5，中心原点
    Entity capA = MakeShape(world, "CapA", CollisionShape::Capsule, float3(0, 0, 0),
                            float3(0), 0.5f, 2.0f);
    // 胶囊 B：水平并排，中心 (0.9, 0, 0) → 段间距 0.9 < 0.5+0.5 → 重叠
    Entity capB = MakeShape(world, "CapB", CollisionShape::Capsule, float3(0.9f, 0, 0),
                            float3(0), 0.5f, 2.0f);
    Entity capC = MakeShape(world, "CapC", CollisionShape::Capsule, float3(3.0f, 0, 0),
                            float3(0), 0.5f, 2.0f);
    // 球：压到胶囊侧壁
    Entity sph = MakeShape(world, "Sph", CollisionShape::Sphere, float3(0.9f, 1.0f, 0),
                           float3(0), 0.3f, 0);
    // 盒：套在胶囊下段
    Entity box = MakeShape(world, "BoxC", CollisionShape::AABB, float3(0, -0.9f, 0),
                           float3(0.3f), 0, 0);

    CHECK(CollisionSystem::Overlap(world, capA, capB) == true);
    CHECK(CollisionSystem::Overlap(world, capA, capC) == false);
    // 球 (0.9,1,0) 到段 (0,±0.5,0) 最近距离 ≈1.03 > 0.5+0.3=0.8 → 分离
    CHECK(CollisionSystem::Overlap(world, capA, sph) == false);
    CHECK(CollisionSystem::Overlap(world, capA, box) == true);   // 盒与胶囊段最近距离 < 半径
}

TEST_CASE("CollisionSystem Contains 点包含") {
    World world;
    Entity box = MakeShape(world, "Box", CollisionShape::AABB, float3(0, 0, 0),
                           float3(1, 1, 1), 0, 0);
    Entity sph = MakeShape(world, "Sph", CollisionShape::Sphere, float3(5, 0, 0),
                           float3(0), 1.0f, 0);
    Entity cap = MakeShape(world, "Cap", CollisionShape::Capsule, float3(-5, 0, 0),
                           float3(0), 0.5f, 2.0f);

    CHECK(CollisionSystem::Contains(world, box, float3(0.5f, 0.5f, 0.5f)) == true);
    CHECK(CollisionSystem::Contains(world, box, float3(1.5f, 0, 0)) == false);
    CHECK(CollisionSystem::Contains(world, sph, float3(5.9f, 0, 0)) == true);
    CHECK(CollisionSystem::Contains(world, sph, float3(7.0f, 0, 0)) == false);
    // 胶囊段端点 (±0.5,0,0) + 半径 0.5 → 完整胶囊 y ∈ [-1,1]
    CHECK(CollisionSystem::Contains(world, cap, float3(-5, 0.9f, 0)) == true);   // 距段 0.4 < 0.5
    CHECK(CollisionSystem::Contains(world, cap, float3(-5, 0, 0)) == true);      // 段上
    CHECK(CollisionSystem::Contains(world, cap, float3(-5, 1.5f, 0)) == false);  // 距段 1.0 > 0.5
    CHECK(CollisionSystem::Contains(world, cap, float3(-5.8f, 0, 0)) == false);  // 侧面外
}

TEST_CASE("CollisionSystem Raycast 最近命中") {
    World world;
    // 近盒在 z=5，远球在 z=10（沿 +Z 射线）
    Entity nearBox = MakeShape(world, "NearBox", CollisionShape::AABB, float3(0, 0, 5),
                               float3(1, 1, 1), 0, 0);
    Entity farSph = MakeShape(world, "FarSph", CollisionShape::Sphere, float3(3, 0, 10),
                              float3(0), 1.5f, 0);   // x=3：避开近盒（x∈[-1,1]）与胶囊（x=2）
    Entity sideCap = MakeShape(world, "SideCap", CollisionShape::Capsule, float3(2, 0, 2),
                               float3(0), 0.5f, 2.0f);

    // 盒命中：t = 5 - 1 = 4
    Entity hit; float t = 0;
    REQUIRE(CollisionSystem::Raycast(world, float3(0, 0, 0), float3(0, 0, 1), 100.0f, hit, t));
    CHECK(hit == nearBox);
    CHECK(t == doctest::Approx(4.0f));

    // 最近命中仍是近盒（球在 x=2，该射线不经过）
    Entity hit2; float t2 = 0;
    REQUIRE(CollisionSystem::Raycast(world, float3(0, 0, 0), float3(0, 0, 1), 100.0f, hit2, t2));
    CHECK(hit2 == nearBox);

    // 从 (3,0,0) 沿 +Z：绕过近盒与胶囊，命中球 t = 10 - 1.5 = 8.5
    REQUIRE(CollisionSystem::Raycast(world, float3(3, 0, 0), float3(0, 0, 1), 9.0f, hit2, t2));
    CHECK(hit2 == farSph);
    CHECK(t2 == doctest::Approx(8.5f));

    // 无命中（射线朝向 -Z）
    CHECK(CollisionSystem::Raycast(world, float3(0, 0, 0), float3(0, 0, -1), 100.0f, hit2, t2) == false);

    // 侧面胶囊命中：胶囊 (2,0,2) 恰好压住射线路径（射线 x=z），最近距离 0 < 0.5
    float3 dir = glm::normalize(float3(1, 0, 1));
    Entity hit3; float t3 = 0;
    REQUIRE(CollisionSystem::Raycast(world, float3(0, 0, 0), dir, 100.0f, hit3, t3));
    CHECK(hit3 == sideCap);
}

TEST_CASE("CollisionSystem Raycast 返回命中法线（坡度判断基础）") {
    World world;
    // 顶面朝 +Y 的盒（地面）
    Entity ground = MakeShape(world, "Ground", CollisionShape::AABB, float3(0, -1, 0),
                              float3(10, 1, 10), 0, 0);

    Entity hit; float t = 0; float3 n;
    // 从上方下射命中盒顶面 → 法线 +Y（up）
    REQUIRE(CollisionSystem::Raycast(world, float3(0, 5, 0), float3(0, -1, 0), 100.0f, hit, t, kInvalidEntity, &n));
    CHECK(hit == ground);
    CHECK(n.y == doctest::Approx(1.0f));   // 顶面法线朝上（可站立）

    // 从 +Z 侧射入盒（起点 y=-1 在盒内避开顶面，z=20 在盒外）→ 命中 +Z 面 → 法线 +Z（侧面 → 陡坡不可站立）
    REQUIRE(CollisionSystem::Raycast(world, float3(0, -1, 20), float3(0, 0, -1), 100.0f, hit, t, kInvalidEntity, &n));
    CHECK(hit == ground);
    CHECK(n.z == doctest::Approx(1.0f));   // +Z 面法线（坡度 90°）
}

TEST_CASE("CollisionSystem 禁用与缺失容错") {
    World world;
    Entity a = MakeShape(world, "A", CollisionShape::Sphere, float3(0, 0, 0),
                         float3(0), 1.0f, 0);
    Entity b = MakeShape(world, "B", CollisionShape::Sphere, float3(0.5f, 0, 0),
                         float3(0), 1.0f, 0);
    // 禁用 B → 视为无碰撞
    world.GetComponent<CollisionComponent>(b)->bEnabled = false;
    CHECK(CollisionSystem::Overlap(world, a, b) == false);
    CHECK(CollisionSystem::Contains(world, b, float3(0.5f, 0, 0)) == false);

    // 无碰撞组件的实体 → 全部容错返回 false
    Entity c = world.CreateEntity("NoCollision");
    world.AddComponent<TransformComponent>(c);
    CHECK(CollisionSystem::Overlap(world, a, c) == false);
    CHECK(CollisionSystem::Contains(world, c, float3(0)) == false);

    // 射线：禁用/无碰撞实体被跳过，仍可命中 A
    Entity hit; float t = 0;
    REQUIRE(CollisionSystem::Raycast(world, float3(0, 0, 5), float3(0, 0, -1), 100.0f, hit, t));
    CHECK(hit == a);
}
