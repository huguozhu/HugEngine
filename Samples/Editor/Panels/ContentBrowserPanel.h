// Samples/Editor/Panels/ContentBrowserPanel.h
#pragma once

// ============================================================
// ContentBrowserPanel — 资源浏览器
//
// 左侧目录树 + 右侧文件平铺视图。
// 双击 glTF 文件 → glTFLoader 加载到场景。
// ============================================================

#include "Core/Types.h"

#include <filesystem>

namespace he::editor {
    class EditorContext;
}

namespace he::editor {

class ContentBrowserPanel {
public:
    void Initialize(EditorContext* ctx) {
        m_Ctx = ctx;
        // 将默认路径转为绝对路径，确保 directory_iterator 不受工作目录影响
        std::error_code ec;
        auto absPath = std::filesystem::absolute("Content", ec);
        if (!ec) m_CurrentPath = absPath.string();
    }
    void Render();

private:
    void RenderDirectoryTree();
    void RenderFileGrid();
    void ImportGLTF(const String& filePath); // 通过 cgltf 加载 .glb / .gltf

    EditorContext* m_Ctx         = nullptr;
    String         m_CurrentPath = "Content";  // 默认浏览目录
    String         m_SelectedFile;
public:
    bool           m_Visible     = false;       // 由菜单控制，公开以便菜单 Item 直接引用
};

} // namespace he::editor
