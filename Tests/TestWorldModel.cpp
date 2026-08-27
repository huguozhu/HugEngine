// ============================================================
// Tests/TestWorldModel.cpp — WorldModel 反射化快照/词表测试
//
// 依赖 SceneReflect.cpp 里注册的 AI 注解属性
// （TransformComponent / DirectionalLight / PointLight）。
// ============================================================

#include "doctest.h"

#include "AI/WorldModel/WorldModel.h"
#include "Scene/World.h"
#include "Scene/SceneGraph.h"
#include "Scene/Transform.h"
#include "Scene/LightComponent.h"

#include "nlohmann/json.hpp"

using namespace he;
using namespace he::ai;

TEST_CASE("WorldModel::Snapshot 导出 AI_VISIBLE 字段") {
    World world;
    SceneGraph sg(world);

    // 一个实体：Transform（position/scale 带 AI_VISIBLE）+ 方向光（color/intensity 带 AI_VISIBLE）
    Entity e = world.CreateEntity("Sun");
    auto* xform = world.AddComponent<TransformComponent>(e);
    xform->position = float3(1.0f, 2.0f, 3.0f);
    xform->scale    = float3(2.0f);
    auto* dl = world.AddComponent<DirectionalLight>(e);
    dl->color     = float3(1.0f, 0.5f, 0.25f);
    dl->intensity = 5.0f;
    sg.SetParent(e, Entity{kInvalidEntity});

    WorldModel wm;
    auto j = nlohmann::json::parse(wm.Snapshot(world, {}));

    REQUIRE(j["entities"].size() == 1);
    auto comps = j["entities"][0]["components"];
    REQUIRE(comps.size() == 2);

    bool foundXform = false, foundLight = false;
    for (auto& c : comps) {
        if (c["type"] == "TransformComponent") {
            foundXform = true;
            REQUIRE(c["fields"].contains("position"));
            CHECK(c["fields"]["position"][0] == 1.0f);
            CHECK(c["fields"]["position"][2] == 3.0f);
            CHECK(c["fields"]["scale"][0] == 2.0f);
        }
        if (c["type"] == "DirectionalLight") {
            foundLight = true;
            REQUIRE(c["fields"].contains("color"));
            CHECK(c["fields"]["color"][1] == 0.5f);
            CHECK(c["fields"]["intensity"] == 5.0f);
        }
    }
    CHECK(foundXform);
    CHECK(foundLight);
}

TEST_CASE("WorldModel::TypeSchema 输出组件词汇表") {
    WorldModel wm;
    auto j = nlohmann::json::parse(wm.TypeSchema());

    REQUIRE(j["component_types"].size() >= 2);
    bool foundTransform = false;
    for (auto& t : j["component_types"]) {
        if (t["type"] == "TransformComponent") {
            foundTransform = true;
            bool hasPosition = false;
            for (auto& f : t["fields"]) {
                if (f["name"] == "position") {
                    hasPosition = true;
                    CHECK(f["writable"] == true);   // HE_ATTR_AI_WRITABLE
                    CHECK(f["description"].get<std::string>().size() > 0);  // HE_ATTR_AI_DESCRIPTION
                }
            }
            CHECK(hasPosition);
        }
    }
    REQUIRE(foundTransform);
}

TEST_CASE("WorldModel::Snapshot 过滤器生效") {
    World world;
    Entity e1 = world.CreateEntity("A");
    world.AddComponent<TransformComponent>(e1);
    Entity e2 = world.CreateEntity("B");
    world.AddComponent<TransformComponent>(e2);

    WorldModel wm;

    // targetEntity 过滤：只导出指定实体
    ObservationFilter f1;
    f1.targetEntity = e2.id;
    auto j1 = nlohmann::json::parse(wm.Snapshot(world, f1));
    REQUIRE(j1["entities"].size() == 1);
    CHECK(j1["entities"][0]["id"] == e2.id);

    // componentTypes 过滤：只导出 DirectionalLight —— 两个实体都没有 → 空
    ObservationFilter f2;
    f2.componentTypes.push_back("DirectionalLight");
    auto j2 = nlohmann::json::parse(wm.Snapshot(world, f2));
    REQUIRE(j2["entities"].empty());
}
