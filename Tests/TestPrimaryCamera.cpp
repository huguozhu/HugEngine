// ============================================================
// Tests/TestPrimaryCamera.cpp — World::GetPrimaryCamera 单元测试（S0.4）
//
// 覆盖：无相机实体 / 单主相机 / 多相机取首个 isMain / 全非主相机。
// 渲染侧 ResolveFrameCamera（Render 模块）依赖本函数，见 05.AISamples 冒烟。
// ============================================================

#include "doctest.h"

#include "Scene/World.h"
#include "Scene/Transform.h"
#include "Scene/CameraComponent.h"

using namespace he;

TEST_CASE("GetPrimaryCamera 无相机实体时返回 nullptr") {
    World world;
    Entity e = world.CreateEntity("Cube");
    world.AddComponent<TransformComponent>(e);
    CHECK(world.GetPrimaryCamera() == nullptr);
}

TEST_CASE("GetPrimaryCamera 返回首个 isMain 相机") {
    World world;

    // 普通实体不干扰
    Entity e0 = world.CreateEntity("Cube");
    world.AddComponent<TransformComponent>(e0);

    // 相机 A：isMain=false（跟随相机）→ 不应被选中
    Entity camA = world.CreateEntity("FollowCam");
    world.AddComponent<TransformComponent>(camA);
    auto* a = world.AddComponent<CameraComponent>(camA);
    a->isMain = false;

    // 无主相机时返回 nullptr
    CHECK(world.GetPrimaryCamera() == nullptr);

    // 相机 B：isMain=true → 应被选中
    Entity camB = world.CreateEntity("MainCam");
    world.AddComponent<TransformComponent>(camB);
    auto* b = world.AddComponent<CameraComponent>(camB);
    b->isMain = true;

    CHECK(world.GetPrimaryCamera() == b);

    // 相机 C：第二个 isMain=true → 仍返回先注册的 B（首个 isMain 优先）
    Entity camC = world.CreateEntity("MainCam2");
    world.AddComponent<TransformComponent>(camC);
    auto* c = world.AddComponent<CameraComponent>(camC);
    c->isMain = true;

    CHECK(world.GetPrimaryCamera() == b);

    // 销毁主相机 B 后 → 轮到 C
    world.DestroyEntity(camB);
    CHECK(world.GetPrimaryCamera() == c);

    // A 的 isMain 改为 true → A 变为桶内首个主相机（首个 isMain 优先）
    a->isMain = true;
    CHECK(world.GetPrimaryCamera() == a);
}
