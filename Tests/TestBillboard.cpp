// ============================================================
// Tests/TestBillboard.cpp — Phase A4 BillboardComponent 单元测试
//
// 覆盖：OnCreate 四边形网格生成、billboard 矩阵对齐相机、
//       任意相机朝向下的正交基与朝向相机法线。
// ============================================================

#include "doctest.h"

#include "Scene/World.h"
#include "Scene/Transform.h"
#include "Scene/BillboardComponent.h"

using namespace he;

TEST_CASE("BillboardComponent::OnCreate 生成单位四边形") {
    World world;
    Entity e = world.CreateEntity("BB");
    world.AddComponent<TransformComponent>(e);
    auto* bb = world.AddComponent<BillboardComponent>(e);

    // 单位四边形：4 顶点 / 6 索引
    CHECK(bb->GetVertexCount() == 4);
    CHECK(bb->GetIndexCount() == 6);

    // 包围盒 = 单位四边形（本地空间 ±0.5）
    auto b = bb->GetBounds();
    CHECK(b.min.x == doctest::Approx(-0.5f));
    CHECK(b.max.x == doctest::Approx(0.5f));
    CHECK(b.min.y == doctest::Approx(-0.5f));
    CHECK(b.max.y == doctest::Approx(0.5f));

    // 广告牌默认渲染方式：无光照 + 半透明混合 + 双面 + 不投影
    CHECK(bb->unlit == true);
    CHECK(bb->alphaMode == 2);       // AlphaMode::Blend
    CHECK(bb->doubleSided == true);
    CHECK(bb->castShadow == false);
}

TEST_CASE("BillboardComponent::MakeBillboardMatrix 对齐默认相机") {
    // 默认相机：forward = -Z，up = +Y；尺寸 (2,3)；位置 (5,6,7)
    float4x4 m = BillboardComponent::MakeBillboardMatrix(
        float3(5, 6, 7), float3(0, 0, -1), float3(0, 1, 0), float2(2, 3));

    // 列 0 = 相机右 × size.x = (2,0,0)；列 1 = 上 × size.y = (0,3,0)
    CHECK(m[0].x == doctest::Approx(2.0f));
    CHECK(m[0].y == doctest::Approx(0.0f));
    CHECK(m[1].y == doctest::Approx(3.0f));
    CHECK(m[1].x == doctest::Approx(0.0f));
    // 列 2 = 朝向相机 = −forward = (0,0,1)
    CHECK(m[2].z == doctest::Approx(1.0f));
    // 列 3 = 位置
    CHECK(m[3].x == doctest::Approx(5.0f));
    CHECK(m[3].y == doctest::Approx(6.0f));
    CHECK(m[3].z == doctest::Approx(7.0f));

    // 本地右上角 (0.5, 0.5, 0) → 世界 (5+1, 6+1.5, 7)（尺寸半宽 1 / 半高 1.5）
    float4 corner = m * float4(0.5f, 0.5f, 0.0f, 1.0f);
    CHECK(corner.x == doctest::Approx(6.0f));
    CHECK(corner.y == doctest::Approx(7.5f));
    CHECK(corner.z == doctest::Approx(7.0f));

    // 法线（本地 +Z）变换后指向 −forward = (0,0,1) → 面向相机（背向相机视线方向）
    float4 n = m * float4(0.0f, 0.0f, 1.0f, 0.0f);
    CHECK(n.z == doctest::Approx(1.0f));
}

TEST_CASE("BillboardComponent::MakeBillboardMatrix 任意相机朝向正交且朝向相机") {
    // 相机朝 +X 看（forward = (1,0,0)，up = (0,1,0)）
    float4x4 m = BillboardComponent::MakeBillboardMatrix(
        float3(0, 0, 0), float3(1, 0, 0), float3(0, 1, 0), float2(1, 1));

    // 相机右 = cross(forward, up) = (0,0,1)
    float3 right(m[0]);
    float3 up(m[1]);
    float3 normal(m[2]);
    CHECK(right.z == doctest::Approx(1.0f));
    CHECK(up.y == doctest::Approx(1.0f));

    // 三轴两两正交（单位尺寸下）
    CHECK(glm::dot(right, up) == doctest::Approx(0.0f));
    CHECK(glm::dot(right, normal) == doctest::Approx(0.0f));
    CHECK(glm::dot(up, normal) == doctest::Approx(0.0f));

    // 法线 = −forward → 与相机视线方向点积为 −1（正面朝向相机）
    CHECK(glm::dot(normal, float3(1, 0, 0)) == doctest::Approx(-1.0f));

    // 视线方向的四边形始终覆盖屏幕横纵方向：
    // 本地点 (±0.5, 0, 0) 变换后仍与相机右轴共线
    float4 p = m * float4(0.5f, 0.0f, 0.0f, 1.0f);
    CHECK(p.z == doctest::Approx(0.5f));   // 沿 right=(0,0,1) 正半轴
}
