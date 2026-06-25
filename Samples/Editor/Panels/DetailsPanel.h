// Samples/Editor/Panels/DetailsPanel.h
#pragma once

// ============================================================
// DetailsPanel — 选中对象属性检查器
//
// 显示 EditorContext 当前选中实体的所有组件及其属性。
// 通过 ReflectionAPI 枚举属性，按类型渲染编辑器控件。
// ============================================================

#include "Core/Types.h"

namespace he {
    class World;
    struct Entity;
}
namespace he::editor {
    class EditorContext;
}

namespace he::editor {

class DetailsPanel {
public:
    void Initialize(EditorContext* ctx);

    /// 渲染属性面板（每帧调用）
    void Render();

private:
    /// 渲染单个 Transform 属性
    void RenderTransform(he::World* world, he::Entity entity);

    /// 渲染单个 Mesh 材质属性
    void RenderMesh(he::World* world, he::Entity entity);

    /// 渲染单个 Light 属性
    void RenderLight(he::World* world, he::Entity entity);

    EditorContext* m_Ctx = nullptr;
};

} // namespace he::editor
