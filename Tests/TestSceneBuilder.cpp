// ============================================================
// Tests/TestSceneBuilder.cpp — SceneBuilder 单元测试
//
// 覆盖：有效场景 JSON / 非法 JSON / 未知组件类型容错。
// 注意：无 RHI 设备时 MeshComponent::SetMeshData 会打印
// "RHI device not available" 日志，属预期噪声，不影响断言。
// ============================================================

#include "doctest.h"

#include "AI/SceneBuilder.h"
#include "Scene/World.h"
#include "Scene/SceneGraph.h"
#include "Scene/Transform.h"
#include "Scene/CubeComponent.h"
#include "Scene/SphereComponent.h"
#include "Scene/LightComponent.h"

using namespace he;
using namespace he::ai;

TEST_CASE("BuildScene 解析有效场景 JSON") {
    World world;
    SceneGraph sg(world);

    String json = R"({
      "entities": [
        {"name":"Ground","transform":{"position":[0,-1,0],"scale":[10,0.2,10]},
         "components":[{"type":"Cube","halfExtent":0.5,"baseColor":[0.3,0.3,0.35],"roughness":0.9}]},
        {"name":"Sun","transform":{"position":[0,10,0]},
         "components":[{"type":"DirectionalLight","direction":[0.5,-1,0.5],"color":[1,0.95,0.85],"intensity":5.0}]}
      ]
    })";

    SceneBuildResult r = BuildScene(world, sg, json);

    REQUIRE(r.success == true);
    REQUIRE(r.entities.size() == 2);
    REQUIRE(world.GetEntityCount() == 2);

    auto* xform = world.GetComponent<TransformComponent>(r.entities[0]);
    REQUIRE(xform != nullptr);
    CHECK(xform->position.y == doctest::Approx(-1.0f));
    CHECK(xform->scale.x == doctest::Approx(10.0f));

    auto* cube = world.GetComponent<CubeComponent>(r.entities[0]);
    REQUIRE(cube != nullptr);
    CHECK(cube->halfExtent == doctest::Approx(0.5f));
    CHECK(cube->roughnessFactor == doctest::Approx(0.9f));

    auto* dl = world.GetComponent<DirectionalLight>(r.entities[1]);
    REQUIRE(dl != nullptr);
    CHECK(dl->intensity == doctest::Approx(5.0f));
    CHECK(dl->color.x == doctest::Approx(1.0f));
}

TEST_CASE("BuildScene 对非法 JSON 返回失败") {
    World world;
    SceneGraph sg(world);
    SceneBuildResult r = BuildScene(world, sg, "{ 不是合法 JSON ");
    REQUIRE(r.success == false);
    CHECK(!r.error.empty());
}

TEST_CASE("BuildScene 跳过未知组件类型且不崩溃") {
    World world;
    SceneGraph sg(world);
    String json = R"({"entities":[{"name":"X","components":[{"type":"Nope"}]}]})";
    SceneBuildResult r = BuildScene(world, sg, json);
    REQUIRE(r.success == true);       // 实体仍被创建
    REQUIRE(r.entities.size() == 1);
    REQUIRE(world.GetComponent<TransformComponent>(r.entities[0]) != nullptr);
}
