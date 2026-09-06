// ============================================================
// Tests/TestInstancedMesh.cpp — Phase B1 InstancedMeshComponent 单元测试
//
// 覆盖（CPU 侧；GPU 实例化绘制由 02.Cube 万级实例冒烟验证）：
//   内置单位立方体几何、实例变换设置/脏标记/计数、空变换容错。
// ============================================================

#include "doctest.h"

#include "Scene/World.h"
#include "Scene/Transform.h"
#include "Scene/InstancedMeshComponent.h"

using namespace he;

TEST_CASE("InstancedMeshComponent::OnCreate 生成单位立方体") {
    World world;
    Entity e = world.CreateEntity("Inst");
    world.AddComponent<TransformComponent>(e);
    auto* im = world.AddComponent<InstancedMeshComponent>(e);

    // 单位立方体：24 顶点 / 36 索引，包围盒 ±0.5
    CHECK(im->GetVertexCount() == 24);
    CHECK(im->GetIndexCount() == 36);
    auto b = im->GetBounds();
    CHECK(b.min.x == doctest::Approx(-0.5f));
    CHECK(b.max.x == doctest::Approx(0.5f));
}

TEST_CASE("InstancedMeshComponent 实例变换与脏标记") {
    World world;
    Entity e = world.CreateEntity("Inst");
    world.AddComponent<TransformComponent>(e);
    auto* im = world.AddComponent<InstancedMeshComponent>(e);

    // 初始无实例
    CHECK(im->GetInstanceCount() == 0);
    CHECK(im->bTransformsDirty == false);

    // 设置 100 个实例：计数更新 + 置脏（渲染管线下一帧重建 GPU 缓冲）
    std::vector<float4x4> xforms;
    for (int i = 0; i < 100; ++i) {
        float4x4 m(1.0f);
        m[3] = float4((float)i, 0.0f, 0.0f, 1.0f);
        xforms.push_back(m);
    }
    im->SetInstanceTransforms(std::move(xforms));
    CHECK(im->GetInstanceCount() == 100);
    CHECK(im->bTransformsDirty == true);

    // 变换数据保留（平移分量可读）
    CHECK(im->instanceTransforms[42][3].x == doctest::Approx(42.0f));
    CHECK(im->instanceTransforms[99][3].x == doctest::Approx(99.0f));

    // 空变换：计数 0（渲染 Pass 跳过），不崩溃
    im->SetInstanceTransforms({});
    CHECK(im->GetInstanceCount() == 0);
    CHECK(im->bTransformsDirty == true);
}
