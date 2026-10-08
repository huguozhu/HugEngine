// ============================================================
// TestSceneSnapshotBuilder.cpp — 阶段 1 T1.2a：光源收集（快照口径）
//
// 【为什么要有它】收集口径的错误几乎全是**静默**的：少收一个光源、阴影索引填错、物理模式的
// 负范围标记丢失、色温没叠加 —— 画面上只表现为"某个光源不对"，很难回溯到收集代码。
// 这里把 `CollectLights` 的每条分支都钉一遍（方向光/点光/聚光、关闭、上限截断、物理模式、
// 阴影索引解析器）。
//
// 本测试直接编译 `Engine/Render/Threading/SceneSnapshotBuilder.cpp`（它 RHI-free，只依赖 Scene 与
// PhysicalLight 的纯函数），不链接 HugEngineRender —— 这条约束是钉子：收集逻辑一旦引入 RHI 依赖，
// 单测目标立刻编译失败。
// ============================================================
#include "Threading/SceneSnapshotBuilder.h"

#include "Pipeline/PhysicalLight.h"   // kPhysicalLightExposure（与收集口径同源）
#include "Scene/LightComponent.h"
#include "Scene/SceneGraph.h"
#include "Scene/Transform.h"
#include "Scene/World.h"

#include <doctest/doctest.h>

using namespace he;
using namespace he::render;

namespace {

/// 建一个带 SceneGraph 的世界（SceneGraph 持有 World 引用，故两者必须同生命周期）
struct LightWorld {
    World      world;
    SceneGraph sg{world};

    LightWorld() { world.SetSceneGraph(&sg); }

    Entity AddEntity(const char* name, const float3& pos) {
        Entity e = world.CreateEntity(name);
        auto*  t = world.AddComponent<TransformComponent>(e);
        t->position = pos;
        return e;
    }
};

} // namespace

TEST_CASE("SceneSnapshotBuilder：空世界不产出光源") {
    LightWorld lw;
    FrameSceneSnapshot snap;
    const u32 n = SceneSnapshotBuilder::BuildLights(lw.world, lw.sg, {}, snap);
    CHECK(n == 0u);
    CHECK(snap.lights.empty());
    CHECK(snap.IsEmpty());
}

TEST_CASE("SceneSnapshotBuilder：关闭的光源被跳过，且不影响后续光源的下标") {
    LightWorld lw;
    const Entity off = lw.AddEntity("off", float3(0.0f));
    const Entity on  = lw.AddEntity("on", float3(1.0f, 2.0f, 3.0f));
    lw.world.AddComponent<DirectionalLight>(off)->enabled = false;
    auto* dl = lw.world.AddComponent<DirectionalLight>(on);
    dl->enabled     = true;
    dl->direction   = float3(0.0f, -1.0f, 0.0f);
    dl->intensity   = 4.0f;
    dl->color       = float3(1.0f, 0.5f, 0.25f);
    dl->colorTemperature = 0.0f;                 // 不启用色温
    dl->illuminance = 0.0f;                      // 不启用物理模式

    FrameSceneSnapshot snap;
    CHECK(SceneSnapshotBuilder::BuildLights(lw.world, lw.sg, {}, snap) == 1u);
    REQUIRE(snap.lights.size() == 1u);
    CHECK(snap.lights[0].colorIntensity.w == 4.0f);
    CHECK(snap.lights[0].colorIntensity.x == 1.0f);
    CHECK(snap.lights[0].directionType.y == -1.0f);
    CHECK(snap.lights[0].directionType.w == 0.0f);   // 0 = 方向光
    CHECK(snap.lights[0].shadowIndex == -1);         // 未提供解析器 ⇒ -1
}

TEST_CASE("SceneSnapshotBuilder：方向光用世界方向、聚光归一化并带锥角、点光带世界位置") {
    LightWorld lw;
    const Entity de = lw.AddEntity("dir", float3(0.0f));
    const Entity pe = lw.AddEntity("point", float3(5.0f, 6.0f, 7.0f));
    const Entity se = lw.AddEntity("spot", float3(-1.0f, -2.0f, -3.0f));

    auto* dl = lw.world.AddComponent<DirectionalLight>(de);
    dl->direction = float3(0.3f, -1.0f, 0.2f);

    auto* pl = lw.world.AddComponent<PointLight>(pe);
    pl->range = 12.0f;

    auto* sl = lw.world.AddComponent<SpotLight>(se);
    sl->direction      = float3(0.0f, 0.0f, 4.0f);   // 未归一化 ⇒ 收集时必须归一化
    sl->range          = 9.0f;
    sl->innerConeAngle = 0.25f;
    sl->outerConeAngle = 0.5f;

    FrameSceneSnapshot snap;
    CHECK(SceneSnapshotBuilder::BuildLights(lw.world, lw.sg, {}, snap) == 3u);
    REQUIRE(snap.lights.size() == 3u);

    // 顺序：方向光 → 点光 → 聚光（与旧路径一致）
    CHECK(snap.lights[0].directionType.w == 0.0f);
    CHECK(snap.lights[1].directionType.w == 1.0f);
    CHECK(snap.lights[2].directionType.w == 2.0f);

    CHECK(snap.lights[1].positionRange.x == 5.0f);       // 点光世界位置来自 SceneGraph
    CHECK(snap.lights[1].positionRange.y == 6.0f);
    CHECK(snap.lights[1].positionRange.w == 12.0f);      // 范围（非物理模式为正）

    CHECK(snap.lights[2].directionType.z == doctest::Approx(1.0f));   // normalize((0,0,4))
    CHECK(snap.lights[2].directionType.x == doctest::Approx(0.0f));
    CHECK(snap.lights[2].coneAngles.x == 0.25f);
    CHECK(snap.lights[2].coneAngles.y == 0.5f);
    CHECK(snap.lights[2].positionRange.x == -1.0f);
}

TEST_CASE("SceneSnapshotBuilder：物理模式用负范围/负标记，且只在开关打开时生效") {
    LightWorld lw;
    const Entity de = lw.AddEntity("dir", float3(0.0f));
    const Entity pe = lw.AddEntity("point", float3(1.0f, 1.0f, 1.0f));
    auto* dl = lw.world.AddComponent<DirectionalLight>(de);
    dl->illuminance = 120000.0f;                  // 真实日照量级（lux）
    dl->intensity   = 7.0f;
    auto* pl = lw.world.AddComponent<PointLight>(pe);
    pl->luminousIntensity = 800.0f;               // 坎德拉
    pl->range             = 15.0f;

    SceneSnapshotResolvers resolvers;

    SUBCASE("全局开关关闭 ⇒ 维持传统强度模式") {
        resolvers.physicalUnitsEnabled = false;
        FrameSceneSnapshot snap;
        CHECK(SceneSnapshotBuilder::BuildLights(lw.world, lw.sg, resolvers, snap) == 2u);
        CHECK(snap.lights[0].colorIntensity.w == 7.0f);        // 用 intensity
        CHECK(snap.lights[0].positionRange.w == 0.0f);         // 无负标记
        CHECK(snap.lights[1].positionRange.w == 15.0f);        // 范围保持正
    }

    SUBCASE("全局开关打开 ⇒ 换成物理量并打负标记") {
        resolvers.physicalUnitsEnabled = true;
        FrameSceneSnapshot snap;
        CHECK(SceneSnapshotBuilder::BuildLights(lw.world, lw.sg, resolvers, snap) == 2u);
        CHECK(snap.lights[0].colorIntensity.w == doctest::Approx(120000.0f * kPhysicalLightExposure));
        CHECK(snap.lights[0].positionRange.w == -1.0f);        // 方向光的物理标记固定为 -1
        CHECK(snap.lights[1].colorIntensity.w == doctest::Approx(800.0f * kPhysicalLightExposure));
        CHECK(snap.lights[1].positionRange.w == -15.0f);       // 点光：负范围即标记
    }
}

TEST_CASE("SceneSnapshotBuilder：色温叠加到颜色上（并锁住当前近似函数的输出）") {
    LightWorld lw;
    const Entity de = lw.AddEntity("dir", float3(0.0f));
    auto* dl = lw.world.AddComponent<DirectionalLight>(de);
    dl->color            = float3(1.0f, 1.0f, 1.0f);
    dl->colorTemperature = 6500.0f;

    FrameSceneSnapshot snap;
    REQUIRE(SceneSnapshotBuilder::BuildLights(lw.world, lw.sg, {}, snap) == 1u);

    // 收集必须**乘上**色温颜色（否则等于没启用色温）。这里用当前近似函数的实际输出做钉子，
    // 与 `PhysicalLight.h` 的实现相互印证；该函数本身的可疑之处（6500K 得到偏橙的 (1, 0.46, 0)）
    // 不在本任务范围内，已作为画质问题单独登记。
    CHECK(snap.lights[0].colorIntensity.x == doctest::Approx(1.0f));
    CHECK(snap.lights[0].colorIntensity.y == doctest::Approx(0.460435f).epsilon(0.001));
    CHECK(snap.lights[0].colorIntensity.z == doctest::Approx(0.0f));
}

TEST_CASE("SceneSnapshotBuilder：阴影索引来自注入的解析器") {
    LightWorld lw;
    const Entity e = lw.AddEntity("dir", float3(0.0f));
    lw.world.AddComponent<DirectionalLight>(e);

    SceneSnapshotResolvers resolvers;
    resolvers.shadowIndex = [](he::Entity entity) -> i32 {
        return entity.id == 1u ? 2 : -1;          // 只给第一个实体一个阴影槽
    };

    FrameSceneSnapshot snap;
    REQUIRE(SceneSnapshotBuilder::BuildLights(lw.world, lw.sg, resolvers, snap) == 1u);
    CHECK(snap.lights[0].shadowIndex == 2);
}

TEST_CASE("SceneSnapshotBuilder：超过上限时按 kGPUMaxLights 截断") {
    LightWorld lw;
    for (u32 i = 0; i < kGPUMaxLights + 3u; ++i) {
        const Entity e = lw.AddEntity("p", float3(static_cast<float>(i), 0.0f, 0.0f));
        auto* pl = lw.world.AddComponent<PointLight>(e);
        pl->range = 1.0f + static_cast<float>(i);
    }

    FrameSceneSnapshot snap;
    const u32 n = SceneSnapshotBuilder::BuildLights(lw.world, lw.sg, {}, snap);
    CHECK(n == kGPUMaxLights);
    CHECK(snap.lights.size() == kGPUMaxLights);
    CHECK(snap.lights.back().positionRange.w == static_cast<float>(kGPUMaxLights));   // 取前 N 个
}

TEST_CASE("SceneSnapshotBuilder：逐帧复用不残留上一帧光源") {
    LightWorld lw;
    const Entity e = lw.AddEntity("p", float3(0.0f, 0.0f, 0.0f));
    lw.world.AddComponent<PointLight>(e);

    FrameSceneSnapshot snap;
    CHECK(SceneSnapshotBuilder::BuildLights(lw.world, lw.sg, {}, snap) == 1u);
    // 同一份快照再次收集：必须清空后重填，而不是累加
    CHECK(SceneSnapshotBuilder::BuildLights(lw.world, lw.sg, {}, snap) == 1u);
    CHECK(snap.lights.size() == 1u);
}
