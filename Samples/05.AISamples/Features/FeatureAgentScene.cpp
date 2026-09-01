#include "Features/FeatureAgentScene.h"

#include "AI/Runtime/AIDevice.h"
#include "AI/Agent/AgentComponent.h"
#include "AI/Agent/AgentSystem.h"
#include "Scene/Transform.h"
#include "Scene/CubeComponent.h"
#include "Scene/LightComponent.h"
#include "Scene/PhysicalSkyComponent.h"
#include "Core/Log.h"
#include "imgui.h"

using namespace he;

bool FeatureAgentScene::Initialize(rhi::IRHIDevice* device, rhi::IRHISwapChain* sc,
                                   he::ai::IAIDevice* ai) {
    m_AI = ai;
    (void)device; (void)sc;

    // 场景：地面 + 光源 + 天空
    {
        Entity e = m_World.CreateEntity("Ground");
        auto* xform = m_World.AddComponent<TransformComponent>(e);
        xform->position = float3(0, -1, 0);
        xform->scale    = float3(20, 0.2f, 20);
        auto* cube = m_World.AddComponent<CubeComponent>(e);
        cube->baseColorFactor = float4(0.3f, 0.3f, 0.35f, 1.0f);
        cube->roughnessFactor = 0.9f;
        m_SG.SetParent(e, Entity{kInvalidEntity});
    }
    {
        Entity e = m_World.CreateEntity("Sun");
        m_World.AddComponent<TransformComponent>(e);
        auto* dl = m_World.AddComponent<DirectionalLight>(e);
        dl->direction = float3(0.5f, -1, 0.5f);
        dl->intensity = 5.0f;
        dl->castShadow = true;
        m_SG.SetParent(e, Entity{kInvalidEntity});
    }
    {
        Entity e = m_World.CreateEntity("Sky");
        m_World.AddComponent<TransformComponent>(e);
        m_World.AddComponent<PhysicalSkyComponent>(e);
        m_SG.SetParent(e, Entity{kInvalidEntity});
    }
    // 智能体实体（Mock 大脑，2 秒思考一次）
    {
        Entity e = m_World.CreateEntity("Agent");
        m_AgentEntity = e;
        m_World.AddComponent<TransformComponent>(e);
        auto* agent = m_World.AddComponent<he::ai::AgentComponent>(e);
        agent->brainType     = "Mock";
        agent->thinkInterval = 2.0f;
        m_SG.SetParent(e, Entity{kInvalidEntity});
    }
    m_LastEntityCount = (int)m_World.GetEntityCount();
    HE_CORE_INFO("[AgentScene] 智能体已挂载（Mock 大脑，每 2s 思考一次）");
    return true;
}

void FeatureAgentScene::Shutdown() {}

void FeatureAgentScene::Update(float dt) {
    // 智能体节律驱动：计时 → 决策 → 动作执行（可撤销）
    he::ai::AgentSystem::Update(m_World, m_SG, m_History, m_AI, dt);
    if ((int)m_World.GetEntityCount() != m_LastEntityCount) {
        m_LastEntityCount = (int)m_World.GetEntityCount();
        ++m_ThinkCount;
        HE_CORE_INFO("[AgentScene] 智能体思考完成，实体数: {}", m_LastEntityCount);
    }
}

void FeatureAgentScene::RenderUI() {
    ImGui::Begin("AI 智能体");
    ImGui::Text("实体数: %d | 思考次数: %d", m_LastEntityCount, m_ThinkCount);
    ImGui::Separator();
    auto* agent = m_World.GetComponent<he::ai::AgentComponent>(m_AgentEntity);
    if (agent) {
        ImGui::Checkbox("enabled", &agent->enabled);
        ImGui::SliderFloat("thinkInterval(s)", &agent->thinkInterval, 0.2f, 10.0f);
        const char* brains[] = {"Mock", "LLM"};
        int cur = (agent->brainType == "LLM") ? 1 : 0;
        if (ImGui::Combo("brainType", &cur, brains, 2))
            agent->brainType = (cur == 1) ? "LLM" : "Mock";

        if (ImGui::Button("立即思考")) {
            agent->m_ThinkTimer = agent->thinkInterval;
        }
        ImGui::SameLine();
        if (ImGui::Button("撤销上一步")) {
            m_History.Undo();
            m_LastEntityCount = (int)m_World.GetEntityCount();
        }
    }
    ImGui::End();
}
