// ============================================================
// Tests/TestPromptToScene.cpp — PromptToScene 端到端单元测试
//
// 使用假 LLM（FakeLLM）返回固定 OpenAI 兼容响应，
// 验证 prompt → LLM → 场景 JSON → World 的完整链路。
// ============================================================

#include "doctest.h"

#include "AI/PromptToScene.h"
#include "AI/LLMClient.h"
#include "Scene/World.h"
#include "Scene/SceneGraph.h"
#include "Scene/SphereComponent.h"
#include "Scene/Transform.h"
#include "Scene/LightComponent.h"
#include "Scene/CameraComponent.h"
#include "Scene/HealthComponent.h"

#include "nlohmann/json.hpp"

using namespace he;
using namespace he::ai;

// 假 LLM：返回固定 HTTP 响应（choices[0].message.content = 场景 JSON）
struct FakeLLM : ILLMClient {
    String Chat(const String&, const String&) override {
        nlohmann::json scene = {
            {"entities", {{
                {"name","Ball"},
                {"transform",{{"position",{1,2,3}}}},
                {"components",{ {{"type","Sphere"},{"radius",1.0}} }}
            }}}
        };
        nlohmann::json resp = { {"choices", { {{"message", {{"content", scene.dump()}}}} }} };
        return resp.dump();
    }
};

TEST_CASE("PromptToScene 用假 LLM 生成场景") {
    World world;
    SceneGraph sg(world);
    FakeLLM fake;

    SceneBuildResult r = PromptToScene(fake, world, sg, "一个球");

    REQUIRE(r.success == true);
    REQUIRE(r.entities.size() == 1);
    auto* sphere = world.GetComponent<SphereComponent>(r.entities[0]);
    REQUIRE(sphere != nullptr);
    CHECK(sphere->radius == doctest::Approx(1.0f));
    auto* xform = world.GetComponent<TransformComponent>(r.entities[0]);
    REQUIRE(xform != nullptr);
    CHECK(xform->position.x == doctest::Approx(1.0f));
}

TEST_CASE("BuildSceneSystemPrompt 词表包含 S0.3 新增组件") {
    // S0.3：LLM system prompt 词表必须包含 SpotLight/RectLight/Camera
    //（「一个路灯照着的街角」类 prompt 依赖 SpotLight 词条）
    String p = BuildSceneSystemPrompt();
    CHECK(p.find("SpotLight") != String::npos);
    CHECK(p.find("RectLight") != String::npos);
    CHECK(p.find("Camera") != String::npos);
    CHECK(p.find("innerConeAngle") != String::npos);   // SpotLight 字段
    CHECK(p.find("softness") != String::npos);         // RectLight 字段
    CHECK(p.find("isMain") != String::npos);           // Camera 字段
    // P1 A8：Health 词条（LLM 可"给敌人 100 点血"）
    CHECK(p.find("Health") != String::npos);
    CHECK(p.find("maxHealth") != String::npos);
}

// 假 LLM：返回「高血量守卫」场景（P1 A8 用例的离线等价）
struct FakeGuardLLM : ILLMClient {
    String Chat(const String&, const String&) override {
        nlohmann::json scene = {
            {"entities", {{
                {"name","EliteGuard"},
                {"transform",{{"position",{0,0,4}}}},
                {"components",{
                    {{"type","Cube"},{"halfExtent",0.5},{"baseColor",{0.8,0.1,0.1}}},
                    {{"type","Health"},{"maxHealth",300},{"currentHealth",300}}
                }}
            }}}
        };
        nlohmann::json resp = { {"choices", { {{"message", {{"content", scene.dump()}}}} }} };
        return resp.dump();
    }
};

TEST_CASE("PromptToScene 生成高血量守卫（P1 A8 用例）") {
    World world;
    SceneGraph sg(world);
    FakeGuardLLM fake;

    SceneBuildResult r = PromptToScene(fake, world, sg, "一个高血量的守卫");
    REQUIRE(r.success == true);
    REQUIRE(r.entities.size() == 1);

    auto* h = world.GetComponent<HealthComponent>(r.entities[0]);
    REQUIRE(h != nullptr);
    CHECK(h->maxHealth == doctest::Approx(300.0f));
    CHECK(h->currentHealth == doctest::Approx(300.0f));
    CHECK_FALSE(h->IsDead());
}

// 假 LLM：返回「路灯 + 相机」场景（S0.3 冒烟用例的离线等价）
struct FakeStreetLLM : ILLMClient {
    String Chat(const String&, const String&) override {
        nlohmann::json scene = {
            {"entities", {{
                {"name","StreetLamp"},
                {"transform",{{"position",{0,4,0}}}},
                {"components",{ {{"type","SpotLight"},{"direction",{0,-1,0}},
                                 {"intensity",30},{"range",12},
                                 {"innerConeAngle",0.35},{"outerConeAngle",0.7},
                                 {"castShadow",true}} }}
            },{
                {"name","MainCamera"},
                {"transform",{{"position",{0,2,6}}}},
                {"components",{ {{"type","Camera"},{"fov",50},{"isMain",true}} }}
            }}}
        };
        nlohmann::json resp = { {"choices", { {{"message", {{"content", scene.dump()}}}} }} };
        return resp.dump();
    }
};

TEST_CASE("PromptToScene 生成路灯场景：SpotLight + 主相机（S0.3/S0.4 冒烟）") {
    World world;
    SceneGraph sg(world);
    FakeStreetLLM fake;

    SceneBuildResult r = PromptToScene(fake, world, sg, "一个路灯照着的街角");
    REQUIRE(r.success == true);
    REQUIRE(r.entities.size() == 2);

    // SpotLight 组件 + 关键字段
    auto* sl = world.GetComponent<SpotLight>(r.entities[0]);
    REQUIRE(sl != nullptr);
    CHECK(sl->intensity == doctest::Approx(30.0f));
    CHECK(sl->innerConeAngle == doctest::Approx(0.35f));
    CHECK(sl->castShadow == true);

    // 相机实体成为主相机（渲染帧入口 ResolveFrameCamera 的前提成立）
    auto* cam = world.GetComponent<CameraComponent>(r.entities[1]);
    REQUIRE(cam != nullptr);
    CHECK(cam->fov == doctest::Approx(50.0f));
    CHECK(world.GetPrimaryCamera() == cam);
}
