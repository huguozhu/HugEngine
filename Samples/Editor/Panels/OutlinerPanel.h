// Samples/Editor/Panels/OutlinerPanel.h
#pragma once

// ============================================================
// OutlinerPanel — 场景层级树面板
//
// 显示 World 中所有实体的层级结构。
// 点击实体 -> EditorContext::SelectEntity()。
// 支持搜索过滤和右键菜单。
// ============================================================

#include "Core/Types.h"
#include "Scene/Entity.h"
#include "Containers/Array.h"

#include <unordered_map>

namespace he::editor {
    class EditorContext;
}

namespace he::editor {

class OutlinerPanel {
public:
    void Initialize(EditorContext* ctx) { m_Ctx = ctx; }

    /// 渲染场景层级树（每帧由 EditorApp 调用）
    void Render();

    /// 设置搜索过滤文本
    void SetFilter(const char* filter) { m_Filter = filter; }

private:
    /// 递归渲染实体及其子节点（childrenMap 预计算，O(1) 查找子节点）
    void RenderEntity(he::Entity entity, int depth,
                      const std::unordered_map<he::EntityID, TArray<he::Entity>>& childrenMap);

    EditorContext* m_Ctx    = nullptr;
    String         m_Filter;  // 搜索过滤文本
};

} // namespace he::editor
