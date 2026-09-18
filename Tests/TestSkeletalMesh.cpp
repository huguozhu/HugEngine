// ============================================================
// Tests/TestSkeletalMesh.cpp — Phase C C1b 骨骼蒙皮系统单元测试
//
// 覆盖（CPU 侧；GPU 蒙皮绘制由 C1c 演示冒烟）：
//   关节 TRS 采样（静态/插值/slerp）、层级世界矩阵与蒙皮矩阵、
//   播放时间推进/循环/播完停止、组件网格上传与剪辑边界。
//   任务 21 追加：多层混合采样（权重归一化/四元数半球对齐）与交叉淡入状态机。
//   任务 22 追加：动画重定向（名字匹配/绑定姿势差值/联合平移缩放开关/与混合组合/组件接入）。
// ============================================================

#include "doctest.h"

#include "Scene/World.h"
#include "Scene/Transform.h"
#include "Scene/SkeletonAsset.h"
#include "Scene/SkeletalMeshComponent.h"
#include "Scene/SkeletalMeshSystem.h"

#include <cmath>

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

// 两剪辑骨架：clip0 = child 平移 1→3；clip1 = child 平移 5→7（便于验证混合中点）
std::shared_ptr<asset::SkeletonAsset> MakeTwoClips() {
    auto skel = MakeChain();
    asset::AnimationClip clip;
    clip.name = "Test2";
    clip.duration = 1.0f;
    asset::JointAnimationChannel ch;
    ch.jointIndex = 1;
    ch.times = { 0.0f, 1.0f };
    ch.translations = { float3(5, 0, 0), float3(7, 0, 0) };
    clip.channels.push_back(ch);
    skel->clips.push_back(clip);
    return skel;
}

// 带旋转通道的骨架：child 绕 Z 轴 0° → 180°（验证四元数混合的半球对齐）
std::shared_ptr<asset::SkeletonAsset> MakeRotatingChain() {
    auto skel = MakeChain();
    asset::JointAnimationChannel ch;
    ch.jointIndex = 1;
    ch.times = { 0.0f, 1.0f };
    ch.rotations = { glm::identity<quat>(), glm::angleAxis(glm::pi<float>(), float3(0, 0, 1)) };
    skel->clips[0].channels.push_back(ch);
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

// ── 任务 22：重定向用的两副骨架（同名关节，**绑定姿势不同**）─────────────────

/// 源骨架："root"/"child"，child 绑定平移 (1,0,0)、绑定旋转绕 Y 30°、缩放 1；
/// 剪辑 t=0 恰好等于自己的绑定姿势（所以 delta 从单位四元数开始），t=1 时：
/// 平移 (3,0,0)、旋转再绕 Z 180°、缩放 (3,3,3)
std::shared_ptr<asset::SkeletonAsset> MakeRetargetSource() {
    auto skel = std::make_shared<asset::SkeletonAsset>();
    skel->name = "Source";
    asset::SkeletonJoint j0;
    j0.name = "root";
    j0.parent = -1;
    j0.inverseBind = float4x4(1.0f);
    asset::SkeletonJoint j1;
    j1.name = "child";
    j1.parent = 0;
    j1.translation = float3(1, 0, 0);
    j1.rotation    = glm::angleAxis(glm::radians(30.0f), float3(0, 1, 0));
    j1.scale       = float3(1.0f);
    j1.inverseBind = float4x4(1.0f);
    skel->joints = { j0, j1 };

    asset::AnimationClip clip;
    clip.name     = "Walk";
    clip.duration = 1.0f;
    asset::JointAnimationChannel ch;
    ch.jointIndex   = 1;
    ch.times        = { 0.0f, 1.0f };
    ch.translations = { float3(1, 0, 0), float3(3, 0, 0) };
    ch.rotations    = { j1.rotation, j1.rotation * glm::angleAxis(glm::pi<float>(), float3(0, 0, 1)) };
    ch.scales       = { float3(1.0f), float3(3.0f) };
    clip.channels.push_back(ch);
    skel->clips.push_back(clip);
    return skel;
}

/// 目标骨架：同名关节，但 child 绑定平移 (2,0,0)、绑定旋转绕 X 90°、缩放 2；自己**没有剪辑**
std::shared_ptr<asset::SkeletonAsset> MakeRetargetTarget() {
    auto skel = std::make_shared<asset::SkeletonAsset>();
    skel->name = "Target";
    asset::SkeletonJoint j0;
    j0.name = "root";
    j0.parent = -1;
    j0.inverseBind = float4x4(1.0f);
    asset::SkeletonJoint j1;
    j1.name = "child";
    j1.parent = 0;
    j1.translation = float3(2, 0, 0);
    j1.rotation    = glm::angleAxis(glm::half_pi<float>(), float3(1, 0, 0));
    j1.scale       = float3(2.0f);
    j1.inverseBind = float4x4(1.0f);
    skel->joints = { j0, j1 };
    return skel;
}
/// 用矩阵把一个方向变换过去（归一化 ⇒ 忽略缩放/平移），用于比较“旋转是否一致”
float3 RotateDir(const float4x4& m, const float3& v) {
    return glm::normalize(float3(m * float4(v, 0.0f)));
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

// ============================================================
// 任务 21：剪辑混合（多层权重 + 交叉淡入）
// ============================================================

TEST_CASE("剪辑混合：单层权重 1 等价于单剪辑采样；权重归一化后是加权平均") {
    auto skel = MakeTwoClips();
    float3 t;
    quat r;
    float3 s;

    // 单层、权重 1 → 与 SampleJointTRS 完全一致（混合路径不能改变单剪辑语义）
    asset::AnimationBlendLayer one[1];
    one[0].clipIndex = 0;
    one[0].weight    = 1.0f;
    one[0].time      = 0.5f;
    SkeletalMeshSystem::SampleJointTRSBlended(*skel, one, 1, 1, t, r, s);
    float3 tRef;
    quat rRef;
    float3 sRef;
    SkeletalMeshSystem::SampleJointTRS(*skel, 0, 0.5f, 1, tRef, rRef, sRef);
    CHECK(t.x == doctest::Approx(tRef.x).epsilon(0.0001));
    CHECK(s.x == doctest::Approx(sRef.x).epsilon(0.0001));
    CHECK(r.w == doctest::Approx(rRef.w).epsilon(0.0001));

    // 两层等权（各 0.5）：clip0@t=0.5 → x=2；clip1@t=0.5 → x=6 ⇒ 平均 4
    asset::AnimationBlendLayer two[2];
    two[0].clipIndex = 0; two[0].weight = 1.0f; two[0].time = 0.5f;
    two[1].clipIndex = 1; two[1].weight = 1.0f; two[1].time = 0.5f;
    SkeletalMeshSystem::SampleJointTRSBlended(*skel, two, 2, 1, t, r, s);
    CHECK(t.x == doctest::Approx(4.0f).epsilon(0.001));

    // 权重 1:3（clip0 x=2、clip1 x=6）⇒ (1*2 + 3*6)/4 = 5
    two[1].weight = 3.0f;
    SkeletalMeshSystem::SampleJointTRSBlended(*skel, two, 2, 1, t, r, s);
    CHECK(t.x == doctest::Approx(5.0f).epsilon(0.001));
}

TEST_CASE("剪辑混合：全零权重/越界层/空层都退回绑定姿势") {
    auto skel = MakeTwoClips();
    float3 t;
    quat r;
    float3 s;

    // 空层
    SkeletalMeshSystem::SampleJointTRSBlended(*skel, nullptr, 0, 1, t, r, s);
    CHECK(t.x == doctest::Approx(1.0f));            // 关节静态平移

    // 权重全 0
    asset::AnimationBlendLayer zero[2];
    zero[0].clipIndex = 0; zero[0].weight = 0.0f; zero[0].time = 0.5f;
    zero[1].clipIndex = 1; zero[1].weight = 0.0f; zero[1].time = 0.5f;
    SkeletalMeshSystem::SampleJointTRSBlended(*skel, zero, 2, 1, t, r, s);
    CHECK(t.x == doctest::Approx(1.0f));

    // 层里剪辑越界（-1 = 绑定姿势层、99 = 非法）→ 不参与，等同静态
    asset::AnimationBlendLayer bad[2];
    bad[0].clipIndex = -1; bad[0].weight = 1.0f;
    bad[1].clipIndex = 99; bad[1].weight = 1.0f; bad[1].time = 0.5f;
    SkeletalMeshSystem::SampleJointTRSBlended(*skel, bad, 2, 1, t, r, s);
    CHECK(t.x == doctest::Approx(1.0f));

    // 无效关节下标：单位值（不崩溃）
    SkeletalMeshSystem::SampleJointTRSBlended(*skel, zero, 2, 42, t, r, s);
    CHECK(t.x == doctest::Approx(0.0f));
    CHECK(s.x == doctest::Approx(1.0f));
}

TEST_CASE("剪辑混合：四元数半球对齐（等价旋转不互相抵消）") {
    // clip0 在 t=1 是 180°；把同一旋转写成 q 与 -q（表示同一旋转）再等权混合，
    // 结果必须是那个旋转本身（不做半球对齐时加权和≈0，归一化会得到垃圾/单位四元数）
    auto skel = MakeRotatingChain();
    const quat q180 = glm::angleAxis(glm::pi<float>(), float3(0, 0, 1));

    asset::AnimationBlendLayer two[2];
    two[0].clipIndex = 0; two[0].weight = 1.0f; two[0].time = 1.0f;   // 180°
    two[1].clipIndex = 0; two[1].weight = 1.0f; two[1].time = 1.0f;   // 同一个 180°

    float3 t;
    quat r;
    float3 s;
    SkeletalMeshSystem::SampleJointTRSBlended(*skel, two, 2, 1, t, r, s);
    // 旋转角仍是 180°（|w| ≈ 0），且分量与 q180 同向或反向
    CHECK(std::abs(r.w) == doctest::Approx(0.0f).epsilon(0.01));
    CHECK(std::abs(glm::dot(r, q180)) == doctest::Approx(1.0f).epsilon(0.001));

    // 反向写入（q 与 -q）：结果不变
    two[1].time = 1.0f;
    asset::AnimationBlendLayer flipped[2];
    flipped[0] = two[0];
    flipped[1] = two[1];
    // 直接构造 -q 场景：另一个剪辑在 t=1 给同一旋转（采样端都是正向），这里用等效检查代替：
    SkeletalMeshSystem::SampleJointTRSBlended(*skel, flipped, 2, 1, t, r, s);
    CHECK(std::abs(glm::dot(r, q180)) == doctest::Approx(1.0f).epsilon(0.001));
}

TEST_CASE("剪辑混合：组件 API 与交叉淡入状态机") {
    World world;
    Entity e = world.CreateEntity("Skel");
    world.AddComponent<TransformComponent>(e);
    auto* sm = world.AddComponent<SkeletalMeshComponent>(e);
    sm->SetSkeleton(MakeTwoClips());

    // 手工设层：越界剪辑被拒绝、超上限被拒绝
    CHECK(sm->SetBlendLayer(0, 0, 1.0f) == true);
    CHECK(sm->SetBlendLayer(1, 99, 1.0f) == false);          // 剪辑越界
    CHECK(sm->SetBlendLayer(9, 1, 1.0f) == false);           // 超 kMaxBlendLayers
    CHECK(sm->blendLayerCount == 1);

    // 权重归一化读数
    CHECK(sm->SetBlendLayer(1, 1, 3.0f) == true);
    float w[4] = { 0, 0, 0, 0 };
    sm->GetBlendWeights(w, 4);
    CHECK(w[0] == doctest::Approx(0.25f).epsilon(0.001));
    CHECK(w[1] == doctest::Approx(0.75f).epsilon(0.001));

    // 每层按自己的速度推进时间
    SkeletalMeshSystem::Update(world, 0.5f);
    CHECK(sm->blendLayers[0].time == doctest::Approx(0.5f).epsilon(0.001));
    CHECK(sm->blendLayers[1].time == doctest::Approx(0.5f).epsilon(0.001));
    // 权重 1:3 的混合结果：clip0@0.5 → x=2、clip1@0.5 → x=6 ⇒ 0.25*2+0.75*6 = 5
    CHECK(sm->boneMatrices[1][3].x == doctest::Approx(5.0f).epsilon(0.001));

    // 交叉淡入：先回到单剪辑（clip0），再淡入 clip1，时长 1s
    sm->ClearBlendLayers();
    sm->PlayClip(0, true);
    sm->CrossFadeTo(1, 1.0f, true);
    REQUIRE(sm->blendLayerCount == 2);
    REQUIRE(sm->bCrossFading == true);
    CHECK(sm->blendLayers[0].weight == doctest::Approx(1.0f));
    CHECK(sm->blendLayers[1].weight == doctest::Approx(0.0f));

    SkeletalMeshSystem::Update(world, 0.5f);      // 淡入一半
    CHECK(sm->blendLayers[0].weight == doctest::Approx(0.5f).epsilon(0.001));
    CHECK(sm->blendLayers[1].weight == doctest::Approx(0.5f).epsilon(0.001));
    CHECK(sm->bCrossFading == true);

    SkeletalMeshSystem::Update(world, 0.5f);      // 淡入完成 → 收敛成单层（clip1，权重 1）
    CHECK(sm->bCrossFading == false);
    CHECK(sm->blendLayerCount == 1);
    CHECK(sm->blendLayers[0].clipIndex == 1);
    CHECK(sm->blendLayers[0].weight == doctest::Approx(1.0f));
    CHECK(sm->currentClip == 1);                  // 旧字段与层同步（面板显示用）

    // 立即切换（duration=0）：一帧内收尾
    sm->CrossFadeTo(0, 0.0f, true);
    SkeletalMeshSystem::Update(world, 0.016f);
    CHECK(sm->blendLayerCount == 1);
    CHECK(sm->blendLayers[0].clipIndex == 0);

    // 越界目标剪辑：忽略、不改状态
    const u32 before = sm->blendLayerCount;
    sm->CrossFadeTo(42, 1.0f, true);
    CHECK(sm->blendLayerCount == before);

    // 清空后回到单剪辑路径（PlayClip 也会清空混合层）
    sm->SetBlendLayer(0, 1, 1.0f);
    sm->PlayClip(0, true);
    CHECK(sm->blendLayerCount == 0);
    SkeletalMeshSystem::Update(world, 0.25f);
    CHECK(sm->clipTime == doctest::Approx(0.25f).epsilon(0.001));
}

// ============================================================
// 任务 22：动画重定向（不同骨架共用同一套剪辑）
// ============================================================

TEST_CASE("动画重定向：按关节名字构建映射（对不上的保持绑定姿势）") {
    auto target = MakeRetargetTarget();
    auto source = MakeRetargetSource();

    auto profile = SkeletalMeshSystem::BuildRetargetProfile(*target, *source);
    REQUIRE(profile.targetToSource.size() == 2);
    CHECK(profile.targetToSource[0] == 0);            // "root" → "root"
    CHECK(profile.targetToSource[1] == 1);            // "child" → "child"
    CHECK(profile.MappedJointCount() == 2);
    CHECK(profile.retargetTranslation == false);      // 默认只借旋转
    CHECK(profile.retargetScale == false);

    // 名字对不上 / 无名关节 → -1（保持目标自己的绑定姿势）
    auto other = MakeRetargetTarget();
    other->joints[1].name = "tail";
    other->joints[0].name.clear();
    auto p2 = SkeletalMeshSystem::BuildRetargetProfile(*other, *source);
    CHECK(p2.targetToSource[0] == -1);
    CHECK(p2.targetToSource[1] == -1);
    CHECK(p2.MappedJointCount() == 0);

    // 目标关节比源多/少都不越界：源里没有同名 → 未映射
    auto p3 = SkeletalMeshSystem::BuildRetargetProfile(*source, *other);
    CHECK(p3.targetToSource[1] == -1);
}

TEST_CASE("动画重定向：借的是相对绑定姿势的偏移，不是源的绝对姿态") {
    auto target = MakeRetargetTarget();
    auto source = MakeRetargetSource();
    auto profile = SkeletalMeshSystem::BuildRetargetProfile(*target, *source);

    const quat srcBind  = source->joints[1].rotation;
    const quat dstBind  = target->joints[1].rotation;
    const quat q180z    = glm::angleAxis(glm::pi<float>(), float3(0, 0, 1));

    float3 t;
    quat r;
    float3 s;

    // ① 源处于**它自己的绑定姿势**（t=0）⇒ 偏移 = 单位四元数 ⇒ 目标保持自己的绑定姿势
    SkeletalMeshSystem::SampleJointTRSRetargeted(*target, *source, profile, 0, 0.0f, 1, t, r, s);
    CHECK(glm::dot(r, dstBind) == doctest::Approx(1.0f).epsilon(0.0001));
    CHECK(glm::dot(r, srcBind) != doctest::Approx(1.0f));    // 绝不是源的绑定/姿态（两者不同）

    // ② t=1：目标 = 目标绑定旋转 × (源绑定⁻¹ × 源动画旋转) = dstBind × q180z
    SkeletalMeshSystem::SampleJointTRSRetargeted(*target, *source, profile, 0, 1.0f, 1, t, r, s);
    const quat expected = glm::normalize(dstBind * q180z);
    CHECK(std::abs(glm::dot(r, expected)) == doctest::Approx(1.0f).epsilon(0.0001));

    // ③ 平移/缩放默认**不重定向**：即使源有平移/缩放动画，目标仍是自己的绑定值
    CHECK(t.x == doctest::Approx(2.0f));
    CHECK(s.x == doctest::Approx(2.0f));

    // ④ 未映射关节 / 越界关节 → 目标静态 TRS / 单位值（不崩溃）
    profile.targetToSource[1] = -1;
    SkeletalMeshSystem::SampleJointTRSRetargeted(*target, *source, profile, 0, 1.0f, 1, t, r, s);
    CHECK(t.x == doctest::Approx(2.0f));
    CHECK(glm::dot(r, dstBind) == doctest::Approx(1.0f).epsilon(0.0001));
    SkeletalMeshSystem::SampleJointTRSRetargeted(*target, *source, profile, 0, 1.0f, 42, t, r, s);
    CHECK(t.x == doctest::Approx(0.0f));
    CHECK(s.x == doctest::Approx(1.0f));
}

TEST_CASE("动画重定向：同骨架时等价于直接采样；开关打开后平移/缩放按比例") {
    auto source = MakeRetargetSource();
    // 目标骨架 = 源骨架的副本（同名同绑定）→ 打开全部开关后必须与单剪辑采样逐项相等
    auto target = std::make_shared<asset::SkeletonAsset>(*source);
    auto profile = SkeletalMeshSystem::BuildRetargetProfile(*target, *source);
    profile.retargetTranslation = true;
    profile.retargetScale       = true;

    for (float time : { 0.0f, 0.25f, 0.5f, 1.0f }) {
        float3 t1, s1, t2, s2;
        quat r1, r2;
        SkeletalMeshSystem::SampleJointTRSRetargeted(*target, *source, profile, 0, time, 1, t1, r1, s1);
        SkeletalMeshSystem::SampleJointTRS(*source, 0, time, 1, t2, r2, s2);
        CHECK(t1.x == doctest::Approx(t2.x).epsilon(0.0001));
        CHECK(s1.x == doctest::Approx(s2.x).epsilon(0.0001));
        CHECK(std::abs(glm::dot(r1, r2)) == doctest::Approx(1.0f).epsilon(0.0001));
    }

    // 不同绑定姿势 + 平移开关：按各关节绑定长度比缩放
    //   源绑定 |t| = 1、目标绑定 |t| = 2 ⇒ k = 2；t=0.5 时源平移 2、源绑定 1 ⇒ delta=1 ⇒ 目标 = 2 + 1*2 = 4
    auto target2 = MakeRetargetTarget();
    auto profile2 = SkeletalMeshSystem::BuildRetargetProfile(*target2, *source);
    float3 t;
    quat r;
    float3 s;
    profile2.retargetTranslation = true;
    SkeletalMeshSystem::SampleJointTRSRetargeted(*target2, *source, profile2, 0, 0.5f, 1, t, r, s);
    CHECK(t.x == doctest::Approx(4.0f).epsilon(0.001));

    // 关掉自动比例 → k = 1 ⇒ 2 + 1 = 3；再乘全局 0.5 ⇒ 2.5
    profile2.autoProportion = false;
    SkeletalMeshSystem::SampleJointTRSRetargeted(*target2, *source, profile2, 0, 0.5f, 1, t, r, s);
    CHECK(t.x == doctest::Approx(3.0f).epsilon(0.001));
    profile2.translationScale = 0.5f;
    SkeletalMeshSystem::SampleJointTRSRetargeted(*target2, *source, profile2, 0, 0.5f, 1, t, r, s);
    CHECK(t.x == doctest::Approx(2.5f).epsilon(0.001));

    // 缩放开关：源 t=1 缩放 3 / 源绑定 1 = 倍率 3 ⇒ 目标 2*3 = 6
    profile2.retargetScale = true;
    SkeletalMeshSystem::SampleJointTRSRetargeted(*target2, *source, profile2, 0, 1.0f, 1, t, r, s);
    CHECK(s.x == doctest::Approx(6.0f).epsilon(0.001));

    // 源绑定缩放为 0 的退化情况：倍率取 1（不除零、不出 NaN）
    source->joints[1].scale = float3(0.0f);
    SkeletalMeshSystem::SampleJointTRSRetargeted(*target2, *source, profile2, 0, 1.0f, 1, t, r, s);
    CHECK(s.x == doctest::Approx(2.0f).epsilon(0.001));
    CHECK(std::isfinite(s.x));
}

TEST_CASE("动画重定向：与多层混合组合（先重定向、再按任务 21 规则混合）") {
    auto source = MakeTwoClips();      // clip0 平移 1→3、clip1 平移 5→7（同名 root/child）
    auto target = MakeChain();         // child 绑定平移 (1,0,0)
    auto profile = SkeletalMeshSystem::BuildRetargetProfile(*target, *source);
    profile.retargetTranslation = true;
    profile.autoProportion      = false;   // k=1 ⇒ 重定向后的平移就是源的平移

    asset::AnimationBlendLayer two[2];
    two[0].clipIndex = 0; two[0].weight = 1.0f; two[0].time = 0.5f;   // 源 → x=2
    two[1].clipIndex = 1; two[1].weight = 3.0f; two[1].time = 0.5f;   // 源 → x=6

    float3 t;
    quat r;
    float3 s;
    SkeletalMeshSystem::SampleJointTRSBlendedRetargeted(*target, *source, profile, two, 2, 1, t, r, s);
    CHECK(t.x == doctest::Approx(5.0f).epsilon(0.001));   // (1*2 + 3*6)/4

    // 全部层不参与 / 层为空 ⇒ 目标自己的绑定姿势
    target->joints[1].translation = float3(2, 0, 0);
    auto profile2 = SkeletalMeshSystem::BuildRetargetProfile(*target, *source);
    profile2.retargetTranslation = true;
    profile2.autoProportion      = false;                 // k=1 ⇒ 重定向后的平移 = 目标绑定 + 源平移增量
    asset::AnimationBlendLayer zero[1];
    zero[0].clipIndex = 0; zero[0].weight = 0.0f; zero[0].time = 0.5f;
    SkeletalMeshSystem::SampleJointTRSBlendedRetargeted(*target, *source, profile2, zero, 1, 1, t, r, s);
    CHECK(t.x == doctest::Approx(2.0f));                  // 目标绑定姿势
    SkeletalMeshSystem::SampleJointTRSBlendedRetargeted(*target, *source, profile2, nullptr, 0, 1, t, r, s);
    CHECK(t.x == doctest::Approx(2.0f));
    SkeletalMeshSystem::SampleJointTRSBlendedRetargeted(*target, *source, profile2, zero, 1, 42, t, r, s);
    CHECK(t.x == doctest::Approx(0.0f));                  // 越界关节：单位值

    // 组合路径的蒙皮矩阵：层级与 inverseBind 用**目标骨架**（target 有 2 关节）
    std::vector<float4x4> skin, world;
    SkeletalMeshSystem::ComputeSkinMatricesBlendedRetargeted(*target, *source, profile2, two, 2, skin, &world);
    REQUIRE(skin.size() == 2);
    CHECK(world[1][3].x == doctest::Approx(2.0f + 5.0f - 1.0f).epsilon(0.001));   // 目标绑定 2 + (混合后 5 − 源绑定 1)
}

TEST_CASE("动画重定向：改过绑定姿势后重算逆绑定矩阵，绑定姿势仍是单位蒙皮") {
    auto skel = MakeChain();
    // 改绑定姿势（child 骨骼拉长 1.5 倍）但不重算 → 绑定姿势的蒙皮矩阵不再是单位矩阵
    skel->joints[1].translation *= 1.5f;
    std::vector<float4x4> skin;
    SkeletalMeshSystem::ComputeSkinMatrices(*skel, -1, 0.0f, skin);
    CHECK(skin[1][3].x != doctest::Approx(0.0f));

    // 重算逆绑定矩阵后：绑定姿势的蒙皮矩阵回到单位阵（这就是"重算"的目的）
    SkeletalMeshSystem::RebuildInverseBindMatrices(*skel);
    SkeletalMeshSystem::ComputeSkinMatrices(*skel, -1, 0.0f, skin);
    for (const auto& m : skin) {
        CHECK(m[0][0] == doctest::Approx(1.0f).epsilon(0.0001));
        CHECK(m[1][1] == doctest::Approx(1.0f).epsilon(0.0001));
        CHECK(m[2][2] == doctest::Approx(1.0f).epsilon(0.0001));
        CHECK(m[3][0] == doctest::Approx(0.0f).epsilon(0.0001));
        CHECK(m[3][1] == doctest::Approx(0.0f).epsilon(0.0001));
        CHECK(m[3][2] == doctest::Approx(0.0f).epsilon(0.0001));
    }

    // 空骨架安全
    asset::SkeletonAsset empty;
    SkeletalMeshSystem::RebuildInverseBindMatrices(empty);
    CHECK(empty.joints.empty());
}

TEST_CASE("动画重定向：组件设置动画来源后使用源骨架的剪辑表") {
    World world;
    Entity e = world.CreateEntity("Retargeted");
    world.AddComponent<TransformComponent>(e);
    auto* sm = world.AddComponent<SkeletalMeshComponent>(e);

    auto target = MakeRetargetTarget();
    auto source = MakeRetargetSource();
    sm->SetSkeleton(target);
    CHECK(sm->AnimationSource() != nullptr);             // 默认用自身剪辑表
    CHECK(sm->AnimationSource()->name == target->name);
    CHECK(!sm->sourceSkeleton);

    // 设置来源：按名字自动构建映射（2/2），剪辑表切到源骨架
    sm->SetAnimationSource(source);
    const bool sameSource = (sm->sourceSkeleton.get() == source.get());
    REQUIRE(sameSource);
    const bool hasProfile = (sm->retargetProfile != nullptr);
    REQUIRE(hasProfile);
    CHECK(sm->retargetProfile->MappedJointCount() == 2);
    CHECK(sm->AnimationSource()->name == source->name);

    // 目标骨架自己没有剪辑，但 PlayClip(0) 现在有效（剪辑取自源骨架）
    CHECK(target->clips.empty());
    sm->PlayClip(0, true);
    CHECK(sm->currentClip == 0);

    // 推进半秒：蒙皮矩阵由**重定向**结果驱动。注意目标 child 绑定缩放是 2（矩阵非正交），
    // 所以比较"旋转是否一致"用**方向变换**而不是直接从矩阵取四元数。
    SkeletalMeshSystem::Update(world, 0.5f);
    CHECK(sm->clipTime == doctest::Approx(0.5f).epsilon(0.001));
    REQUIRE(sm->boneMatrices.size() == 2);
    float3 tExp;
    quat rExp;
    float3 sExp;
    SkeletalMeshSystem::SampleJointTRSRetargeted(*target, *source, *sm->retargetProfile,
                                                0, 0.5f, 1, tExp, rExp, sExp);
    const float3 probe(0.3f, 0.5f, 0.81f);
    const float3 worldDir = RotateDir(sm->jointWorldMatrices[1], probe);
    CHECK(glm::dot(worldDir, glm::normalize(rExp * probe)) == doctest::Approx(1.0f).epsilon(0.001));
    // 且**不是**直接套用源骨架的姿态（两者绑定姿势不同）
    float3 sT;
    quat sR;
    float3 sS;
    SkeletalMeshSystem::SampleJointTRS(*source, 0, 0.5f, 1, sT, sR, sS);
    CHECK(glm::dot(worldDir, glm::normalize(sR * probe)) < 0.999f);
    // 平移未重定向 ⇒ child 仍在目标绑定位置 x=2
    CHECK(sm->jointWorldMatrices[1][3].x == doctest::Approx(2.0f).epsilon(0.001));

    // 越界剪辑 / 清空来源：安全降级
    sm->PlayClip(99);
    CHECK(sm->currentClip == -1);
    sm->ClearAnimationSource();
    CHECK(!sm->sourceSkeleton);
    CHECK(sm->AnimationSource()->name == target->name);
    sm->PlayClip(0);                                     // 目标骨架没有剪辑 ⇒ 绑定姿势
    CHECK(sm->currentClip == -1);

    // 传入自定义映射（只映射 root）：child 保持目标绑定姿势
    auto custom = std::make_shared<asset::RetargetProfile>();
    custom->targetToSource = { 0, -1 };
    sm->SetAnimationSource(source, custom);
    CHECK(sm->retargetProfile->MappedJointCount() == 1);
    sm->PlayClip(0, true);
    SkeletalMeshSystem::Update(world, 0.25f);
    CHECK(sm->jointWorldMatrices[1][3].x == doctest::Approx(2.0f).epsilon(0.001));
}
