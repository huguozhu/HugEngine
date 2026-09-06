#pragma once

#include "Scene/Component.h"
#include "Scene/Entity.h"
#include "Math/Math.h"

#include <functional>

// ============================================================
// ProjectileMovementComponent — 抛射物运动（对应 UE5 UProjectileMovementComponent）
//
// 数据组件：描述直线/抛物线/追踪运动参数。
// 由 ProjectileSystem::Update 每帧积分位置（初速沿实体朝向），
// 支持重力、最大速度钳制、目标追踪与超时销毁。
// 命中回调 onHit 预留（Phase B5 碰撞系统落地后接入）。
// ============================================================

namespace he {

class ProjectileMovementComponent : public Component {
    HE_COMPONENT()
public:
    // --- 运动参数 ---
    float initialSpeed = 10.0f;               // 初速（米/秒，方向 = 实体前向）
    float maxSpeed     = 0.0f;                // 最大速度（0 = 不限速）
    float gravityScale = 1.0f;                // 重力倍率（0 = 无重力直线运动）
    bool  bHoming      = false;               // 是否追踪 homingTarget
    EntityID homingTarget = kInvalidEntity;   // 追踪目标实体（bHoming 时有效）
    float lifetime     = 5.0f;                // 生命周期（秒，超时自动销毁；0 = 无限）

    // --- 命中回调（预留：Phase B5 Collision 落地后由碰撞检测触发）---
    std::function<void(Entity, const float3&)> onHit;

    // --- 运行时状态（ProjectileSystem 驱动，勿手动改）---
    float3 velocity    = float3(0.0f);   // 当前速度（首帧由 initialSpeed × 前向初始化）
    float  lifeElapsed = 0.0f;           // 已存活时间
    bool   bInitialized = false;         // 首帧初始化标记
};

} // namespace he
