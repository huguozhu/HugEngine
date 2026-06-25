// Samples/Editor/Panels/ProjectSettingsPanel.cpp

#include "ProjectSettingsPanel.h"
#include "Editor/EditorContext.h"
#include "Editor/CVar.h"
#include "imgui.h"

namespace he::editor {

void ProjectSettingsPanel::Render() {
    if (!m_Ctx || !m_Visible)
        return;

    ImGui::Begin("Project Settings", &m_Visible);

    // 搜索过滤输入框
    ImGui::InputText("Search", m_SearchBuf, sizeof(m_SearchBuf));
    ImGui::Separator();

    // 收集搜索关键字（转换为小写用于不区分大小写匹配）
    String searchFilter(m_SearchBuf);
    for (auto& c : searchFilter) {
        if (c >= 'A' && c <= 'Z')
            c += ('a' - 'A');
    }

    // 遍历所有已注册的 CVar
    const auto& allCVars = CVarBase::GetAll();
    for (CVarBase* cvar : allCVars) {
        if (!cvar) continue;

        // 搜索过滤：匹配名称或描述
        if (!searchFilter.empty()) {
            String lowerName = cvar->GetName();
            for (auto& c : lowerName) {
                if (c >= 'A' && c <= 'Z')
                    c += ('a' - 'A');
            }
            String lowerDesc = cvar->GetDescription();
            for (auto& c : lowerDesc) {
                if (c >= 'A' && c <= 'Z')
                    c += ('a' - 'A');
            }
            if (lowerName.find(searchFilter) == String::npos &&
                lowerDesc.find(searchFilter) == String::npos) {
                continue;
            }
        }

        // 显示 CVar 名称和描述
        ImGui::PushID(cvar);
        ImGui::Text("%s", cvar->GetName().c_str());
        if (!cvar->GetDescription().empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("(%s)", cvar->GetDescription().c_str());
        }

        // 获取当前值
        CVarValue val = cvar->GetValue();

        // 根据类型渲染对应的编辑器控件
        if (auto* v = std::get_if<i32>(&val)) {
            int tmp = *v;
            if (ImGui::InputInt("##Value", &tmp)) {
                cvar->SetFromString(std::to_string(tmp));
            }
        } else if (auto* v = std::get_if<f32>(&val)) {
            float tmp = *v;
            if (ImGui::DragFloat("##Value", &tmp, 0.1f)) {
                cvar->SetFromString(std::to_string(tmp));
            }
        } else if (auto* v = std::get_if<bool>(&val)) {
            bool tmp = *v;
            if (ImGui::Checkbox("##Value", &tmp)) {
                cvar->SetFromString(tmp ? "true" : "false");
            }
        } else if (auto* v = std::get_if<String>(&val)) {
            // 使用固定大小缓冲编辑字符串
            char buf[256];
            size_t copyLen = v->size();
            if (copyLen >= sizeof(buf)) copyLen = sizeof(buf) - 1;
            memcpy(buf, v->data(), copyLen);
            buf[copyLen] = '\0';
            if (ImGui::InputText("##Value", buf, sizeof(buf))) {
                cvar->SetFromString(String(buf));
            }
        }

        ImGui::PopID();
        ImGui::Separator();
    }

    ImGui::End();
}

} // namespace he::editor
