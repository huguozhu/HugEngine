#pragma once

// ============================================================
// Physics/PhysicsSystem.h — 刚体物理系统（静态入口，仿 AgentSystem）
//
// 每帧由调用方驱动一次；内部：
//   1. 遍历带 RigidBodyComponent 的实体 → 无 body 则按 Transform/形状创建 Jolt Body
//   2. 固定步长 Step（accumulator = 1/120s，单帧步数上限防 spiral）
//   3. 每步后把激活 body 的世界变换回写实体 TransformComponent
// 组件放 Physics 模块（与 AI Agent 组件同模式），Scene 层不依赖 Jolt。
// ============================================================

#include "Core/Types.h"
#include "Scene/Entity.h"

namespace he {
class World;
class SceneGraph;
}

namespace he::physics {

class PhysicsSystem {
public:
    /// 每帧调用；内部固定步积累加器 + Entity↔Body 双向同步
    static void Update(he::World& world, he::SceneGraph& sg, f32 dt);

    /// 供管线/工具查询：某实体是否已在物理世界
    static bool HasBody(he::World& world, he::Entity e);

    /// 当前物理世界中激活（移动中）的刚体数量（ImGui 调试显示用）
    static int GetActiveBodyCount();
};

} // namespace he::physics
