// Panels/LevelLoader.h — Level 资产加载/卸载
#pragma once

#include "Core/Types.h"
#include "Scene/Entity.h"

namespace he {
    class World;
    class LevelComponent;
}

namespace he::editor {

// ============================================================
// LevelLoader — LevelComponent 的加载/卸载逻辑
//
// 遍历 World 中所有 LevelComponent，对未展开的调用 LoadLevel，
// 对已展开但路径变化的重新加载。
// ============================================================
class LevelLoader {
public:
    /// 同步所有 LevelComponent（加载新的，卸载过期的）
    static void SyncAll(World& world);

    /// 加载单个 Level（读 .hescene → 创建子实体 → 挂到 Entity）
    static void LoadLevel(World& world, LevelComponent& lc);

    /// 卸载单个 Level（销毁所有子实体）
    static void UnloadLevel(World& world, LevelComponent& lc);
};

} // namespace he::editor
