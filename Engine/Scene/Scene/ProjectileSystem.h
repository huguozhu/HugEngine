#pragma once

#include "Core/Types.h"

// ============================================================
// ProjectileSystem — 抛射物运动驱动（静态系统，仿 AgentSystem）
//
// 每帧由调用方（Feature Update / 游戏循环）驱动一次：
//   1. 首帧用 initialSpeed × 实体前向初始化速度
//   2. 追踪目标（bHoming）→ 转向
//   3. 重力积分 + maxSpeed 钳制 + 位置积分
//   4. 生命周期超时 → 帧尾安全销毁（避免迭代中删实体）
// ============================================================

namespace he {
class World;

class ProjectileSystem {
public:
    /// 驱动所有 ProjectileMovementComponent 一帧
    /// @param gravity 重力加速度（米/秒²，默认 9.8，与 UE 一致）
    static void Update(World& world, f32 dt, float gravity = 9.8f);
};

} // namespace he
