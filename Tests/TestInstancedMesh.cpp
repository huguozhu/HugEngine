// ============================================================
// Tests/TestInstancedMesh.cpp — Phase B1 InstancedMeshComponent 单元测试
//
// 覆盖（CPU 侧；GPU 实例化绘制由 02.Cube 万级实例冒烟验证）：
//   内置单位立方体几何、实例变换设置/脏标记/计数、空变换容错。
//   任务 25 追加：逐实例剔除的组件状态、间接命令结构布局（20 字节）、
//     push constant 布局（144 字节，与 InstancedCull.comp.slang 逐字段对齐），
//     以及剔除数学本身（与 GPU 侧同一套公式，用 CPU 复算钉住行为）。
// ============================================================

#include "doctest.h"

#include "Scene/World.h"
#include "Scene/Transform.h"
#include "Scene/InstancedMeshComponent.h"
#include "Pipeline/InstanceCuller.h"
#include "Math/Geometry.h"

#include <cstddef>

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

// ============================================================
// 任务 25：逐实例 GPU 视锥剔除
// ============================================================

TEST_CASE("逐实例剔除：组件状态与间接命令按飞行帧分槽") {
    World world;
    Entity e = world.CreateEntity("Inst");
    world.AddComponent<TransformComponent>(e);
    auto* im = world.AddComponent<InstancedMeshComponent>(e);

    // 默认关（与原"整批绘制"行为一致，便于 A/B 对比）
    CHECK(im->enableFrustumCull == false);
    // 命令缓冲每飞行帧一份（避免本帧 CPU 清零与上帧 GPU 间接绘制打架）
    CHECK(std::size(im->instanceCullCmd) == rhi::kMaxFramesInFlight);
    CHECK(im->instanceCullCmdHandle[0] == 0);
    CHECK(im->visibleInstanceCount == 0);
    // 容量常量：可见列表上限
    CHECK(he::render::InstanceCuller::kMaxInstances == 100000u);
}

TEST_CASE("逐实例剔除：间接命令与 push constant 的布局与 shader 对齐") {
    using Cmd = he::render::InstanceIndirectCommand;
    // 间接命令必须与 VkDrawIndexedIndirectCommand 同布局（20 字节，5 个 u32/i32）
    static_assert(sizeof(Cmd) == 20, "间接命令必须是 20 字节");
    CHECK(offsetof(Cmd, indexCount)    == 0);
    CHECK(offsetof(Cmd, instanceCount) == 4);
    CHECK(offsetof(Cmd, firstIndex)    == 8);
    CHECK(offsetof(Cmd, vertexOffset)  == 12);
    CHECK(offsetof(Cmd, firstInstance) == 16);

    using P = he::render::InstancedCullParams;
    static_assert(sizeof(P) == 144, "push constant 必须与 InstancedCull.comp.slang 一致");
    CHECK(offsetof(P, planes)              == 0);
    CHECK(offsetof(P, localBoundsMin)      == 96);
    CHECK(offsetof(P, localBoundsMax)      == 112);
    CHECK(offsetof(P, instanceCount)       == 128);
    CHECK(offsetof(P, instanceBufferHandle) == 132);
    CHECK(offsetof(P, visibleBufferHandle) == 136);
    CHECK(offsetof(P, commandHandle)       == 140);
}

TEST_CASE("逐实例剔除：六平面判定的保守行为（与 GPU 侧同一公式）") {
    // 相机在原点看向 +Z（左手/右手不影响这里：只验证"盒在视锥内/外"的判定关系）
    const float4x4 view = glm::lookAt(float3(0.0f), float3(0.0f, 0.0f, 1.0f), float3(0, 1, 0));
    const float4x4 proj = glm::perspective(glm::radians(60.0f), 1.0f, 0.1f, 100.0f);
    const Frustum  fr   = Frustum::FromViewProj(proj * view);

    // 与 shader 中 AABBOutside 同一实现（support point + 0.01 余量）
    auto outside = [&](const float3& mn, const float3& mx) {
        for (int p = 0; p < 6; ++p) {
            const float4 pl = fr.planes[p];
            const float3 n  = float3(pl);
            const float3 s((n.x >= 0) ? mx.x : mn.x,
                           (n.y >= 0) ? mx.y : mn.y,
                           (n.z >= 0) ? mx.z : mn.z);
            if (glm::dot(n, s) + pl.w < -0.01f) return true;
        }
        return false;
    };

    // 正前方 10 米处的盒子：可见
    CHECK(outside(float3(-0.5f, -0.5f, 9.5f), float3(0.5f, 0.5f, 10.5f)) == false);
    // 相机背后：剔除
    CHECK(outside(float3(-0.5f, -0.5f, -10.5f), float3(0.5f, 0.5f, -9.5f)) == true);
    // 远超出远平面：剔除
    CHECK(outside(float3(-0.5f, -0.5f, 200.0f), float3(0.5f, 0.5f, 201.0f)) == true);
    // 侧面很远：剔除
    CHECK(outside(float3(90.0f, -0.5f, 9.5f), float3(91.0f, 0.5f, 10.5f)) == true);
    // 跨近平面的盒子（部分在视锥内）：保守判定为**可见**（不许误剔）
    CHECK(outside(float3(-0.5f, -0.5f, -1.0f), float3(0.5f, 0.5f, 1.0f)) == false);
}
