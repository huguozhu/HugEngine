// Samples/Editor/Panels/ContentBrowserPanel.h
#pragma once

// ============================================================
// ContentBrowserPanel — 资源浏览器
//
// 左侧目录树 + 右侧文件平铺视图。
// 双击 glTF 文件 → glTFLoader 加载到场景。
// ============================================================

#include "Core/Types.h"

namespace he::editor {
    class EditorContext;
}

namespace he::editor {

class ContentBrowserPanel {
public:
    void Initialize(EditorContext* ctx) { m_Ctx = ctx; }
    void Render();

private:
    void RenderDirectoryTree();
    void RenderFileGrid();
    void ImportGLB(const String& filePath);

    EditorContext* m_Ctx         = nullptr;
    String         m_CurrentPath = "Content";  // 默认浏览目录
    String         m_SelectedFile;
public:
    bool           m_Visible     = false;       // 由菜单控制，公开以便菜单 Item 直接引用
};

} // namespace he::editor
