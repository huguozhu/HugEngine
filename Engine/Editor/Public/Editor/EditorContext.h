#pragma once

#include "Core/Types.h"
#include "Containers/Array.h"
#include "Scene/Entity.h"

#include <functional>

namespace he {
    class World;
    class SceneGraph;
    class CommandHistory;
}

namespace he::editor {

// ============================================================
// EditorContext — 编辑器运行时共享上下文
//
// 所有面板通过 EditorContext 读写编辑器状态，
// 避免面板间直接耦合。选中变化通过回调通知面板刷新。
// ============================================================
class EditorContext {
public:
    EditorContext() = default;

    /// 绑定引擎对象（World/SceneGraph 不由此类拥有）
    void Initialize(he::World* world, he::SceneGraph* sg, he::CommandHistory* cmdHistory);

    // --- 选中管理 ---
    void SelectEntity(he::Entity e);
    void DeselectAll();
    const TArray<he::Entity>& GetSelection() const { return m_Selection; }
    bool IsSelected(he::Entity e) const;

    // --- 属性编辑（自动包装为 Command，支持 Undo/Redo）---
    template<typename T>
    void SetProperty(he::Entity entity, const char* propName, const T& value);

    // --- 引擎引用 ---
    he::World*          GetWorld()          const { return m_World; }
    he::SceneGraph*     GetSceneGraph()     const { return m_SceneGraph; }
    he::CommandHistory* GetCommandHistory() const { return m_CmdHistory; }

    // --- 回调 ---
    void OnSelectionChanged(std::function<void()> callback) {
        m_SelectionCallbacks.push_back(std::move(callback));
    }

private:
    void NotifySelectionChanged();

    he::World*          m_World      = nullptr;
    he::SceneGraph*     m_SceneGraph = nullptr;
    he::CommandHistory* m_CmdHistory = nullptr;
    TArray<he::Entity>  m_Selection;
    TArray<std::function<void()>> m_SelectionCallbacks;
};

} // namespace he::editor
