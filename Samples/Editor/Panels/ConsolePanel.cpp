// Panels/ConsolePanel.cpp — 控制台实现
#include "ConsolePanel.h"
#include "Editor/CVar.h"
#include "imgui.h"

namespace he::editor {

void ConsolePanel::AddLog(const std::string& text, u32 color) {
    m_Log.push_back(text);
    // 限制最多 1000 行
    if (m_Log.size() > 1000) m_Log.erase(m_Log.begin());
}

void ConsolePanel::ExecuteCommand(const std::string& cmd) {
    AddLog("> " + cmd, 0xFF888888);

    if (cmd.empty()) return;
    if (cmd == "help") {
        AddLog("  set <name> <value>  — 设置 CVar");
        AddLog("  get <name>           — 查询 CVar");
        AddLog("  list                 — 列出所有 CVar");
        AddLog("  help                 — 显示帮助");
        return;
    }
    if (cmd == "list") {
        for (auto* cv : CVarBase::GetAll())
            AddLog("  " + cv->GetName() + " = " + cv->GetString());
        return;
    }

    // set <name> <value>
    if (cmd.rfind("set ", 0) == 0) {
        std::string rest = cmd.substr(4);
        auto sp = rest.find(' ');
        if (sp == std::string::npos) { AddLog("  用法: set <name> <value>", 0xFFFF8888); return; }
        std::string name = rest.substr(0, sp);
        std::string val  = rest.substr(sp + 1);
        auto* cv = FindCVar(name);
        if (!cv) { AddLog("  未找到: " + name, 0xFFFF8888); return; }
        cv->SetFromString(val);
        AddLog("  " + name + " = " + cv->GetString(), 0xFF88FF88);
        return;
    }

    // get <name>
    if (cmd.rfind("get ", 0) == 0) {
        std::string name = cmd.substr(4);
        auto* cv = FindCVar(name);
        if (!cv) { AddLog("  未找到: " + name, 0xFFFF8888); return; }
        AddLog("  " + name + " = " + cv->GetString(), 0xFF88FFFF);
        return;
    }

    AddLog("  未知命令: " + cmd + " (输入 help 查看帮助)", 0xFFFF8888);
}

void ConsolePanel::Render() {
    if (!m_Visible) return;

    ImGui::SetNextWindowSize(ImVec2(600, 300), ImGuiCond_FirstUseEver);
    ImGui::Begin("Console", &m_Visible);

    // 日志区域
    ImGui::BeginChild("LogRegion", ImVec2(-1, ImGui::GetContentRegionAvail().y - 28), true);
    for (auto& line : m_Log)
        ImGui::TextUnformatted(line.c_str());
    if (m_AutoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
        ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();

    // 输入框
    ImGui::PushItemWidth(-1);
    if (m_FocusInput) {
        ImGui::SetKeyboardFocusHere();
        m_FocusInput = false;
    }
    bool reclaimFocus = false;
    if (ImGui::InputText("##ConsoleInput", m_InputBuf, sizeof(m_InputBuf),
        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory,
        [](ImGuiInputTextCallbackData* data) {
            auto* self = (ConsolePanel*)data->UserData;
            int prev = self->m_HistoryPos;
            if (data->EventKey == ImGuiKey_UpArrow) {
                if (prev == -1) self->m_HistoryPos = (int)self->m_History.size() - 1;
                else if (prev > 0) self->m_HistoryPos--;
            } else if (data->EventKey == ImGuiKey_DownArrow) {
                if (prev != -1 && prev < (int)self->m_History.size() - 1) self->m_HistoryPos++;
                else self->m_HistoryPos = -1;
            }
            if (self->m_HistoryPos != prev && self->m_HistoryPos >= 0) {
                data->DeleteChars(0, data->BufTextLen);
                data->InsertChars(0, self->m_History[self->m_HistoryPos].c_str());
            } else if (self->m_HistoryPos == -1 && data->EventKey == ImGuiKey_DownArrow) {
                data->DeleteChars(0, data->BufTextLen);
            }
            return 0;
        }, this))
    {
        std::string cmd(m_InputBuf);
        if (!cmd.empty()) {
            ExecuteCommand(cmd);
            m_History.push_back(cmd);
            if (m_History.size() > 100) m_History.erase(m_History.begin());
            m_HistoryPos = -1;
        }
        memset(m_InputBuf, 0, sizeof(m_InputBuf));
        reclaimFocus = true;
    }
    ImGui::PopItemWidth();

    // 保持聚焦
    if (reclaimFocus) m_FocusInput = true;
    // 按 ` 键弹出控制台
    if (ImGui::IsKeyPressed(ImGuiKey_GraveAccent)) {
        m_Visible = !m_Visible;
        if (m_Visible) m_FocusInput = true;
    }

    ImGui::End();
}

} // namespace he::editor
