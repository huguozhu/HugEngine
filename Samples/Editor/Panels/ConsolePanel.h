// Panels/ConsolePanel.h — 交互式命令控制台
#pragma once

#include "Core/Types.h"
#include <vector>
#include <string>

namespace he::editor {

class ConsolePanel {
public:
    bool m_Visible = false;

    void Render();

private:
    void ExecuteCommand(const std::string& cmd);
    void AddLog(const std::string& text, u32 color = 0xFFFFFFFF);

    std::vector<std::string> m_Log;        // 输出日志
    std::vector<std::string> m_History;    // 命令历史
    int   m_HistoryPos = -1;               // 当前浏览位置
    char  m_InputBuf[256] = {};            // 输入缓冲
    bool  m_AutoScroll = true;             // 自动滚动到底部
    bool  m_FocusInput = false;            // 下一帧聚焦输入框
};

} // namespace he::editor
