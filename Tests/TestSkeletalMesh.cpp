// ============================================================
// Tests/TestSkeletalMesh.cpp — Phase C C1b 骨骼蒙皮系统单元测试
//
// 覆盖（CPU 侧；GPU 蒙皮绘制由 C1c 演示冒烟）：
//   关节 TRS 采样（静态/插值/slerp）、层级世界矩阵与蒙皮矩阵、
//   播放时间推进/循环/播完停止、组件网格上传与剪辑边界。
// ============================================================

#include "doctest.h"

#include "Scene/World.h"
#include "Scene/Transform.h"
#include "Scene/SkeletonAsset.h"
#include "Scene/SkeletalMeshComponent.h"
#include "Scene/SkeletalMeshSystem.h"

using namespace he;

namespace {
// 最小两关节骨架：root(原点) → child(+X 1米)，child 有平移动画（1→3 米）
std::shared_ptr<asset::SkeletonAsset> MakeChain() {
    auto skel = std::make_shared<asset::SkeletonAsset>();
    asset::SkeletonJoint j0;
    j0.name = "root";
    j0.parent = -1;
    j0.inverseBind = float4x4(1.0f);
    asset::SkeletonJoint j1;
    j1.name = "child";
    j1.parent = 0;
    j1.translation = float3(1, 0, 0);
    j1.inverseBind = float4x4(1.0f);
    skel->joints = { j0, j1 };

    asset::AnimationClip clip;
    clip.name = "Test";
    clip.duration = 1.0f;
    asset::JointAnimationChannel ch;
    ch.jointIndex = 1;
    ch.times = { 0.0f, 1.0f };
    ch.translations = { float3(1, 0, 0), float3(3, 0, 0) };
    clip.channels.push_back(ch);
    skel->clips.push_back(clip);
    return skel;
}

// 带少量蒙皮顶点的骨架
std::shared_ptr<asset::SkeletonAsset> MakeSkinned() {
    auto skel = MakeChain();
    for (int i = 0; i < 3; ++i) {
        asset::SkinnedVertex v{};
        v.position = float3((float)i, 0, 0);
        v.normal   = float3(0, 1, 0);
        v.joint[0] = (u8)i % 2;
        v.weight[0] = 1.0f;
        skel->vertices.push_back(v);
    }
    skel->indices = { 0, 1, 2 };
    return skel;
}
} // namespace

TEST_CASE("SkeletalMeshSystem 关节 TRS 采样") {
    auto skel = MakeChain();
    float3 t;
    quat r;
    float3 s;

    // 无剪辑（-1）→ 静态 TRS
    SkeletalMeshSystem::SampleJointTRS(*skel, -1, 0.0f, 1, t, r, s);
    CHECK(t.x == doctest::Approx(1.0f));

    // 动画中点 → 平移 lerp 1→3 = 2
    SkeletalMeshSystem::SampleJointTRS(*skel, 0, 0.5f, 1, t, r, s);
    CHECK(t.x == doctest::Approx(2.0f).epsilon(0.001));

    // 越界时间钳制到端点
    SkeletalMeshSystem::SampleJointTRS(*skel, 0, 5.0f, 1, t, r, s);
    CHECK(t.x == doctest::Approx(3.0f).epsilon(0.001));

    // 无通道的关节（root）保持静态
    SkeletalMeshSystem::SampleJointTRS(*skel, 0, 0.5f, 0, t, r, s);
    CHECK(t.x == doctest::Approx(0.0f));
}

TEST_CASE("SkeletalMeshSystem 层级世界矩阵与蒙皮矩阵") {
    auto skel = MakeChain();
    std::vector<float4x4> skin, world;

    // t=0.5：child 世界 = root(I) × child(translate 2) → 蒙皮矩阵（invBind=I）同世界
    SkeletalMeshSystem::ComputeSkinMatrices(*skel, 0, 0.5f, skin, &world);
    REQUIRE(skin.size() == 2);
    CHECK(world[0][3].x == doctest::Approx(0.0f));
    CHECK(world[1][3].x == doctest::Approx(2.0f).epsilon(0.001));
    CHECK(skin[1][3].x == doctest::Approx(2.0f).epsilon(0.001));   // invBind = I

    // 绑定姿势（clip=-1）→ child 在 x=1
    SkeletalMeshSystem::ComputeSkinMatrices(*skel, -1, 0.0f, skin, &world);
    CHECK(world[1][3].x == doctest::Approx(1.0f).epsilon(0.001));
}

TEST_CASE("SkeletalMeshComponent 播放推进/循环/播完停止") {
    World world;
    Entity e = world.CreateEntity("Skel");
    world.AddComponent<TransformComponent>(e);
    auto* sm = world.AddComponent<SkeletalMeshComponent>(e);
    sm->SetSkeleton(MakeChain());

    // 循环播放：1 秒剪辑 × 循环
    sm->PlayClip(0, true);
    CHECK(sm->currentClip == 0);
    SkeletalMeshSystem::Update(world, 0.6f);
    CHECK(sm->clipTime == doctest::Approx(0.6f).epsilon(0.001));
    CHECK(sm->bBonesDirty == true);
    CHECK(sm->boneMatrices.size() == 2);
    SkeletalMeshSystem::Update(world, 0.6f);   // 累计 1.2 → 回绕 0.2
    CHECK(sm->clipTime == doctest::Approx(0.2f).epsilon(0.01));

    // 不循环：播完停在末帧
    sm->PlayClip(0, false);
    SkeletalMeshSystem::Update(world, 1.5f);
    CHECK(sm->playing == false);
    CHECK(sm->clipTime == doctest::Approx(1.0f).epsilon(0.001));
    // 播完后骨骼矩阵停在末帧姿态（child x=3）
    CHECK(sm->boneMatrices[1][3].x == doctest::Approx(3.0f).epsilon(0.001));

    // 非法剪辑下标 → 绑定姿势
    sm->PlayClip(99);
    CHECK(sm->currentClip == -1);
}

TEST_CASE("SkeletalMeshComponent 蒙皮网格上传（无设备容错）") {
    World world;
    Entity e = world.CreateEntity("Skel");
    world.AddComponent<TransformComponent>(e);
    auto* sm = world.AddComponent<SkeletalMeshComponent>(e);
    sm->SetSkeleton(MakeSkinned());

    // 无 RHI 设备：顶点/索引计数与包围盒仍正确（缓冲创建跳过）
    CHECK(sm->GetVertexCount() == 3);
    CHECK(sm->GetIndexCount() == 3);
    auto b = sm->GetBounds();
    CHECK(b.min.x == doctest::Approx(0.0f));
    CHECK(b.max.x == doctest::Approx(2.0f));

    // 空骨架安全
    sm->SetSkeleton(nullptr);
    CHECK(sm->GetVertexCount() == 0);
    CHECK(sm->jointWorldMatrices.empty());
    SkeletalMeshSystem::Update(world, 0.016f);   // 不崩溃
}
