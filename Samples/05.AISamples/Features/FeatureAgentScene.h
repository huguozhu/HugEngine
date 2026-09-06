#pragma once

#include "Features/IFeature.h"

#include "Scene/World.h"
#include "Scene/SceneGraph.h"
#include "Editor/Command.h"

// ============================================================
// FeatureAgentScene — 功能2：AI 智能体演示（原 06.AgentScene）
// ============================================================

class FeatureAgentScene : public IFeature {
public:
    const char* GetName() const override { return "AI 智能体"; }

    bool Initialize(he::rhi::IRHIDevice* device, he::rhi::IRHISwapChain* sc,
                    he::ai::IAIDevice* ai) override;
    void Shutdown() override;
    void Update(float dt) override;
    void RenderUI() override;

    he::World* GetWorld() override { return &m_World; }
    he::SceneGraph* GetSceneGraph() override { return &m_SG; }

private:
    he::World m_World;
    he::SceneGraph m_SG{m_World};
    he::CommandHistory m_History;         // 智能体动作历史（可撤销）
    he::ai::IAIDevice* m_AI = nullptr;
    he::Entity m_AgentEntity;             // 智能体实体

    int m_ThinkCount = 0;                 // 累计思考次数
    int m_LastEntityCount = 0;
    int   m_FireballSkill = -1;           // 火球技能下标（B4 演示）
    bool  m_AutoCast = true;              // 自动施放开关（关闭后用手动按钮）
};
