// Samples/Editor/Panels/ProjectSettingsPanel.h
#pragma once

// ============================================================
// ProjectSettingsPanel — 项目设置面板
//
// 提供 CVar 系统的可视化编辑界面，支持搜索过滤，
// 按类型渲染不同的编辑器控件（int / float / bool / string）。
// ============================================================

#include "Core/Types.h"

namespace he::editor {
    class EditorContext;
}

namespace he::editor {

class ProjectSettingsPanel {
public:
    void Initialize(EditorContext* ctx) { m_Ctx = ctx; }

    /// 渲染项目设置面板（每帧调用）
    void Render();

    bool m_Visible = false; // 由菜单栏 Checkbox 控制显隐

private:
    EditorContext* m_Ctx = nullptr;
    char m_SearchBuf[128] = {}; // 搜索过滤缓冲区
};

} // namespace he::editor
