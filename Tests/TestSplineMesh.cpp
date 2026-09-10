// ============================================================
// Tests/TestSplineMesh.cpp — SplineMeshComponent 条带网格生成测试
//
// 覆盖：条带顶点/索引数量、包围盒（宽度）、版本脏检测、无效样条容错。
// 注：无 RHI 设备时 SetMeshData 只跳过缓冲创建（计数/包围盒仍写入），
//     因此可脱离 GPU 断言几何生成结果；日志中的 device not available 属预期噪声。
// ============================================================

#include "doctest.h"

#include "Scene/World.h"
#include "Scene/SceneGraph.h"
#include "Scene/SplineComponent.h"
#include "Scene/SplineMeshComponent.h"
#include "Scene/SplineMeshSystem.h"

using namespace he;

TEST_CASE("SplineMesh 沿直线样条生成条带（顶点/索引/包围盒）") {
    World world;
    SceneGraph sg(world);

    // 样条：沿 -Z 的 10 米直线（两点开环）
    Entity splineE = world.CreateEntity("Spline");
    auto* spline = world.AddComponent<SplineComponent>(splineE);
    REQUIRE(spline != nullptr);
    spline->AddPoint(float3(0.0f, 0.0f, 0.0f));
    spline->AddPoint(float3(0.0f, 0.0f, -10.0f));

    // 条带：宽 2 米，4 段
    Entity meshE = world.CreateEntity("Road");
    auto* sm = world.AddComponent<SplineMeshComponent>(meshE);
    REQUIRE(sm != nullptr);
    sm->splineEntity = splineE.id;
    sm->width        = 2.0f;
    sm->segments     = 4;
    sm->uvTiling     = 1.0f;

    SplineMeshSystem::Update(world, nullptr);

    // (segments+1) 个采样点 × 每点 2 顶点；每段 2 三角形 = 6 索引
    CHECK(sm->GetVertexCount() == (4 + 1) * 2);
    CHECK(sm->GetIndexCount() == 4 * 6);

    // 包围盒：宽 2（±1 于 X），长度沿 -Z 10 米，Y 为 0
    AABB b = sm->GetBounds();
    CHECK(b.min.x == doctest::Approx(-1.0f));
    CHECK(b.max.x == doctest::Approx(1.0f));
    CHECK(b.min.z == doctest::Approx(-10.0f));
    CHECK(b.max.z == doctest::Approx(0.0f));
    CHECK(b.min.y == doctest::Approx(0.0f));
    CHECK(b.max.y == doctest::Approx(0.0f));

    // 构建后版本与样条版本一致（下次 Update 无需重建）
    CHECK(sm->GetBuiltVersion() == spline->GetVersion());
}

TEST_CASE("SplineMesh 版本脏检测：样条变化后才重建") {
    World world;
    SceneGraph sg(world);

    Entity splineE = world.CreateEntity("Spline");
    auto* spline = world.AddComponent<SplineComponent>(splineE);
    spline->AddPoint(float3(0.0f, 0.0f, 0.0f));
    spline->AddPoint(float3(0.0f, 0.0f, -5.0f));

    Entity meshE = world.CreateEntity("Road");
    auto* sm = world.AddComponent<SplineMeshComponent>(meshE);
    sm->splineEntity = splineE.id;
    sm->segments     = 2;

    SplineMeshSystem::Update(world, nullptr);
    CHECK(sm->GetVertexCount() == (2 + 1) * 2);
    CHECK(sm->GetBuiltVersion() == spline->GetVersion());

    // 新增控制点：版本递增 → 系统应重建（此处段数不变，顶点数仍为 3 采样点 ×2）
    spline->AddPoint(float3(0.0f, 0.0f, -15.0f));
    CHECK(sm->GetBuiltVersion() != spline->GetVersion());   // 已脏
    SplineMeshSystem::Update(world, nullptr);
    CHECK(sm->GetBuiltVersion() == spline->GetVersion());   // 重建后同步
    CHECK(sm->GetVertexCount() == (2 + 1) * 2);
}

TEST_CASE("SplineMesh 对无效样条容错（无控制点 → 空网格不崩溃）") {
    World world;
    SceneGraph sg(world);

    Entity emptySplineE = world.CreateEntity("EmptySpline");
    world.AddComponent<SplineComponent>(emptySplineE);   // 无控制点

    Entity meshE = world.CreateEntity("Road");
    auto* sm = world.AddComponent<SplineMeshComponent>(meshE);
    sm->splineEntity = emptySplineE.id;

    SplineMeshSystem::Update(world, nullptr);
    CHECK(sm->GetVertexCount() == 0);
    CHECK(sm->GetIndexCount() == 0);

    // 关联实体不存在同样容错
    sm->splineEntity = 999999;
    sm->MarkDirty();
    SplineMeshSystem::Update(world, nullptr);
    CHECK(sm->GetVertexCount() == 0);
}
