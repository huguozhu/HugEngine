#pragma once

#include "Core/Types.h"

// ============================================================
// MovementSystem — 角色移动驱动（静态系统，仿 AgentSystem）
//
// 每帧由调用方驱动一次：
//   1. 水平速度 = 输入方向 × (跑/走速度)
//   2. 跳跃（仅地面起跳，边沿触发消费 bWantsJump）
//   3. 重力积分（空中）
//   4. 位置积分
//   5. 地面检测：向下射线（CollisionSystem，排除自身）→ 贴地吸附/落地
// ============================================================

namespace he {
class World;

class MovementSystem {
public:
    /// 驱动所有 CharacterMovementComponent 一帧
    static void Update(World& world, f32 dt);
};

} // namespace he
