#include "Features/FeatureLLMScene.h"

#include "AI/Runtime/AIDevice.h"
#include "AI/AIGC/GenerativeAssetFactory.h"
#include "Scene/Transform.h"
#include "Scene/LightComponent.h"
#include "Scene/PhysicalSkyComponent.h"
#include "Core/Log.h"
#include "imgui.h"

#include <cstring>

using namespace he;

bool FeatureLLMScene::Initialize(rhi::IRHIDevice* device, rhi::IRHISwapChain* sc,
                                 he::ai::IAIDevice* ai) {
    m_AI = ai;
    (void)device; (void)sc;
    Generate();   // 启动即生成一次
    return true;
}

void FeatureLLMScene::Shutdown() {}

void FeatureLLMScene::Update(float) {}

void FeatureLLMScene::Generate() {
    m_World.~World();                    // 重建世界（清空旧场景）
    new (&m_World) World();
    new (&m_SG) he::SceneGraph(m_World);

    if (!m_AI) return;
    he::ai::aigc::GenerativeAssetFactory factory;
    he::ai::aigc::GenerationResult r = factory.GenerateScene(m_World, m_SG, *m_AI, m_Prompt);
    if (r.success) {
        m_ResultInfo = "生成成功：" + std::to_string(r.entities.size()) + " 个实体";
    } else {
        m_ResultInfo = "生成失败：" + r.error;
    }
    HE_CORE_INFO("[LLMScene] {}", m_ResultInfo);
}

void FeatureLLMScene::RenderUI() {
    ImGui::Begin("LLM 场景生成");
    ImGui::Text("实体数: %d", (int)m_World.GetEntityCount());
    ImGui::TextWrapped("说明: %s", m_ResultInfo.c_str());
    ImGui::Separator();
    ImGui::InputTextMultiline("##Prompt", m_PromptBuf, sizeof(m_PromptBuf), ImVec2(-1, 90));
    if (ImGui::Button(m_Generating ? "生成中..." : "重新生成", ImVec2(-1, 0)) && !m_Generating) {
        m_Generating = true;
        m_Prompt = m_PromptBuf;
        Generate();
        m_Generating = false;
    }
    ImGui::End();
}
