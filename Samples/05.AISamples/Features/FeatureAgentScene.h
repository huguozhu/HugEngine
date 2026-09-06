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
    // 生成一枚抛物线抛射物（P1 A7 演示）
    void SpawnProjectile();

    he::World m_World;
    he::SceneGraph m_SG{m_World};
    he::CommandHistory m_History;         // 智能体动作历史（可撤销）
    he::ai::IAIDevice* m_AI = nullptr;
    he::Entity m_AgentEntity;             // 智能体实体

    int m_ThinkCount = 0;                 // 累计思考次数
    int m_LastEntityCount = 0;
    float m_ProjectileTimer = 0.0f;       // 抛射物补射计时（每 5 秒一枚）
};
