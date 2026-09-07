// ============================================================
// Tests/TestNavMesh.cpp — NavMesh A* 寻路测试（C4）
//
// 覆盖：直线路径、绕过阻挡、不可达返回 false、未设置网格容错。
// ============================================================

#include "doctest.h"

#include "Scene/World.h"
#include "Scene/SceneGraph.h"
#include "Scene/Transform.h"
#include "Scene/NavMeshComponent.h"
#include "Scene/NavMeshSystem.h"
#include "Scene/NavAgentComponent.h"
#include "Scene/NavAgentSystem.h"

#include <vector>

using namespace he;

TEST_CASE("NavMesh A* 直线路径（无障碍）") {
    World world;
    NavMeshComponent nav;
    nav.Resize(10, 10, 1.0f);   // 10x10 格，每格 1m，origin (0,0)

    std::vector<float3> path;
    // from (0.5,0,0.5) → to (9.5,0,9.5)（对角）→ 有路径
    REQUIRE(NavMeshSystem::FindPath(world, nav, float3(0.5f, 0, 0.5f), float3(9.5f, 0, 9.5f), path));
    CHECK(path.size() >= 2);
    // 路径端点接近终点（最后一格中心）
    float3 last = path.back();
    CHECK(last.x == doctest::Approx(9.5f));
    CHECK(last.z == doctest::Approx(9.5f));
    // 起终点可通行
    CHECK(NavMeshSystem::IsWalkable(world, nav, float3(0.5f, 0, 0.5f)));
    CHECK(NavMeshSystem::IsWalkable(world, nav, float3(5.5f, 0, 5.5f)));
}

TEST_CASE("NavMesh A* 绕开阻挡墙") {
    World world;
    NavMeshComponent nav;
    nav.Resize(10, 10, 1.0f);

    // 在行 4 横向放一堵阻挡墙（col 3..6 阻挡）→ 需绕行
    for (int c = 3; c <= 6; ++c)
        nav.SetBlocked(c, 4, true);

    std::vector<float3> path;
    // 从 (0.5,0,0.5) 到 (9.5,0,9.5)，墙在中间 → 必须绕开行 4 的阻挡
    REQUIRE(NavMeshSystem::FindPath(world, nav, float3(0.5f, 0, 0.5f), float3(9.5f, 0, 9.5f), path));
    CHECK(path.size() >= 2);

    // 路径不允许经过任何阻挡格（检查每个路径点对应格子非阻挡）
    for (auto& p : path) {
        int c, r;
        if (nav.WorldToCell(p, c, r))
            CHECK(nav.IsBlocked(c, r) == false);
    }
    // 起点在墙的一侧、终点在另一侧 → 路径确实穿越了网格（长度 > 直线）
    CHECK(path.size() > 3);
}

TEST_CASE("NavMesh A* 起点无法通行返回 false") {
    World world;
    NavMeshComponent nav;
    nav.Resize(5, 5, 1.0f);
    nav.SetBlocked(0, 0, true);   // 起点阻挡

    std::vector<float3> path;
    CHECK(NavMeshSystem::FindPath(world, nav, float3(0.5f, 0, 0.5f), float3(4.5f, 0, 4.5f), path) == false);
    CHECK(path.empty());
}

TEST_CASE("NavMesh 未设置网格容错") {
    World world;
    NavMeshComponent nav;   // 未 Resize

    std::vector<float3> path;
    CHECK(NavMeshSystem::FindPath(world, nav, float3(0, 0, 0), float3(5, 0, 5), path) == false);
    CHECK(NavMeshSystem::IsWalkable(world, nav, float3(0, 0, 0)) == false);
}

TEST_CASE("NavAgent 沿路径移动到目标（C4）") {
    World world;
    SceneGraph sg(world);

    // 导航网格实体（10x10 格，无阻挡）
    Entity navE = world.CreateEntity("NavMesh");
    world.AddComponent<TransformComponent>(navE);
    auto* nav = world.AddComponent<NavMeshComponent>(navE);
    nav->Resize(10, 10, 1.0f);

    // 代理实体：从 (0.5,0,0.5) 走到 (9.5,0,9.5)
    Entity agentE = world.CreateEntity("Agent");
    auto* axf = world.AddComponent<TransformComponent>(agentE);
    axf->position = float3(0.5f, 0, 0.5f);
    auto* agent = world.AddComponent<NavAgentComponent>(agentE);
    agent->navMeshEntity = navE;
    agent->speed = 10.0f;
    agent->SetTarget(float3(9.5f, 0, 9.5f));

    // 模拟若干帧（路径对角长 ~12.7m，speed 10 → ~1.3s → 90 帧足够）
    for (int i = 0; i < 120; ++i)
        NavAgentSystem::Update(world, sg, 1.0f / 60.0f);

    CHECK(agent->bReached == true);   // 到达目标
    CHECK(axf->position.x == doctest::Approx(9.5f));   // x 到位
    CHECK(axf->position.z == doctest::Approx(9.5f));   // z 到位
}
