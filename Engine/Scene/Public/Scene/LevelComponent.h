#pragma once

#include "Scene/Component.h"
#include "Core/Types.h"
#include "Containers/Array.h"

// ============================================================
// LevelComponent — 可复用的场景块（类 UE5 Level Instance）
//
// 引用一个 Level 资产文件（.hescene），加载后可将子实体
// 展开为当前 Entity 的子节点，支持多实例放置。
// ============================================================

namespace he {

class LevelComponent : public Component {
    HE_COMPONENT()
public:
    /// Level 资产路径
    String levelPath;

    /// 是否已展开
    bool IsExpanded() const { return m_Expanded; }
    void SetExpanded(bool e) { m_Expanded = e; }

    /// 展开后创建的子实体列表
    const TArray<Entity>& GetChildren() const { return m_Children; }
    TArray<Entity>& GetChildren() { return m_Children; }

private:
    bool m_Expanded = false;
    TArray<Entity> m_Children;
    friend class LevelLoader;  // 由 Editor 层加载逻辑访问
};

} // namespace he
