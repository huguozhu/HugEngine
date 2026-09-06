// ============================================================
// Tests/TestSpline.cpp — Phase B2 SplineComponent 单元测试
//
// 覆盖：直线/折线弧长、距离求值与钳制、闭环回绕、
//       参数求值、切向（用户/自动 Catmull-Rom）、脏重建。
// ============================================================

#include "doctest.h"

#include "Scene/World.h"
#include "Scene/Transform.h"
#include "Scene/SplineComponent.h"
#include "Scene/SplineSystem.h"

using namespace he;

TEST_CASE("SplineComponent 两点直线：弧长/距离求值/切向") {
    World world;
    Entity e = world.CreateEntity("Line");
    world.AddComponent<TransformComponent>(e);
    auto* s = world.AddComponent<SplineComponent>(e);
    s->AddPoint(float3(0, 0, 0));
    s->AddPoint(float3(0, 0, 10));

    CHECK(s->GetSegmentCount() == 1);
    CHECK(s->GetTotalLength() == doctest::Approx(10.0f).epsilon(0.01));

    // 端点与中点
    CHECK(s->EvaluateAtDistance(0.0f).z == doctest::Approx(0.0f));
    CHECK(s->EvaluateAtDistance(5.0f).z == doctest::Approx(5.0f).epsilon(0.01));
    CHECK(s->EvaluateAtDistance(10.0f).z == doctest::Approx(10.0f).epsilon(0.01));

    // 开环钳制：越界距离钳到端点
    CHECK(s->EvaluateAtDistance(99.0f).z == doctest::Approx(10.0f).epsilon(0.01));
    CHECK(s->EvaluateAtDistance(-5.0f).z == doctest::Approx(0.0f).epsilon(0.01));

    // 切向 = 直线方向（自动切线 = 端点单侧差 → (0,0,10)/... 单位化后 +Z）
    float3 tangent = s->GetTangent(3.0f);
    CHECK(tangent.z == doctest::Approx(1.0f).epsilon(0.01));

    // 静态系统包装等价
    CHECK(SplineSystem::EvaluateAtDistance(*s, 5.0f).z == doctest::Approx(5.0f).epsilon(0.01));
}

TEST_CASE("SplineComponent L 形样条：Catmull-Rom 平滑过弯与精确 Hermite 值") {
    World world;
    Entity e = world.CreateEntity("L");
    world.AddComponent<TransformComponent>(e);
    auto* s = world.AddComponent<SplineComponent>(e);
    // L 形：原点 → +X 10 米 → +Y 20 米（自动 Catmull-Rom 切线，拐角平滑外扩）
    s->AddPoint(float3(0, 0, 0));
    s->AddPoint(float3(10, 0, 0));
    s->AddPoint(float3(10, 20, 0));

    CHECK(s->GetSegmentCount() == 2);
    // 平滑过弯比折线（30）略长
    CHECK(s->GetTotalLength() > 30.0f);
    CHECK(s->GetTotalLength() < 33.0f);

    // 精确 Hermite 值（手算）：
    // 自动切线 T0=(5,0,0) T1=(5,10,0) T2=(0,10,0)
    // 第一段 t=0.5：H10=0.125 H01=0.5 H11=-0.125 → (5, -1.25, 0)
    float3 mid1 = s->EvaluateAtParam(0.5f);
    CHECK(mid1.x == doctest::Approx(5.0f).epsilon(0.001));
    CHECK(mid1.y == doctest::Approx(-1.25f).epsilon(0.001));
    // 第二段 t=0.5：H00=0.5 H10=0.125 H01=0.5 H11=-0.125 → (10.625, 10, 0)
    float3 mid2 = s->EvaluateAtParam(1.5f);
    CHECK(mid2.x == doctest::Approx(10.625f).epsilon(0.001));
    CHECK(mid2.y == doctest::Approx(10.0f).epsilon(0.001));

    // 参数端点精确过控制点
    CHECK(s->EvaluateAtParam(0.0f).x == doctest::Approx(0.0f));
    CHECK(s->EvaluateAtParam(1.0f).x == doctest::Approx(10.0f).epsilon(0.001));
    CHECK(s->EvaluateAtParam(2.0f).y == doctest::Approx(20.0f).epsilon(0.001));
}

TEST_CASE("SplineComponent 闭环回绕") {
    World world;
    Entity e = world.CreateEntity("Loop");
    world.AddComponent<TransformComponent>(e);
    auto* s = world.AddComponent<SplineComponent>(e);
    s->bClosedLoop = true;
    // 正方形闭环：边长 10，周长 40
    s->AddPoint(float3(0, 0, 0));
    s->AddPoint(float3(10, 0, 0));
    s->AddPoint(float3(10, 0, 10));
    s->AddPoint(float3(0, 0, 10));

    CHECK(s->GetSegmentCount() == 4);   // 闭环段数 = 点数
    // 平滑过弯比折线周长（40）略长
    CHECK(s->GetTotalLength() > 40.0f);
    CHECK(s->GetTotalLength() < 44.0f);

    // 距离回绕：总长整数倍回到首点（闭环 fmod）
    float total = s->GetTotalLength();
    float3 p = s->EvaluateAtDistance(total);
    CHECK(p.x == doctest::Approx(0.0f).epsilon(0.01));
    CHECK(p.z == doctest::Approx(0.0f).epsilon(0.01));
    // 负距离回绕：-ε → 总长−ε ≈ 首点附近
    float3 q = s->EvaluateAtDistance(-0.001f);
    CHECK(q.x == doctest::Approx(0.0f).epsilon(0.05));
    CHECK(q.z == doctest::Approx(0.0f).epsilon(0.05));
    // 距离 total+5 与 5 同点（周期回绕）
    float3 a = s->EvaluateAtDistance(5.0f);
    float3 b = s->EvaluateAtDistance(total + 5.0f);
    CHECK(a.x == doctest::Approx(b.x).epsilon(0.01));
    CHECK(a.z == doctest::Approx(b.z).epsilon(0.01));

    // 参数循环：t = 4 回绕到 t = 0（首点）
    float3 r = s->EvaluateAtParam(4.0f);
    CHECK(r.x == doctest::Approx(0.0f).epsilon(0.01));
    CHECK(r.z == doctest::Approx(0.0f).epsilon(0.01));
}

TEST_CASE("SplineComponent 用户切向与自动 Catmull-Rom 切线") {
    World world;
    Entity e = world.CreateEntity("Curve");
    world.AddComponent<TransformComponent>(e);
    auto* s = world.AddComponent<SplineComponent>(e);

    // 用户指定起点切向 +X：起点处切线 = +X
    s->AddPoint(float3(0, 0, 0), float3(1, 0, 0));
    s->AddPoint(float3(5, 0, 0));
    float3 t0 = s->GetTangent(0.0f);
    CHECK(t0.x == doctest::Approx(1.0f).epsilon(0.01));

    // 全自动切线：三点直线（0 → 5 → 10），首点切线 = (P1−P0) 单侧差 → +Z
    World world2;
    Entity e2 = world2.CreateEntity("Auto");
    world2.AddComponent<TransformComponent>(e2);
    auto* s2 = world2.AddComponent<SplineComponent>(e2);
    s2->AddPoint(float3(0, 0, 0));
    s2->AddPoint(float3(0, 0, 5));
    s2->AddPoint(float3(0, 0, 10));
    // 起点处切线 +Z（自动 Catmull-Rom 单侧差）
    float3 tAuto = s2->GetTangent(0.0f);
    CHECK(tAuto.z == doctest::Approx(1.0f).epsilon(0.01));
}

TEST_CASE("SplineComponent 脏重建与容错") {
    World world;
    Entity e = world.CreateEntity("Dirty");
    world.AddComponent<TransformComponent>(e);
    auto* s = world.AddComponent<SplineComponent>(e);
    s->AddPoint(float3(0, 0, 0));
    s->AddPoint(float3(0, 0, 5));
    CHECK(s->GetTotalLength() == doctest::Approx(5.0f).epsilon(0.01));

    // 加点 → 弧长缓存失效重建
    s->AddPoint(float3(0, 0, 12));
    CHECK(s->GetTotalLength() == doctest::Approx(12.0f).epsilon(0.01));

    // 少于 2 点：安全返回零向量，不崩溃
    s->Clear();
    CHECK(s->GetSegmentCount() == 0);
    CHECK(s->GetTotalLength() == doctest::Approx(0.0f));
    CHECK(glm::length(s->EvaluateAtDistance(3.0f)) == doctest::Approx(0.0f));
    CHECK(glm::length(s->EvaluateAtParam(1.0f)) == doctest::Approx(0.0f));
}
