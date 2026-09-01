#pragma once

#include "Features/IFeature.h"

#include "Core/Types.h"
#include "Scene/World.h"
#include "Scene/SceneGraph.h"
#include "Editor/Command.h"

#include <memory>

// ============================================================
// FeatureLLMScene — 功能1：LLM 一句话生成场景（原 05.LLMScene）
// ============================================================

class FeatureLLMScene : public IFeature {
public:
    const char* GetName() const override { return "LLM 场景生成"; }

    bool Initialize(he::rhi::IRHIDevice* device, he::rhi::IRHISwapChain* sc,
                    he::ai::IAIDevice* ai) override;
    void Shutdown() override;
    void Update(float dt) override;
    void RenderUI() override;

    he::World* GetWorld() override { return &m_World; }
    he::SceneGraph* GetSceneGraph() override { return &m_SG; }

private:
    // 生成场景（LLM 或降级内置），供 Initialize / UI 重新生成
    void Generate();

    he::World m_World;
    he::SceneGraph m_SG{m_World};
    he::CommandHistory m_History;
    he::ai::IAIDevice* m_AI = nullptr;

    he::String m_Prompt = "一个黄昏下的中世纪村庄，几间石屋、一口井和一盏温暖的篝火";
    char   m_PromptBuf[1024] = "一个黄昏下的中世纪村庄，几间石屋、一口井和一盏温暖的篝火";
    bool   m_Generating = false;
    he::String m_ResultInfo;
};
