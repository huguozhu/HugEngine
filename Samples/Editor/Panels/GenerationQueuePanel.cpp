// Panels/GenerationQueuePanel.cpp — 生成队列面板实现
#include "Panels/GenerationQueuePanel.h"

#include "AI/AIGC/AICommand.h"
#include "Scene/World.h"
#include "Scene/SceneGraph.h"
#include "Editor/Command.h"
#include "Core/Log.h"

#include "imgui.h"

#include <memory>

namespace he::editor {

void GenerationQueuePanel::AddPending(String prompt, String sceneJson) {
    m_Queue.push_back({std::move(prompt), std::move(sceneJson)});
    HE_CORE_INFO("[GenerationQueue] 新生成结果入待接受队列（共 {} 项）", m_Queue.size());
}

void GenerationQueuePanel::Render() {
    if (!m_Visible) return;

    ImGui::SetNextWindowSize(ImVec2(480, 360), ImGuiCond_FirstUseEver);
    ImGui::Begin("生成队列", &m_Visible);

    if (m_Queue.empty()) {
        ImGui::TextWrapped("暂无待接受的生成结果。在「AI 生成场景」面板输入描述并点击生成。");
        ImGui::End();
        return;
    }

    // 逐项展示：prompt + 场景规格预览 + 接受/拒绝
    for (usize i = 0; i < m_Queue.size(); ++i) {
        auto& item = m_Queue[i];

        ImGui::Separator();
        ImGui::TextWrapped("Prompt: %s", item.prompt.c_str());
        // 规格预览（截断显示）
        if (ImGui::CollapsingHeader("查看场景 JSON")) {
            ImGui::TextWrapped("%.1000s", item.sceneJson.c_str());
        }

        // 接受：经命令历史装配实体（可撤销）
        if (ImGui::Button(("接受##accept_" + std::to_string(i)).c_str())) {
            if (m_World && m_SG && m_History) {
                // 生成器 = JSON→World 装配（BuildScene），由命令 Execute 时调用
                String sceneJson = item.sceneJson;
                m_History->Execute(std::make_unique<he::ai::aigc::GenerateSceneCommand>(
                    *m_World, *m_SG, item.prompt,
                    [sceneJson](he::World& w, he::SceneGraph& g, const he::String&) {
                        return he::ai::BuildScene(w, g, sceneJson);
                    }));
                HE_CORE_INFO("[GenerationQueue] 已接受生成，实体装配完成（可撤销）");
            } else {
                HE_CORE_ERROR("[GenerationQueue] 场景/命令历史未注入，无法装配");
            }
            m_Queue.erase(m_Queue.begin() + i);
            --i;
        }
        ImGui::SameLine();
        // 拒绝：直接丢弃
        if (ImGui::Button(("拒绝##reject_" + std::to_string(i)).c_str())) {
            HE_CORE_INFO("[GenerationQueue] 已拒绝生成结果");
            m_Queue.erase(m_Queue.begin() + i);
            --i;
        }
    }

    ImGui::End();
}

} // namespace he::editor
