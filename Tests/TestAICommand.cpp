// ============================================================
// Tests/TestAICommand.cpp — 生成命令（可撤销）单元测试
//
// 覆盖：GenerateSceneCommand Execute/Undo 对称性
//       （创建 N 实体 → Undo 后 GetEntityCount 复原），
//       以及 CommandHistory 的 Redo。
// ============================================================

#include "doctest.h"

#include "AI/AIGC/AICommand.h"
#include "Scene/World.h"
#include "Scene/SceneGraph.h"
#include "Scene/Transform.h"
#include "Editor/Command.h"

#include <memory>

using namespace he;
using namespace he::ai;
using namespace he::ai::aigc;

namespace {

// 测试用生成器：创建 2 个带 Transform 的实体
SceneBuildResult MakeTwoEntities(World& world, SceneGraph& sg, const String&) {
    SceneBuildResult r;
    Entity e1 = world.CreateEntity("A");
    world.AddComponent<TransformComponent>(e1);
    sg.SetParent(e1, Entity{kInvalidEntity});
    Entity e2 = world.CreateEntity("B");
    world.AddComponent<TransformComponent>(e2);
    sg.SetParent(e2, Entity{kInvalidEntity});
    r.success = true;
    r.entities = {e1, e2};
    return r;
}

} // namespace

TEST_CASE("GenerateSceneCommand Execute/Undo 对称性") {
    World world;
    SceneGraph sg(world);
    he::CommandHistory history;

    history.Execute(std::make_unique<GenerateSceneCommand>(world, sg, "测试场景", MakeTwoEntities));
    REQUIRE(world.GetEntityCount() == 2);   // Execute 创建 2 个实体

    history.Undo();
    CHECK(world.GetEntityCount() == 0);     // Undo 后复原

    history.Redo();
    CHECK(world.GetEntityCount() == 2);     // Redo 重新创建
}

TEST_CASE("GenerateSceneCommand 生成失败不产生副作用") {
    World world;
    SceneGraph sg(world);
    he::CommandHistory history;

    // 失败生成器：返回 success=false
    auto failGen = [](World&, SceneGraph&, const String&) {
        SceneBuildResult r;
        r.success = false;
        r.error = "模拟失败";
        return r;
    };
    history.Execute(std::make_unique<GenerateSceneCommand>(world, sg, "失败场景", failGen));
    CHECK(world.GetEntityCount() == 0);     // 无副作用

    history.Undo();                         // 对失败命令 Undo 也应安全
    CHECK(world.GetEntityCount() == 0);
}
