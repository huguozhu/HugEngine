#pragma once

// ============================================================
// Scene/NavAgentSystem.h — 寻路代理驱动（C4）
//
// 每帧由调用方驱动：对带 NavAgentComponent 的实体
//  · 目标变化/初始 → NavMeshSystem::FindPath 求路径
//  · 沿路径点移动（朝向当前路径点）
//  · 到达/不可达 → bReached
// ============================================================

#include "Core/Types.h"

namespace he {
class World;
class SceneGraph;
}

namespace he {

class NavAgentSystem {
public:
    static void Update(World& world, SceneGraph& sg, f32 dt);
};

} // namespace he
