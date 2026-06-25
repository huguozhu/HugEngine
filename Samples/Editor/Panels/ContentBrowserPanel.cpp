// Samples/Editor/Panels/ContentBrowserPanel.cpp
#include "ContentBrowserPanel.h"
#include "Core/Core.h"
#include "Editor/EditorContext.h"
#include "Scene/World.h"
#include "Scene/SceneGraph.h"
#include "Asset/glTFLoader.h"
#include "imgui.h"

#include <filesystem>

namespace he::editor {

void ContentBrowserPanel::Render() {
    if (!m_Visible) return;

    ImGui::Begin("Content Browser", &m_Visible);

    // 左右分栏
    ImGui::Columns(2, "ContentBrowserColumns", true);
    ImGui::SetColumnWidth(0, 200);

    RenderDirectoryTree();

    ImGui::NextColumn();

    // 路径导航：.. 按钮在前，防止被路径文字挤出视野
    if (ImGui::Button("..")) {
        auto parent = std::filesystem::path(m_CurrentPath).parent_path();
        String parentStr = parent.string();
        // 防止退到 Content 目录之上（parent_path 对 "Content" 返回 ""）
        if (!parentStr.empty()) m_CurrentPath = parentStr;
    }
    ImGui::SameLine();
    ImGui::Text("Path: %s", m_CurrentPath.c_str());
    ImGui::Separator();

    RenderFileGrid();

    ImGui::Columns(1);
    ImGui::End();
}

void ContentBrowserPanel::RenderDirectoryTree() {
    ImGui::Text("Directories");
    ImGui::Separator();

    // 列出 Content 目录下的子目录
    std::error_code ec;
    for (auto& entry : std::filesystem::directory_iterator(m_CurrentPath, ec)) {
        if (entry.is_directory()) {
            String name = entry.path().filename().string();
            if (ImGui::Selectable(name.c_str(), false)) {
                m_CurrentPath = entry.path().string();
            }
        }
    }
}

void ContentBrowserPanel::RenderFileGrid() {
    std::error_code ec;
    int itemCount = 0; // 非 static，每帧重置
    for (auto& entry : std::filesystem::directory_iterator(m_CurrentPath, ec)) {
        if (entry.is_regular_file()) {
            String name = entry.path().filename().string();
            String ext  = entry.path().extension().string();

            // 仅显示支持的文件类型
            if (ext != ".glb" && ext != ".gltf" && ext != ".png" && ext != ".jpg" && ext != ".hescene")
                continue;

            bool isGLB = (ext == ".glb" || ext == ".gltf");

            // 平铺视图：每行 4 个
            ImGui::BeginGroup();
            ImGui::PushID(name.c_str());

            // 图标占位
            ImGui::Button(isGLB ? "3D" : "IMG", {60, 60});

            // 文件名（截断过长名称）
            String displayName = name;
            if (displayName.size() > 12)
                displayName = displayName.substr(0, 10) + "..";
            ImGui::Text("%s", displayName.c_str());

            // 双击导入
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                if (isGLB) {
                    ImportGLB(entry.path().string());
                } else if (ext == ".hescene") {
                    // 场景文件加载
                    // Phase 3-2: 通过 SceneSerializer::Load
                }
            }

            // 右键菜单
            if (ImGui::BeginPopupContextItem(name.c_str())) {
                if (isGLB && ImGui::MenuItem("Import to Scene")) {
                    ImportGLB(entry.path().string());
                }
                ImGui::MenuItem("Show in Explorer");
                ImGui::EndPopup();
            }

            ImGui::PopID();
            ImGui::EndGroup();

            // 每行 4 个，自动换行
            itemCount++;
            if (itemCount % 4 != 0)
                ImGui::SameLine();
        }
    }
}

void ContentBrowserPanel::ImportGLB(const String& filePath) {
    if (!m_Ctx) return;
    auto* world = m_Ctx->GetWorld();
    auto* sg    = m_Ctx->GetSceneGraph();
    if (!world || !sg) return;

    HE_CORE_INFO("Importing GLB: {}", filePath);
    auto result = asset::LoadGLB(*world, filePath);
    if (result.success) {
        // 将所有导入实体设为根节点
        for (auto& e : result.entities) {
            sg->SetParent(e, {kInvalidEntity});
        }
        HE_CORE_INFO("Imported {} entities from {}", result.meshCount, filePath);
    } else {
        HE_CORE_ERROR("Failed to import GLB: {}", result.error);
    }
}

} // namespace he::editor
