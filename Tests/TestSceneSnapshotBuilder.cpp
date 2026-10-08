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

#include "Pipeline/GPUScene.h"        // GPUSceneObject + MakeObjectRecord（只用到静态转换，不需要链接 RHI）
#include "Pipeline/PhysicalLight.h"   // kPhysicalLightExposure（与收集口径同源）
#include "Scene/LightComponent.h"
#include "Scene/PhysicalSkyComponent.h"   // 环境（太阳方向/浑浊度）进快照的用例
#include "Scene/SkeletalMeshComponent.h"  // 蒙皮矩阵进快照的用例
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

/// 迁移参考实现：**逐行转写改动前 `DeferredPipeline::CollectLights` 的内联逻辑**
/// （含点光 `directionType.xyz = 0`、聚光方向归一化、色温、物理模式负标记、`MAX_LIGHTS` 截断）。
/// 为什么把旧逻辑抄进测试：迁移类改动的判据就是"新实现与旧行为逐字段一致"，
/// 而全帧转储对比在当前构建下**已经不可判定**（同一二进制两趟就差约 4.5k 像素，ULP 级噪声）。
/// 参考实现是确定性的、可逐位比较的，能把"迁移"与"口径变更"分开验证。
u32 ReferenceCollectLights(World& world, SceneGraph& sg, bool physicalUnits,
                           const std::function<i32(Entity)>& shadowResolver,
                           std::vector<GPULight>& out, bool writeShadowRadius = false) {
    out.clear();
    auto cl = [&](Entity e, LightComponent& lc) {
        u32 i = static_cast<u32>(out.size());
        // 旧代码用的是 `MAX_LIGHTS`（`Pipeline/Material.h` 里的别名，= `kGPUMaxLights`）；
        // 这里直接用共享常量，避免为一个常量拖进整个 Material.h
        if (i >= kGPUMaxLights || !lc.enabled) return;          // 旧代码：先判上限与开关
        GPULight gl{};

        float3 lightColor = lc.color;
        if (lc.colorTemperature > 0.0f) lightColor *= render::KelvinToRGB(lc.colorTemperature);

        gl.colorIntensity = float4(lightColor, lc.intensity);
        gl.shadowIndex    = shadowResolver ? shadowResolver(e) : -1;
        if (writeShadowRadius) gl.shadowRadius = lc.shadowRadius;   // PathTracing 独有

        switch (lc.type) {
        case he::LightType::Directional: {
            auto* dl = static_cast<he::DirectionalLight*>(&lc);
            gl.directionType = float4(dl->direction, 0.0f);
            if (render::IsPhysicalLightEnabled(physicalUnits, lc.illuminance)) {
                gl.colorIntensity.w = lc.illuminance * render::kPhysicalLightExposure;
                gl.positionRange.w  = -1.0f;
            }
            break;
        }
        case he::LightType::Point: {
            auto* pl = static_cast<he::PointLight*>(&lc);
            gl.positionRange   = float4(sg.GetWorldPosition(e), pl->range);
            gl.directionType.w = 1.0f;                          // 旧代码只写 w（xyz 保持 0）
            if (render::IsPhysicalLightEnabled(physicalUnits, lc.luminousIntensity)) {
                gl.colorIntensity.w = lc.luminousIntensity * render::kPhysicalLightExposure;
                gl.positionRange.w  = -(pl->range);
            }
            break;
        }
        case he::LightType::Spot: {
            auto* sl = static_cast<he::SpotLight*>(&lc);
            const float r = render::IsPhysicalLightEnabled(physicalUnits, lc.luminousIntensity)
                          ? -(sl->range) : sl->range;
            gl.positionRange = float4(sg.GetWorldPosition(e), r);
            gl.directionType = float4(glm::normalize(sl->direction), 2.0f);
            gl.coneAngles    = float2(sl->innerConeAngle, sl->outerConeAngle);
            if (render::IsPhysicalLightEnabled(physicalUnits, lc.luminousIntensity)) {
                gl.colorIntensity.w = lc.luminousIntensity * render::kPhysicalLightExposure;
            }
            break;
        }
        default: break;
        }
        out.push_back(gl);
    };
    world.ForEach<he::DirectionalLight>(cl);
    world.ForEach<he::PointLight>(cl);
    world.ForEach<he::SpotLight>(cl);
    return static_cast<u32>(out.size());
}

/// 建一个覆盖全部分支的场景：方向光（色温 + 物理照度）、点光 ×2（含物理光强）、聚光、
/// REC 面光（只有 Forward 收集）、关闭的光源、以及超出上限的补光
void BuildRichLightScene(LightWorld& lw) {
    const Entity de = lw.AddEntity("dir", float3(0.0f));
    auto* dl = lw.world.AddComponent<DirectionalLight>(de);
    dl->direction        = float3(0.2f, -1.0f, 0.1f);
    dl->color            = float3(1.0f, 0.95f, 0.9f);
    dl->intensity        = 3.5f;
    dl->colorTemperature = 5000.0f;
    dl->illuminance      = 90000.0f;          // 走物理模式
    dl->shadowRadius     = 0.37f;             // 只有 PathTracing 会写进 GPULight

    for (int i = 0; i < 2; ++i) {
        const Entity pe = lw.AddEntity("point", float3(1.0f + i, 2.0f, 3.0f));
        auto* pl = lw.world.AddComponent<PointLight>(pe);
        pl->color             = float3(0.8f, 0.7f, 0.6f);
        pl->intensity         = 5.0f + static_cast<float>(i);
        pl->range             = 10.0f + static_cast<float>(i);
        pl->shadowRadius      = 0.11f + static_cast<float>(i);
        pl->luminousIntensity = (i == 1) ? 400.0f : 0.0f;   // 只让第二个走物理模式
    }

    const Entity se = lw.AddEntity("spot", float3(-2.0f, 5.0f, 1.0f));
    auto* sl = lw.world.AddComponent<SpotLight>(se);
    sl->direction      = float3(0.0f, -2.0f, 0.5f);   // 未归一化
    sl->range          = 7.0f;
    sl->innerConeAngle = 0.2f;
    sl->outerConeAngle = 0.45f;

    const Entity re = lw.AddEntity("rect", float3(3.0f, 4.0f, 5.0f));
    auto* rl = lw.world.AddComponent<RectLight>(re);
    rl->normal            = float3(0.0f, -1.0f, 0.0f);
    rl->width             = 2.5f;
    rl->height            = 1.25f;
    rl->range             = 6.0f;
    rl->intensity         = 2.0f;
    rl->luminousIntensity = 0.0f;

    const Entity offE = lw.AddEntity("off", float3(0.0f));
    lw.world.AddComponent<PointLight>(offE)->enabled = false;

    for (u32 k = 0; k < kGPUMaxLights + 2u; ++k) {    // 触发截断
        const Entity e = lw.AddEntity("fill", float3(static_cast<float>(k), 0.0f, 0.0f));
        lw.world.AddComponent<PointLight>(e)->range = 3.0f;
    }
}

/// 迁移参考实现（二）：**逐行转写 `ForwardPipeline::CollectLights`**。
/// 与 Deferred 版的差异就是头文件登记的三处：收集 `RectLight`、点光写 (0,-1,0)、聚光不归一化。
u32 ReferenceCollectLightsForward(World& world, SceneGraph& sg, bool physicalUnits,
                                  const std::function<i32(Entity)>& shadowResolver,
                                  std::vector<GPULight>& out) {
    out.clear();
    auto cl = [&](Entity e, LightComponent& lc) {
        if (!lc.enabled) return;                                // Forward 先判开关，再判上限
        u32 i = static_cast<u32>(out.size());
        if (i >= kGPUMaxLights) return;

        float3 lightColor = lc.color;
        if (lc.colorTemperature > 0.0f) lightColor *= render::KelvinToRGB(lc.colorTemperature);

        GPULight gl{};
        gl.colorIntensity = float4(lightColor, lc.intensity);
        gl.shadowIndex    = shadowResolver ? shadowResolver(e) : -1;

        switch (lc.type) {
        case he::LightType::Directional: {
            auto* dl = static_cast<he::DirectionalLight*>(&lc);
            gl.directionType = float4(dl->direction, 0.0f);
            gl.positionRange = float4(0.0f, 0.0f, 0.0f, 0.0f);
            if (render::IsPhysicalLightEnabled(physicalUnits, lc.illuminance)) {
                gl.colorIntensity.w = lc.illuminance * render::kPhysicalLightExposure;
                gl.positionRange.w  = -1.0f;
            }
            break;
        }
        case he::LightType::Point: {
            auto* pl = static_cast<he::PointLight*>(&lc);
            gl.positionRange = float4(sg.GetWorldPosition(e), pl->range);
            gl.directionType = float4(0.0f, -1.0f, 0.0f, 1.0f);
            if (render::IsPhysicalLightEnabled(physicalUnits, lc.luminousIntensity)) {
                gl.colorIntensity.w = lc.luminousIntensity * render::kPhysicalLightExposure;
                gl.positionRange.w  = -(pl->range);
            }
            break;
        }
        case he::LightType::Spot: {
            auto* sl = static_cast<he::SpotLight*>(&lc);
            const float r = render::IsPhysicalLightEnabled(physicalUnits, lc.luminousIntensity)
                          ? -(sl->range) : sl->range;
            gl.positionRange = float4(sg.GetWorldPosition(e), r);
            gl.directionType = float4(sl->direction, 2.0f);     // 不归一化
            gl.coneAngles    = float2(sl->innerConeAngle, sl->outerConeAngle);
            if (render::IsPhysicalLightEnabled(physicalUnits, lc.luminousIntensity)) {
                gl.colorIntensity.w = lc.luminousIntensity * render::kPhysicalLightExposure;
            }
            break;
        }
        case he::LightType::Rect: {
            auto* rl = static_cast<he::RectLight*>(&lc);
            gl.positionRange = float4(sg.GetWorldPosition(e), rl->range);
            gl.directionType = float4(rl->normal, 3.0f);
            gl.coneAngles    = float2(rl->width, rl->height);
            if (render::IsPhysicalLightEnabled(physicalUnits, rl->luminousIntensity)) {
                gl.colorIntensity.w = rl->luminousIntensity * render::kPhysicalLightExposure;
            }
            break;
        }
        default: break;
        }
        out.push_back(gl);
    };
    world.ForEach<he::DirectionalLight>(cl);
    world.ForEach<he::PointLight>(cl);
    world.ForEach<he::SpotLight>(cl);
    world.ForEach<he::RectLight>(cl);
    return static_cast<u32>(out.size());
}

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

TEST_CASE("SceneSnapshotBuilder：口径开关（点光方向 / 聚光归一化）可显式切换") {
    LightWorld lw;
    const Entity pe = lw.AddEntity("point", float3(0.0f, 0.0f, 0.0f));
    const Entity se = lw.AddEntity("spot", float3(0.0f, 0.0f, 0.0f));
    lw.world.AddComponent<PointLight>(pe);
    auto* sl = lw.world.AddComponent<SpotLight>(se);
    sl->direction = float3(0.0f, 0.0f, 5.0f);          // 未归一化

    SUBCASE("默认 = Deferred 口径：点光 xyz 留 0、聚光归一化") {
        FrameSceneSnapshot snap;
        REQUIRE(SceneSnapshotBuilder::BuildLights(lw.world, lw.sg, {}, snap) == 2u);
        CHECK(snap.lights[0].directionType.x == 0.0f);       // 点光 xyz 留 0
        CHECK(snap.lights[0].directionType.y == 0.0f);
        CHECK(snap.lights[1].directionType.z == doctest::Approx(1.0f));   // 归一化
    }

    SUBCASE("Forward 历史口径：点光写 (0,-1,0)、聚光不归一化") {
        SceneSnapshotLightOptions options;
        options.pointLightWritesDirection = true;
        options.normalizeSpotDirection    = false;

        FrameSceneSnapshot snap;
        REQUIRE(SceneSnapshotBuilder::BuildLights(lw.world, lw.sg, {}, snap, options) == 2u);
        CHECK(snap.lights[0].directionType.y == -1.0f);
        CHECK(snap.lights[1].directionType.z == doctest::Approx(5.0f));   // 保持原值
    }
}

TEST_CASE("SceneSnapshotBuilder：与改动前 CollectLights 逐字段一致（迁移钉子）") {
    LightWorld lw;
    BuildRichLightScene(lw);

    // 阴影索引解析器：给每个实体一个可区分且确定的值
    auto resolver = [](he::Entity e) -> i32 { return static_cast<i32>(e.id % 4u) - 1; };
    // PathTracing：没有传统阴影系统（恒 -1），但要写 shadowRadius
    auto noShadow = [](he::Entity) -> i32 { return -1; };

    struct Case {
        const char*             name;
        SceneSnapshotLightOptions options;      // 传给收集器的口径开关
        bool                    forwardRef;     // 用哪份转写参考实现
        bool                    useNoShadow;    // 用恒 -1 的阴影解析器（PT）
        bool                    writeRadius;    // 参考实现是否写 shadowRadius（PT）
    };
    const Case cases[] = {
        // Deferred：默认口径（不收集 Rect、点光 xyz=0、聚光归一化、不写 shadowRadius）
        {"Deferred", SceneSnapshotLightOptions{}, false, false, false},
        // Forward：收集 Rect、点光写 (0,-1,0)、聚光不归一化（shadowRadius 仍不写）
        {"Forward", SceneSnapshotLightOptions{true, true, false, false}, true, false, false},
        // PathTracing：口径同 Deferred，但 shadowIndex 恒 -1、且写 shadowRadius
        {"PathTracing", SceneSnapshotLightOptions{false, false, true, true}, false, true, true},
    };

    for (const Case& c : cases) {
        for (int physical = 0; physical < 2; ++physical) {
            SceneSnapshotResolvers resolvers;
            resolvers.physicalUnitsEnabled = (physical != 0);
            resolvers.shadowIndex          = c.useNoShadow ? noShadow : resolver;

            FrameSceneSnapshot snap;
            const u32 newCount = SceneSnapshotBuilder::BuildLights(lw.world, lw.sg, resolvers, snap,
                                                                  c.options);

            std::vector<GPULight> reference;
            const u32 refCount = c.forwardRef
                ? ReferenceCollectLightsForward(lw.world, lw.sg, resolvers.physicalUnitsEnabled,
                                                resolvers.shadowIndex, reference)
                : ReferenceCollectLights(lw.world, lw.sg, resolvers.physicalUnitsEnabled,
                                         resolvers.shadowIndex, reference, c.writeRadius);

            INFO("口径=", c.name, " 物理开关=", physical);
            REQUIRE(newCount == refCount);
            REQUIRE(snap.lights.size() == reference.size());
            REQUIRE(!reference.empty());

            // 逐元素整块逐位比较（GPULight 与 SnapshotLight 布局一致已由头文件 static_assert 保证）
            for (usize i = 0; i < reference.size(); ++i) {
                const GPULight gpu = snap.lights[i].ToGpu();
                CHECK(std::memcmp(&gpu, &reference[i], sizeof(GPULight)) == 0);
            }
        }
    }
}

TEST_CASE("SceneSnapshotBuilder：物体映射（与 GPUScene::Collect 的 FillObj 逐字段对齐）") {
    // 假组件：只提供映射需要的三个成员（duck typing）
    struct FakeComponent {
        u32        indexCount = 0;
        u32        materialID = 0;
        he::AABB   bounds;
        u32 GetIndexCount() const { return indexCount; }
        he::AABB GetBounds() const { return bounds; }
    };

    FrameSceneSnapshot snap;
    FakeComponent comp;
    comp.indexCount = 36;
    comp.materialID = 12;
    comp.bounds     = he::AABB{float3(-1.0f, -2.0f, -3.0f), float3(1.0f, 2.0f, 3.0f)};

    he::Entity e{1};
    he::SceneGraph* dummy = nullptr;   // 映射本身不需要 SceneGraph（矩阵由调用方算好）
    (void)dummy;

    const float4x4 wm = glm::translate(float4x4(1.0f), float3(10.0f, 0.0f, 0.0f));
    SceneSnapshotBuilder::CollectObjectItem(e, comp, wm, 5u, nullptr, snap);

    REQUIRE(snap.draws.size() == 1u);
    const SnapshotDrawItem& item = snap.draws[0];
    CHECK(item.object.worldMatrix[3].x == doctest::Approx(10.0f));   // 平移进入世界矩阵
    CHECK(item.object.boundsMin.x == doctest::Approx(9.0f));         // 包围盒已变换到世界空间
    CHECK(item.object.boundsMax.x == doctest::Approx(11.0f));
    CHECK(item.object.boundsMin.y == doctest::Approx(-2.0f));
    CHECK(item.materialIndex == 12u);
    CHECK(item.object.materialID == 12u);
    CHECK(item.objectID == 5u);                    // 收集序号由调用方给（= 下标）
    CHECK(item.visibilityFlags == 1u);             // 与 FillObj 一致
    CHECK(item.prevWorldMatrix[3].x == doctest::Approx(10.0f));   // 无上一帧 ⇒ 取当前矩阵
    CHECK(item.indexCount == 0u);                  // 由 MeshBatcher 填充，收集阶段恒 0
}

TEST_CASE("SceneSnapshotBuilder：物体映射的跳过与上一帧矩阵") {
    struct FakeComponent {
        u32      indexCount = 0;
        u32      materialID = 0;
        he::AABB bounds;
        u32 GetIndexCount() const { return indexCount; }
        he::AABB GetBounds() const { return bounds; }
    };

    he::Entity e{1};
    const float4x4 wm = float4x4(1.0f);

    SUBCASE("无索引的组件不进列表") {
        FrameSceneSnapshot snap;
        FakeComponent comp;                        // indexCount = 0
        SceneSnapshotBuilder::CollectObjectItem(e, comp, wm, 0u, nullptr, snap);
        CHECK(snap.draws.empty());
    }

    SUBCASE("提供上一帧快照时按下标取 prevWorldMatrix") {
        FrameSceneSnapshot prev;
        SnapshotDrawItem prevItem;
        prevItem.object.worldMatrix = glm::translate(float4x4(1.0f), float3(-4.0f, 0.0f, 0.0f));
        prev.draws.push_back(prevItem);

        FrameSceneSnapshot snap;
        FakeComponent comp;
        comp.indexCount = 3;
        SceneSnapshotBuilder::CollectObjectItem(e, comp, wm, 0u, &prev, snap);
        REQUIRE(snap.draws.size() == 1u);
        CHECK(snap.draws[0].prevWorldMatrix[3].x == doctest::Approx(-4.0f));   // 来自上一帧
        CHECK(snap.draws[0].object.worldMatrix[3].x == doctest::Approx(0.0f));
    }

    SUBCASE("上一帧下标不存在时退回当前矩阵（首帧语义）") {
        FrameSceneSnapshot prev;                   // 空
        FrameSceneSnapshot snap;
        FakeComponent comp;
        comp.indexCount = 3;
        SceneSnapshotBuilder::CollectObjectItem(e, comp, wm, 7u, &prev, snap);   // 下标越界
        REQUIRE(snap.draws.size() == 1u);
        CHECK(snap.draws[0].prevWorldMatrix[3].x == doctest::Approx(0.0f));
    }
}

TEST_CASE("SceneSnapshotBuilder：空世界的物体收集为 0（遍历入口）") {
    LightWorld lw;
    FrameSceneSnapshot snap;
    const u32 n = SceneSnapshotBuilder::BuildObjects(lw.world, lw.sg, CameraData{}, {}, nullptr, snap);
    CHECK(n == 0u);
    CHECK(snap.draws.empty());
}

TEST_CASE("GPUScene::MakeObjectRecord：与旧 FillObj 逐位一致（迁移钉子）") {
    // 旧 `FillObj(o, wm, b, idx)` 的逐行转写（`GPUScene.cpp` 里那份已被快照取代）
    auto fillObjReference = [](GPUSceneObject& o, const float4x4& wm, const he::AABB& b, u32 idx) {
        o.localToWorld = wm;
        o.boundsMin = float4(b.min, 0);
        o.boundsMax = float4(b.max, 0);
        o.objectID = idx;
        o.visibilityFlags = 1;
        o.meshIndex = 0;
        o.indexCount = 0;
        o.firstIndex = 0;
        o.vertexOffset = 0;
    };

    SnapshotDrawItem item;
    item.object.worldMatrix = glm::translate(float4x4(1.0f), float3(3.0f, -2.0f, 7.0f));
    item.object.boundsMin   = float4(-1.0f, -2.0f, -3.0f, 0.0f);
    item.object.boundsMax   = float4(1.0f, 2.0f, 3.0f, 0.0f);
    item.objectID           = 4u;
    item.visibilityFlags    = 1u;
    item.materialIndex      = 9u;
    item.meshIndex          = 0u;    // 收集阶段恒 0（由 MeshBatcher 后填）⇒ 可与旧路径逐位对比

    const GPUSceneObject fromSnapshot = GPUScene::MakeObjectRecord(item);

    GPUSceneObject reference{};
    const he::AABB worldBounds{float3(-1.0f, -2.0f, -3.0f), float3(1.0f, 2.0f, 3.0f)};
    fillObjReference(reference, item.object.worldMatrix, worldBounds, item.objectID);
    reference.materialIndex = item.materialIndex;    // 旧路径在 FillObj 之后单独设它

    CHECK(std::memcmp(&fromSnapshot, &reference, sizeof(GPUSceneObject)) == 0);

    // MeshBatcher 后填的三个字段必须能透传（这是快照相对旧路径新增的能力）
    SnapshotDrawItem batched = item;
    batched.meshIndex    = 3u;
    batched.indexCount   = 36u;
    batched.firstIndex   = 72u;
    batched.vertexOffset = 12;
    const GPUSceneObject fromBatched = GPUScene::MakeObjectRecord(batched);
    CHECK(fromBatched.meshIndex == 3u);
    CHECK(fromBatched.indexCount == 36u);
    CHECK(fromBatched.firstIndex == 72u);
    CHECK(fromBatched.vertexOffset == 12);
}

TEST_CASE("GPUSceneObject：布局必须与着色器 std430 一致（128 字节）") {
    CHECK(sizeof(GPUSceneObject) == 128u);
}

TEST_CASE("SceneSnapshotBuilder：环境（太阳方向 + 浑浊度）进快照") {
    LightWorld lw;
    FrameSceneSnapshot snap;

    SUBCASE("没有物理天空 ⇒ 复位为关闭（方向 (0,1,0)、浑浊度 0）") {
        CHECK_FALSE(SceneSnapshotBuilder::BuildEnvironment(lw.world, snap));
        CHECK(snap.atmosphere.y == 1.0f);
        CHECK(snap.atmosphere.w == 0.0f);          // 浑浊度 0 = 关闭
    }

    SUBCASE("有启用的物理天空 ⇒ 取它的方向与浑浊度") {
        const Entity e = lw.AddEntity("sky", float3(0.0f));
        auto* ps = lw.world.AddComponent<he::PhysicalSkyComponent>(e);
        ps->enabled      = true;
        ps->sunDirection = float3(0.3f, -0.9f, 0.2f);
        ps->turbidity    = 4.5f;

        CHECK(SceneSnapshotBuilder::BuildEnvironment(lw.world, snap));
        CHECK(snap.atmosphere.x == doctest::Approx(0.3f));
        CHECK(snap.atmosphere.y == doctest::Approx(-0.9f));
        CHECK(snap.atmosphere.w == doctest::Approx(4.5f));
    }

    SUBCASE("物理天空存在但未启用 ⇒ 仍视为关闭") {
        const Entity e = lw.AddEntity("sky", float3(0.0f));
        auto* ps = lw.world.AddComponent<he::PhysicalSkyComponent>(e);
        ps->enabled   = false;
        ps->turbidity = 9.0f;

        CHECK_FALSE(SceneSnapshotBuilder::BuildEnvironment(lw.world, snap));
        CHECK(snap.atmosphere.w == 0.0f);
    }
}

TEST_CASE("SceneSnapshotBuilder：骨骼矩阵进快照的扁平数组（T1.4）") {
    FrameSceneSnapshot snap;
    he::SkeletalMeshComponent first;
    he::SkeletalMeshComponent second;

    first.boneMatrices  = {float4x4(1.0f), float4x4(2.0f)};
    second.boneMatrices = {float4x4(3.0f), float4x4(4.0f), float4x4(5.0f)};

    SnapshotDrawItem a{};
    SnapshotDrawItem b{};
    SceneSnapshotBuilder::AppendSkinMatrices(a, first, snap);
    SceneSnapshotBuilder::AppendSkinMatrices(b, second, snap);

    CHECK(a.skinMatrixOffset == 0u);
    CHECK(a.skinMatrixCount == 2u);
    CHECK(b.skinMatrixOffset == 2u);                 // 第二段紧接第一段（不覆盖）
    CHECK(b.skinMatrixCount == 3u);
    REQUIRE(snap.skinMatrices.size() == 5u);
    CHECK(snap.skinMatrices[0][0][0] == 1.0f);
    CHECK(snap.skinMatrices[2][0][0] == 3.0f);       // b 的第一段
    CHECK(snap.skinMatrices[4][0][0] == 5.0f);

    // 无骨骼的网格：切片为空，且不污染共享数组
    FrameSceneSnapshot snap2;
    he::SkeletalMeshComponent empty;
    SnapshotDrawItem c{};
    SceneSnapshotBuilder::AppendSkinMatrices(c, empty, snap2);
    CHECK(c.skinMatrixCount == 0u);
    CHECK(snap2.skinMatrices.empty());
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
