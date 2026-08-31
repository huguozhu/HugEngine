// Panels/PromptPanel.cpp — 提示词面板实现
#include "Panels/PromptPanel.h"
#include "Panels/GenerationQueuePanel.h"

#include "AI/AIGC/AIPipeline.h"
#include "AI/AIGC/AIGCProvider.h"
#include "Core/Log.h"

#include "imgui.h"

#include <cstring>

namespace he::editor {

void PromptPanel::Render() {
    if (!m_Visible) return;

    ImGui::SetNextWindowSize(ImVec2(420, 320), ImGuiCond_FirstUseEver);
    ImGui::Begin("AI 生成场景", &m_Visible);

    // 提示词输入
    ImGui::TextWrapped("输入一句话描述场景，交给大模型生成：");
    ImGui::InputTextMultiline("##Prompt", m_PromptBuf, sizeof(m_PromptBuf),
                              ImVec2(-1, 140));

    // 生成按钮（生成中禁用）
    bool canGenerate = m_Pipeline && m_Queue && !m_Generating;
    if (ImGui::Button(m_Generating ? "生成中..." : "生成场景", ImVec2(-1, 0)) && canGenerate) {
        String prompt(m_PromptBuf);
        if (prompt.empty()) {
            m_LastError = "请输入场景描述";
        } else {
            m_Generating = true;
            m_LastError.clear();
            // 提交后台生成；完成回调在主线程（AIPipeline::Poll）执行
            m_Pipeline->Enqueue({he::ai::aigc::GenKind::Scene, prompt},
                [this, prompt](he::ai::aigc::GenResult&& r) {
                    m_Generating = false;
                    if (r.success && !r.sceneJson.empty()) {
                        // 结果入待接受队列（不直接装配，用户确认后才写入场景）
                        m_Queue->AddPending(prompt, std::move(r.sceneJson));
                    } else {
                        m_LastError = r.error.empty() ? "生成失败" : r.error;
                        HE_CORE_ERROR("[PromptPanel] 生成失败: {}", m_LastError);
                    }
                });
        }
    }

    // 错误信息展示
    if (!m_LastError.empty()) {
        ImGui::TextColored({1.0f, 0.4f, 0.4f, 1.0f}, "%s", m_LastError.c_str());
    }

    ImGui::End();
}

} // namespace he::editor
