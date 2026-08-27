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
