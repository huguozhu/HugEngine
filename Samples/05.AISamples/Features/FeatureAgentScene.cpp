#include "Features/FeatureAgentScene.h"

#include "AI/Runtime/AIDevice.h"
#include "AI/Agent/AgentComponent.h"
#include "AI/Agent/AgentSystem.h"
#include "Scene/Transform.h"
#include "Scene/CubeComponent.h"
#include "Scene/SphereComponent.h"
#include "Scene/LightComponent.h"
#include "Scene/PhysicalSkyComponent.h"
#include "Scene/HealthComponent.h"
#include "Scene/ProjectileMovementComponent.h"
#include "Scene/ProjectileSystem.h"
#include "Scene/AbilityComponent.h"
#include "Scene/AbilitySystem.h"
#include "Scene/SplineComponent.h"
#include "Scene/SplineSystem.h"
#include "Core/Log.h"
#include "imgui.h"

#include <algorithm>

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
    // 智能体实体（Mock 大脑，2 秒思考一次；带 100 点血，P1 A8 演示——
    // Health 属性进 WorldModel 快照，LLM 大脑可读到智能体血量；
    // 紫色小球 = 巡逻中的智能体本体）
    {
        Entity e = m_World.CreateEntity("Agent");
        m_AgentEntity = e;
        m_World.AddComponent<TransformComponent>(e);
        auto* vis = m_World.AddComponent<SphereComponent>(e);
        vis->radius = 0.3f;
        vis->segmentCount = 12;
        vis->ringCount = 6;
        vis->baseColorFactor = float4(0.7f, 0.3f, 1.0f, 1.0f);   // 紫色
        vis->castShadow = false;
        vis->OnCreate();
        auto* agent = m_World.AddComponent<he::ai::AgentComponent>(e);
        agent->brainType     = "Mock";
        agent->thinkInterval = 2.0f;
        auto* health = m_World.AddComponent<HealthComponent>(e);
        health->maxHealth     = 100.0f;
        health->currentHealth = 100.0f;
        // 技能组件（P1 B4 演示）：火球，冷却 5 秒，消耗 10 资源
        auto* ability = m_World.AddComponent<AbilityComponent>(e);
        m_FireballSkill = ability->AddSkill("Fireball", 5.0f, 10.0f);
        // 施放回调 = 生成火球实体（橙色球 + 抛射物运动，飞向目标点）
        ability->onCast = [this](Entity caster, const SkillDef&, const float3& target) {
            Entity fireball = m_World.CreateEntity("Fireball");
            auto* fx = m_World.AddComponent<TransformComponent>(fireball);
            if (auto* cx = m_World.GetComponent<TransformComponent>(caster))
                fx->position = cx->position + float3(0.0f, 1.0f, 0.0f);
            float3 dir = glm::normalize(target - fx->position + float3(0.0001f));
            fx->rotation = glm::quatLookAtRH(dir, float3(0, 1, 0));
            auto* vis = m_World.AddComponent<SphereComponent>(fireball);
            vis->radius = 0.25f;
            vis->segmentCount = 12;
            vis->ringCount = 6;
            vis->baseColorFactor = float4(1.0f, 0.5f, 0.1f, 1.0f);
            vis->emissiveFactor  = float3(1.0f, 0.4f, 0.05f);   // 自发光火球
            vis->castShadow = false;
            vis->OnCreate();
            auto* proj = m_World.AddComponent<ProjectileMovementComponent>(fireball);
            proj->initialSpeed = 8.0f;
            proj->gravityScale = 0.3f;   // 轻微下坠的火球弹道
            proj->lifetime     = 2.5f;   // 超时自动销毁
            m_SG.SetParent(fireball, Entity{kInvalidEntity});
            HE_CORE_INFO("[AgentScene] 施放技能: Fireball → 目标 ({:.1f},{:.1f},{:.1f})",
                target.x, target.y, target.z);
        };
        m_SG.SetParent(e, Entity{kInvalidEntity});
    }
    // 巡逻路径（P3 B2 演示）：闭合样条，智能体沿弧长匀速巡逻
    {
        Entity e = m_World.CreateEntity("PatrolPath");
        m_World.AddComponent<TransformComponent>(e);
        auto* spline = m_World.AddComponent<SplineComponent>(e);
        spline->bClosedLoop = true;
        spline->AddPoint(float3(-4, 0.5f, -4));
        spline->AddPoint(float3( 4, 0.5f, -4));
        spline->AddPoint(float3( 4, 0.5f,  4));
        spline->AddPoint(float3(-4, 0.5f,  4));
        m_PatrolEntity = e;
        HE_CORE_INFO("[AgentScene] 巡逻路径总长: {:.1f} 米", spline->GetTotalLength());
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

    // 抛射物系统：积分运动 + 超时销毁（P1 A7；火球技能复用此组件飞行）
    he::ProjectileSystem::Update(m_World, dt);

    // 巡逻路径（P3 B2）：智能体沿闭合样条以 2 m/s 匀速巡逻
    if (auto* spline = m_World.GetComponent<SplineComponent>(m_PatrolEntity)) {
        m_PatrolDistance += 2.0f * dt;
        float3 pos = SplineSystem::EvaluateAtDistance(*spline, m_PatrolDistance);
        if (auto* xf = m_World.GetComponent<TransformComponent>(m_AgentEntity)) {
            xf->position = pos;
        }
    }

    // 技能系统（P3 B4）：冷却计时 + 资源回复 + 自动施放（开关可关，留给手动按钮）
    he::AbilitySystem::Update(m_World, dt);
    if (auto* ability = m_World.GetComponent<AbilityComponent>(m_AgentEntity)) {
        // 回复 10/s：平均消耗 2/s（10 资源 / 5 秒冷却），可长期维持
        ability->resource = std::min(ability->maxResource, ability->resource + 10.0f * dt);
        if (m_AutoCast && ability->CanCast(m_FireballSkill)) {
            // 朝右侧方块堆自动施放（Agent 动作链 CastAbility 的等价演示）
            ability->Cast(m_FireballSkill, float3(6.0f, 0.5f, 0.0f));
        }
    }
}

void FeatureAgentScene::RenderUI() {
    ImGui::Begin("AI 智能体");
    ImGui::Text("实体数: %d | 思考次数: %d", m_LastEntityCount, m_ThinkCount);
    ImGui::Separator();
    // 技能状态（P3 B4）：资源/冷却 + 自动施放开关 + 手动施放按钮
    if (auto* ability = m_World.GetComponent<AbilityComponent>(m_AgentEntity)) {
        ImGui::Text("技能 Fireball：资源 %.0f/%.0f | 冷却 %.1fs",
            ability->resource, ability->maxResource,
            ability->cooldownRemaining[m_FireballSkill]);
        ImGui::Checkbox("自动施放（每 5 秒）", &m_AutoCast);
        // 冷却中/资源不足时按钮置灰（ImGui 禁用态），避免"点不动"困惑
        bool canCast = ability->CanCast(m_FireballSkill);
        if (!canCast) ImGui::BeginDisabled();
        if (ImGui::Button("立即施放火球")) {
            if (ability->Cast(m_FireballSkill, float3(6.0f, 0.5f, 0.0f)))
                HE_CORE_INFO("[AgentScene] 手动施放火球");
        }
        if (!canCast) ImGui::EndDisabled();
        if (!canCast && !m_AutoCast)
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "冷却中或资源不足");
        ImGui::Separator();
    }
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
