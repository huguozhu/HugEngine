#include "AI/Agent/AgentSystem.h"

#include "AI/Agent/AgentComponent.h"
#include "AI/Agent/Brain.h"
#include "AI/Agent/LLMBrain.h"
#include "AI/Agent/MockBrain.h"
#include "AI/Agent/MemoryComponent.h"
#include "AI/WorldModel/WorldModel.h"
#include "AI/WorldModel/Action.h"
#include "AI/Runtime/AIDevice.h"
#include "Scene/World.h"
#include "Scene/SceneGraph.h"
#include "Editor/Command.h"
#include "Core/Log.h"

#include <memory>

namespace he::ai {

namespace {

// Mock 大脑的默认计划：每次生成一个不同位置/颜色的方块（演示用）。
// 位置沿 X 排开、分多行，颜色循环变化 —— 每次思考都能在画面上看到新方块出现。
ActionPlan MakeDefaultMockPlan() {
    static int s_Spawn = 0;                       // 已生成次数（演示计数器）
    const int i = s_Spawn++;

    // 位置：9 列 × 3 行排布，避免重叠
    const float x = -4.0f + (float)(i % 9) * 1.0f;
    const float y = 0.5f + (float)((i / 9) % 3) * 1.5f;

    // 颜色：循环 5 色，视觉醒目
    static const float3 kColors[] = {
        {0.2f, 0.6f, 1.0f}, {1.0f, 0.4f, 0.2f}, {0.4f, 1.0f, 0.4f},
        {1.0f, 0.9f, 0.2f}, {0.9f, 0.3f, 0.8f},
    };
    const float3 c = kColors[i % 5];

    ActionPlan plan;
    Action a;
    a.op = "SpawnEntity";
    a.argsJson = String("{\"name\":\"AgentCube\",\"transform\":{\"position\":[") +
                 std::to_string(x) + "," + std::to_string(y) + ",0],\"scale\":[1,1,1]}," +
                 "\"components\":[{\"type\":\"Cube\",\"halfExtent\":0.5," +
                 "\"baseColor\":[" + std::to_string(c.x) + "," + std::to_string(c.y) +
                 "," + std::to_string(c.z) + "],\"metallic\":0.0,\"roughness\":0.6}]}";
    plan.actions.push_back(std::move(a));
    return plan;
}

} // namespace

void AgentSystem::Update(he::World& world, he::SceneGraph& sg,
                         he::CommandHistory& history, IAIDevice* device, f32 dt) {
    // 遍历所有智能体组件
    world.ForEach<AgentComponent>([&](Entity e, AgentComponent& agent) {
        if (!agent.enabled) return;

        // 1. 思考计时
        agent.m_ThinkTimer += dt;
        if (agent.m_ThinkTimer < agent.thinkInterval) return;
        agent.m_ThinkTimer = 0.0f;

        // 2. 构造观察：该实体的语义快照（只含 AI_VISIBLE 属性）
        WorldModel wm;
        ObservationFilter filter;
        filter.targetEntity = e.id;
        String obs = wm.Snapshot(world, filter);

        // 3. 记忆（可空）
        auto* memory = world.GetComponent<MemoryComponent>(e);

        // 4. 按 brainType 构造大脑
        std::unique_ptr<IBrain> brain;
        if (agent.brainType == "Mock") {
            brain = std::make_unique<MockBrain>(MakeDefaultMockPlan());
        } else {
            // 默认走 LLM（需要推理设备；无设备则跳过本次思考）
            if (!device) {
                HE_CORE_WARN("[AgentSystem] 无推理设备，LLM 大脑不可用（实体 {}）", e.id);
                return;
            }
            brain = std::make_unique<LLMBrain>(device, agent.systemPrompt);
        }

        // 5. 决策 → 动作计划
        ActionPlan plan = brain->Decide(obs, memory);

        // 6. 动作 → 命令 → 执行（可撤销）
        for (auto& a : plan.actions) {
            auto cmd = CompileAction(world, sg, a);
            if (cmd) {
                history.Execute(std::move(cmd));
                HE_CORE_INFO("[AgentSystem] 实体 {} 执行动作: {}", e.id, a.op);
            } else {
                HE_CORE_WARN("[AgentSystem] 动作编译失败，已跳过: {}", a.op);
            }
        }
    });
}

} // namespace he::ai
