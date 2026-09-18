// ============================================================
// Tests/TestDecal.cpp — Phase A3 DecalComponent 单元测试
//
// 覆盖：四边形几何生成（尺寸/旋转）、默认混合渲染方式、
//       不透明度 → 材质 alpha、SceneBuilder 解析。
//   任务 24 追加：投影贴花参数（projectionDepth）、反射/词表登记、
//       DecalPass 逐贴花 push constant 的布局与大小（与 Slang cbuffer 逐字段对齐）。
// ============================================================

#include "doctest.h"

#include "Scene/World.h"
#include "Scene/Transform.h"
#include "Scene/DecalComponent.h"
#include "AI/SceneBuilder.h"
#include "AI/PromptToScene.h"
#include "Scene/SceneGraph.h"
#include "Pipeline/DecalPass.h"   // 仅用纯数据结构（DecalPushConstants），不需要链接 Render
#include "Reflect/ReflectionAPI.h"   // 反射查询（任务 24：projectionDepth 登记校验）

#include <cstddef>

using namespace he;
using namespace he::ai;

TEST_CASE("DecalComponent::OnCreate 生成单位贴花四边形") {
    World world;
    Entity e = world.CreateEntity("Decal");
    world.AddComponent<TransformComponent>(e);
    auto* d = world.AddComponent<DecalComponent>(e);

    CHECK(d->GetVertexCount() == 4);
    CHECK(d->GetIndexCount() == 6);

    // 默认 1×1：包围盒 ±0.5
    auto b = d->GetBounds();
    CHECK(b.min.x == doctest::Approx(-0.5f));
    CHECK(b.max.x == doctest::Approx(0.5f));

    // 贴花默认渲染方式：无光照 + 半透明混合 + 双面 + 不投影
    CHECK(d->unlit == true);
    CHECK(d->alphaMode == 2);        // AlphaMode::Blend
    CHECK(d->doubleSided == true);
    CHECK(d->castShadow == false);
}

TEST_CASE("DecalComponent 尺寸与旋转参数生效") {
    World world;

    // 尺寸 2×4
    {
        Entity e = world.CreateEntity("D1");
        world.AddComponent<TransformComponent>(e);
        auto* d = world.AddComponent<DecalComponent>(e);
        d->size = float2(2.0f, 4.0f);
        d->OnCreate();   // 参数变化后重建几何
        auto b = d->GetBounds();
        CHECK(b.max.x == doctest::Approx(1.0f));
        CHECK(b.min.x == doctest::Approx(-1.0f));
        CHECK(b.max.y == doctest::Approx(2.0f));
        CHECK(b.min.y == doctest::Approx(-2.0f));
    }
    // 绕法线旋转 90°：宽高互换（2×4 → 包围盒 x 方向 4 宽）
    {
        Entity e = world.CreateEntity("D2");
        world.AddComponent<TransformComponent>(e);
        auto* d = world.AddComponent<DecalComponent>(e);
        d->size     = float2(2.0f, 4.0f);
        d->rotation = 1.5707963f;   // 90°
        d->OnCreate();
        auto b = d->GetBounds();
        CHECK(b.max.x == doctest::Approx(2.0f).epsilon(0.001));   // 半宽 = 4/2
        CHECK(b.max.y == doctest::Approx(1.0f).epsilon(0.001));   // 半高 = 2/2
    }
}

TEST_CASE("DecalComponent 不透明度写入材质 alpha 并钳制") {
    World world;
    Entity e = world.CreateEntity("Decal");
    world.AddComponent<TransformComponent>(e);
    auto* d = world.AddComponent<DecalComponent>(e);

    d->opacity = 0.4f;
    d->OnCreate();
    CHECK(d->baseColorFactor.w == doctest::Approx(0.4f));

    // 越界值钳制到 [0,1]
    d->opacity = 1.7f;
    d->OnCreate();
    CHECK(d->baseColorFactor.w == doctest::Approx(1.0f));
    d->opacity = -0.5f;
    d->OnCreate();
    CHECK(d->baseColorFactor.w == doctest::Approx(0.0f));

    // 纹理路径 → baseColor 槽（textureMask bit0）
    d->decalTexture = "Textures/decal.png";
    d->OnCreate();
    CHECK(d->baseColorTexture.find("decal.png") != String::npos);
}

TEST_CASE("BuildScene 解析 Decal 组件（P2 A3）") {
    World world;
    SceneGraph sg(world);
    String json = R"({"entities":[{"name":"RoadMarking","transform":{"position":[0,0.1,0]},
        "components":[{"type":"Decal","decalTexture":"Textures/arrow.png","size":[2,1],"opacity":0.8}]}]})";
    SceneBuildResult r = BuildScene(world, sg, json);
    REQUIRE(r.success == true);

    auto* d = world.GetComponent<DecalComponent>(r.entities[0]);
    REQUIRE(d != nullptr);
    CHECK(d->decalTexture.find("arrow.png") != String::npos);
    CHECK(d->size.x == doctest::Approx(2.0f));
    CHECK(d->size.y == doctest::Approx(1.0f));
    CHECK(d->baseColorFactor.w == doctest::Approx(0.8f));   // opacity → alpha

    // 尺寸生效于几何（包围盒 x 半宽 = 1）
    CHECK(d->GetBounds().max.x == doctest::Approx(1.0f));
}

TEST_CASE("BuildSceneSystemPrompt 词表包含 Decal") {
    // P2 A3：LLM 词表必须含 Decal（「门口有贴花的仓库」类 prompt 依赖该词条）
    String p = BuildSceneSystemPrompt();
    CHECK(p.find("Decal") != String::npos);
    CHECK(p.find("decalTexture") != String::npos);
    // 任务 24：投影贴花的厚度参数也要在词表里（LLM 需要能为"抬高多一点的表面"调厚度）
    CHECK(p.find("projectionDepth") != String::npos);
}

// ============================================================
// 任务 24：GBuffer 投影贴花
// ============================================================

TEST_CASE("投影贴花：projectionDepth 默认 0.5 米并登记到反射与词表") {
    World world;
    Entity e = world.CreateEntity("Decal");
    world.AddComponent<TransformComponent>(e);
    auto* d = world.AddComponent<DecalComponent>(e);

    // 默认厚度 0.5m：默认贴花盒沿法线 ±0.25m，足以覆盖常见的 0~0.25m 抬高
    CHECK(d->projectionDepth == doctest::Approx(0.5f));
    // 反射：编辑器/AI 可写（四件事里的"注册 + 注解"）
    d->StaticClass();
    const auto* cls = he::reflect::TypeRegistry::Instance().FindClass("he::DecalComponent");
    REQUIRE(cls != nullptr);
    const auto* prop = cls->FindProperty("projectionDepth");
    REQUIRE(prop != nullptr);
    CHECK(prop->offset == offsetof(he::DecalComponent, projectionDepth));
    // GetAttribute 返回 StringView；doctest 对它的 stringify 会误配 char 流，先落成 bool
    const bool aiWritable = (prop->GetAttribute("AiWritable") == "1");
    const bool aiVisible  = (prop->GetAttribute("AiVisible") == "1");
    CHECK(aiWritable);
    CHECK(aiVisible);
}

TEST_CASE("投影贴花：DecalPass push constant 布局与 Slang cbuffer 逐字段一致") {
    using PC = he::render::DecalPushConstants;
    // 大小必须 ≤ 引擎 push constant 上限（Vulkan 常见上限 256 字节）
    static_assert(sizeof(PC) <= he::rhi::kMaxPushConstantSize,
                  "DecalPushConstants 超过 push constant 上限");
    CHECK(sizeof(PC) == 256);

    // 逐字段偏移必须与 DecalProject.vert/frag.slang 的 cbuffer 完全对应（错位会静默画错）。
    // 两边都是"float4 步进 + 末尾 4 个 u32"，此断言就是这条约定的守卫。
    CHECK(offsetof(PC, viewProj)      == 0);
    CHECK(offsetof(PC, rotRow0)       == 64);
    CHECK(offsetof(PC, rotRow1)       == 80);
    CHECK(offsetof(PC, rotRow2)       == 96);
    CHECK(offsetof(PC, invRotRow0)    == 112);
    CHECK(offsetof(PC, invRotRow1)    == 128);
    CHECK(offsetof(PC, invRotRow2)    == 144);
    CHECK(offsetof(PC, decalOrigin)   == 160);
    CHECK(offsetof(PC, halfExtents)   == 176);
    CHECK(offsetof(PC, colorOpacity)  == 192);
    CHECK(offsetof(PC, normalMetal)   == 208);
    CHECK(offsetof(PC, params)        == 224);
    CHECK(offsetof(PC, flags)         == 240);
}
