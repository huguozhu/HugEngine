// ============================================================
// Tests/TestAgentBrain.cpp — 智能体记忆/目标/大脑/驱动测试
//
// 覆盖：记忆读写与淘汰、目标排序与达成、
//       LLMBrain 决策解析、AgentSystem 按节律驱动（MockBrain）。
// ============================================================

#include "doctest.h"

#include "AI/Agent/MemoryComponent.h"
#include "AI/Agent/GoalComponent.h"
#include "AI/Agent/AgentComponent.h"
#include "AI/Agent/Brain.h"
#include "AI/Agent/LLMBrain.h"
#include "AI/Agent/AgentSystem.h"
#include "AI/Runtime/AIDevice.h"
#include "Scene/World.h"
#include "Scene/SceneGraph.h"
#include "Scene/Transform.h"
#include "Editor/Command.h"

#include "nlohmann/json.hpp"

using namespace he;
using namespace he::ai;

TEST_CASE("MemoryComponent 写入/查询/覆盖/淘汰") {
    MemoryComponent mem;
    String out;

    mem.AddShortTerm("position", "house_01");
    mem.AddShortTerm("goal", "build_village");
    CHECK(mem.GetShortTermCount() == 2);
    CHECK(mem.Query("position", out));
    CHECK(out == "house_01");

    // 覆盖已有键
    mem.AddShortTerm("position", "house_02");
    CHECK(mem.GetShortTermCount() == 2);
    CHECK(mem.Query("position", out));
    CHECK(out == "house_02");

    // 未命中
    CHECK(!mem.Query("nope", out));

    // 清空
    mem.ClearShortTerm();
    CHECK(mem.GetShortTermCount() == 0);
}

TEST_CASE("GoalComponent 优先级排序与达成") {
    GoalComponent goals;
    goals.AddGoal("次要目标", 1.0f);
    goals.AddGoal("首要目标", 5.0f);
    goals.AddGoal("次要目标2", 2.0f);

    // 按优先级降序
    REQUIRE(goals.GetGoals().size() == 3);
    CHECK(goals.GetGoals()[0].priority == doctest::Approx(5.0f));
    CHECK(goals.GetGoals()[1].priority == doctest::Approx(2.0f));
    CHECK(goals.GetCurrentGoal() != nullptr);
    CHECK(goals.GetCurrentGoal()->description == "首要目标");

    goals.MarkAchieved(0);
    CHECK(goals.GetCurrentGoal()->description == "次要目标2");
    CHECK(goals.GetActiveGoalCount() == 2);
}

namespace {

// 假 AI 设备：Chat 返回固定动作 JSON（LLMBrain 用）
struct FakeAgentDevice : IAIDevice {
    AIDeviceCaps GetCaps() const override { return {}; }
    Ref<IAIModel> LoadModel(AIModelFormat, Span<const u8>, const String&) override { return nullptr; }
    Ref<IAITensor> CreateTensor(const AITensorDesc&) override { return nullptr; }
    Ref<IAIInference> Submit(InferenceRequest&&) override { return nullptr; }
    Ref<IAITensor> WrapRHITexture(rhi::IRHITexture*) override { return nullptr; }
    Ref<IAITensor> WrapRHIBuffer(rhi::IRHIBuffer*) override { return nullptr; }
    rhi::IRHIBuffer* ExportBuffer(IAITensor*) override { return nullptr; }

    String Chat(const String&, const String&) override {
        nlohmann::json actions = {
            {"actions", {{
                {"op", "SpawnEntity"},
                {"argsJson", {{"name", "BrainCube"},
                              {"transform", {{"position", {0, 1, 0}}}},
                              {"components", {{{"type", "Cube"}, {"halfExtent", 0.4f}}}}}}
            }}}
        };
        nlohmann::json resp = { {"choices", { {{"message", {{"content", actions.dump()}}}} }} };
        return resp.dump();
    }
    void ChatStream(const String&, const String&, std::function<void(const String&)>) override {}
};

} // namespace

TEST_CASE("LLMBrain 依据假设备决策出动作计划") {
    FakeAgentDevice dev;
    LLMBrain brain(&dev, "你是测试智能体");

    ActionPlan plan = brain.Decide("{\"entities\":[]}", nullptr);
    REQUIRE(plan.actions.size() == 1);
    CHECK(plan.actions[0].op == "SpawnEntity");
    CHECK(plan.actions[0].argsJson.find("BrainCube") != String::npos);
}

TEST_CASE("AgentSystem 按 thinkInterval 驱动 MockBrain") {
    World world;
    SceneGraph sg(world);
    CommandHistory history;

    // 智能体实体（Mock 大脑，0.1s 思考间隔）
    Entity agentEntity = world.CreateEntity("Agent");
    auto* agent = world.AddComponent<AgentComponent>(agentEntity);
    agent->brainType = "Mock";
    agent->thinkInterval = 0.1f;
    world.AddComponent<MemoryComponent>(agentEntity);
    world.AddComponent<GoalComponent>(agentEntity);
    sg.SetParent(agentEntity, Entity{kInvalidEntity});

    // 第一次 Update：累计 0.2s > 0.1s → 触发一次决策（Mock 默认 SpawnEntity）
    AgentSystem::Update(world, sg, history, nullptr, 0.2f);
    REQUIRE(world.GetEntityCount() == 2);          // Agent + AgentCube

    // 第二次 Update：间隔不足 → 不触发
    AgentSystem::Update(world, sg, history, nullptr, 0.05f);
    CHECK(world.GetEntityCount() == 2);

    // 撤销：Mock 的动作是 SpawnEntity 命令 → 实体复原
    history.Undo();
    CHECK(world.GetEntityCount() == 1);
}

TEST_CASE("AgentSystem 禁用智能体不思考") {
    World world;
    SceneGraph sg(world);
    CommandHistory history;

    Entity agentEntity = world.CreateEntity("Agent");
    auto* agent = world.AddComponent<AgentComponent>(agentEntity);
    agent->brainType = "Mock";
    agent->enabled = false;
    sg.SetParent(agentEntity, Entity{kInvalidEntity});

    AgentSystem::Update(world, sg, history, nullptr, 1.0f);
    CHECK(world.GetEntityCount() == 1);   // 无新实体
}
