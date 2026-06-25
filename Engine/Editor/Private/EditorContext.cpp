// Engine/Editor/Private/EditorContext.cpp
#include "Editor/EditorContext.h"
#include "Scene/World.h"
#include "Scene/SceneGraph.h"
#include "Editor/Command.h"

namespace he::editor {

void EditorContext::Initialize(World* world, SceneGraph* sg, CommandHistory* cmdHistory) {
    m_World      = world;
    m_SceneGraph = sg;
    m_CmdHistory = cmdHistory;
}

void EditorContext::SelectEntity(Entity e) {
    m_Selection.clear();
    if (e.IsValid() && m_World && m_World->IsValid(e)) {
        m_Selection.push_back(e);
    }
    NotifySelectionChanged();
}

void EditorContext::DeselectAll() {
    m_Selection.clear();
    NotifySelectionChanged();
}

bool EditorContext::IsSelected(Entity e) const {
    for (auto& sel : m_Selection) {
        if (sel == e) return true;
    }
    return false;
}

void EditorContext::NotifySelectionChanged() {
    for (auto& cb : m_SelectionCallbacks) {
        cb();
    }
}

// 属性编辑模板方法 — 构建 PropertyChangeCommand 并推入 Undo 栈
// 注：Phase 3-1 不做完整反射路径的自动属性编辑，
// 具体编辑逻辑在 DetailsPanel 中按已知类型处理。
// SetProperty 模板为 3-2 扩展预留接口。

} // namespace he::editor
