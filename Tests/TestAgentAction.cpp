// ============================================================
// Tests/TestAgentAction.cpp — 智能体动作编译单元测试
//
// 覆盖：SpawnEntity（创建/撤销）、SetTransform（改值/撤销）、
//       SetProperty（反射读写/撤销）、非法动作返回空。
// ============================================================

#include "doctest.h"

#include "AI/WorldModel/Action.h"
#include "Scene/World.h"
#include "Scene/SceneGraph.h"
#include "Scene/Transform.h"
#include "Scene/CubeComponent.h"
#include "Editor/Command.h"

#include <memory>

using namespace he;
using namespace he::ai;

// 便捷：执行一条 Action（经 CommandHistory）
static void ExecuteAction(World& world, SceneGraph& sg, CommandHistory& history, const Action& a) {
    auto cmd = CompileAction(world, sg, a);
    REQUIRE(cmd != nullptr);
    history.Execute(std::move(cmd));
}

TEST_CASE("Action SpawnEntity 创建实体且可撤销") {
    World world;
    SceneGraph sg(world);
    CommandHistory history;

    Action a;
    a.op = "SpawnEntity";
    a.argsJson = R"({"name":"Spawned","transform":{"position":[1,2,3]},
                     "components":[{"type":"Cube","halfExtent":0.5}]})";
    ExecuteAction(world, sg, history, a);

    REQUIRE(world.GetEntityCount() == 1);
    auto* xform = world.GetComponent<TransformComponent>(Entity{1});  // 空 world 首个实体 id=1
    REQUIRE(xform != nullptr);
    CHECK(xform->position.x == doctest::Approx(1.0f));

    history.Undo();
    CHECK(world.GetEntityCount() == 0);   // 撤销复原
}

TEST_CASE("Action SetTransform 修改位置且可撤销") {
    World world;
    SceneGraph sg(world);
    Entity e = world.CreateEntity("Target");
    auto* xform = world.AddComponent<TransformComponent>(e);
    xform->position = float3(0.0f, 0.0f, 0.0f);
    xform->scale    = float3(1.0f);
    sg.SetParent(e, Entity{kInvalidEntity});

    CommandHistory history;
    Action a;
    a.targetEntity = e.id;
    a.op = "SetTransform";
    a.argsJson = R"({"position":[5,6,7],"scale":[2,2,2]})";
    ExecuteAction(world, sg, history, a);

    CHECK(xform->position.x == doctest::Approx(5.0f));
    CHECK(xform->scale.x == doctest::Approx(2.0f));

    history.Undo();
    CHECK(xform->position.x == doctest::Approx(0.0f));   // 旧值恢复
    CHECK(xform->scale.x == doctest::Approx(1.0f));
}

TEST_CASE("Action SetProperty 经反射改属性且可撤销") {
    World world;
    SceneGraph sg(world);
    Entity e = world.CreateEntity("Target");
    world.AddComponent<TransformComponent>(e);
    sg.SetParent(e, Entity{kInvalidEntity});

    CommandHistory history;
    Action a;
    a.targetEntity = e.id;
    a.op = "SetProperty";
    a.argsJson = R"({"component":"he::TransformComponent","property":"position","value":[9,9,9]})";
    ExecuteAction(world, sg, history, a);

    auto* xform = world.GetComponent<TransformComponent>(e);
    REQUIRE(xform != nullptr);
    CHECK(xform->position.y == doctest::Approx(9.0f));

    history.Undo();
    CHECK(xform->position.y == doctest::Approx(0.0f));   // 反射旧值恢复
}

TEST_CASE("Action 非法动作返回 nullptr") {
    World world;
    SceneGraph sg(world);

    // 未知 op
    Action a1;
    a1.op = "FlyToMoon";
    { auto cmd = CompileAction(world, sg, a1); CHECK(cmd == nullptr); }

    // 非法 JSON
    Action a2;
    a2.op = "SetTransform";
    a2.argsJson = "{ 不是 JSON ";
    { auto cmd = CompileAction(world, sg, a2); CHECK(cmd == nullptr); }

    // 目标实体不存在
    Action a3;
    a3.op = "SetTransform";
    a3.targetEntity = 999;
    a3.argsJson = R"({"position":[1,2,3]})";
    { auto cmd = CompileAction(world, sg, a3); CHECK(cmd == nullptr); }

    // 属性不存在
    World w2;
    SceneGraph sg2(w2);
    Entity e = w2.CreateEntity("T");
    w2.AddComponent<TransformComponent>(e);
    sg2.SetParent(e, Entity{kInvalidEntity});
    Action a4;
    a4.targetEntity = e.id;
    a4.op = "SetProperty";
    a4.argsJson = R"({"component":"he::TransformComponent","property":"notExist","value":1})";
    { auto cmd = CompileAction(w2, sg2, a4); CHECK(cmd == nullptr); }
}
