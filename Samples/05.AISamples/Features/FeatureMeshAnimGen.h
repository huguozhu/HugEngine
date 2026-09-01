#pragma once

#include "Features/IFeature.h"

#include "Scene/World.h"
#include "Scene/SceneGraph.h"
#include "AI/AIGC/GenerativeAssetFactory.h"

// ============================================================
// FeatureMeshAnimGen — 功能6：文生网格与动画（原 10.MeshAnimGen）
//   LLM 形状规格（无 key 降级金字塔）→ SetMeshData 渲染
//   + LLM 关键帧规格（无 key 降级摆动）→ AnimationComponent 播放
// ============================================================

class FeatureMeshAnimGen : public IFeature {
public:
    const char* GetName() const override { return "文生网格动画"; }

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
    he::ai::IAIDevice* m_AI = nullptr;

    he::Entity m_GenEntity;
    he::AnimationComponent* m_GenAnim = nullptr;

    he::ai::aigc::MeshGenResult m_Mesh;
    he::ai::aigc::AnimGenResult m_Anim;
};
