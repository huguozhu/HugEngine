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
#include "Scene/CameraComponent.h"

#include "nlohmann/json.hpp"

#include <set>

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

TEST_CASE("WorldModel::TypeSchema 包含 SpotLight/RectLight/CameraComponent（S0.1/S0.2）") {
    // TypeRegistry 为惰性注册（StaticClass() 首次调用才注册），先触发这三个类的注册
    he::SpotLight::StaticClass();
    he::RectLight::StaticClass();
    he::CameraComponent::StaticClass();

    WorldModel wm;
    auto j = nlohmann::json::parse(wm.TypeSchema());

    // 收集所有反射类型名（S0 注册后应有 3 个光源类型 + 相机）
    std::set<String> types;
    for (auto& t : j["component_types"]) types.insert(t["type"].get<String>());
    REQUIRE(types.count("SpotLight") == 1);
    REQUIRE(types.count("RectLight") == 1);
    REQUIRE(types.count("CameraComponent") == 1);

    // 逐类型校验字段清单（含可写性 + 中文说明）
    for (auto& t : j["component_types"]) {
        if (t["type"] != "SpotLight") continue;
        std::set<String> fields;
        for (auto& f : t["fields"]) {
            fields.insert(f["name"].get<String>());
            if (f["name"] == "direction" || f["name"] == "color" ||
                f["name"] == "intensity" || f["name"] == "range" ||
                f["name"] == "innerConeAngle" || f["name"] == "outerConeAngle" ||
                f["name"] == "castShadow") {
                CHECK(f["writable"] == true);       // 数值类全部 AI 可写
                CHECK(f["description"].get<String>().size() > 0);
            }
        }
        for (auto n : {"direction", "color", "intensity", "range",
                       "innerConeAngle", "outerConeAngle", "castShadow"})
            CHECK(fields.count(n) == 1);
    }
    for (auto& t : j["component_types"]) {
        if (t["type"] != "CameraComponent") continue;
        bool hasMain = false;
        for (auto& f : t["fields"]) {
            if (f["name"] == "fov" || f["name"] == "nearPlane" || f["name"] == "farPlane") {
                CHECK(f["writable"] == true);
                CHECK(f["description"].get<String>().size() > 0);
            }
            if (f["name"] == "isMain") {
                hasMain = true;
                CHECK(f["writable"] == false);   // isMain 仅 AI_VISIBLE，AI 不可写
            }
        }
        CHECK(hasMain);
    }
}

TEST_CASE("WorldModel::Snapshot 导出 SpotLight AI_VISIBLE 字段") {
    World world;
    SceneGraph sg(world);

    // 路灯实体：SpotLight（S0.1 注册的 7 个 AI 字段应全部进快照）
    Entity e = world.CreateEntity("StreetLamp");
    world.AddComponent<TransformComponent>(e);
    auto* sl = world.AddComponent<SpotLight>(e);
    sl->direction = float3(0.0f, -1.0f, 0.0f);
    sl->color     = float3(1.0f, 0.9f, 0.7f);
    sl->intensity = 25.0f;
    sl->range     = 15.0f;
    sl->innerConeAngle = 0.3f;
    sl->outerConeAngle = 0.6f;
    sl->castShadow = true;
    sg.SetParent(e, Entity{kInvalidEntity});

    WorldModel wm;
    auto j = nlohmann::json::parse(wm.Snapshot(world, {}));

    REQUIRE(j["entities"].size() == 1);
    auto comps = j["entities"][0]["components"];
    bool foundSpot = false;
    for (auto& c : comps) {
        if (c["type"] != "SpotLight") continue;
        foundSpot = true;
        CHECK(c["fields"].contains("direction"));
        CHECK(c["fields"]["direction"][1] == -1.0f);
        CHECK(c["fields"]["color"][2] == 0.7f);
        CHECK(c["fields"]["intensity"] == 25.0f);
        CHECK(c["fields"]["innerConeAngle"].get<float>() == doctest::Approx(0.3f));
        CHECK(c["fields"]["castShadow"] == true);
    }
    CHECK(foundSpot);
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
