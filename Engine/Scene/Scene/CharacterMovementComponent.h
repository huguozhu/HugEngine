#pragma once

#include "Scene/Component.h"
#include "Math/Math.h"

// ============================================================
// CharacterMovementComponent — 角色移动（对应 UE5 UCharacterMovementComponent）
//
// 走/跑/跳/重力/地面检测，由 MovementSystem 每帧积分。
// 地面检测用 CollisionSystem::Raycast 向下射线（排除自身碰撞体），
// 需要实体挂 CollisionComponent（胶囊，取 height/2 为半高）。
//
// 输入约定（调用方每帧写入）：
//   inputDirection：水平期望方向（世界空间，XZ 平面）
//   bWantsJump    ：跳一次（边沿触发，系统处理后自动清零）
//   bRunning      ：跑步速度
// MVP 约定：无水平碰撞滑移（可穿墙）、坡度过滤待射线法线支持（maxSlopeAngle 预留）。
// ============================================================

namespace he {

class CharacterMovementComponent : public Component {
    HE_COMPONENT()
public:
    // --- 移动参数 ---
    float walkSpeed     = 1.5f;   // 步行速度（米/秒）
    float runSpeed      = 4.5f;   // 跑步速度（米/秒）
    float jumpHeight    = 0.8f;   // 跳跃高度（米；起跳初速 = √(2·g·h)）
    float gravity       = 9.8f;   // 重力加速度（米/秒²）
    float maxSlopeAngle = 45.0f;  // 最大可站立坡度（度；MVP 预留，坡度过滤待射线法线）

    // --- 每帧输入（调用方写入，系统消费）---
    float2 inputDirection = float2(0.0f);  // 水平期望方向（世界空间 XZ，长度可 >1 自动归一）
    bool   bWantsJump     = false;         // 跳一次（处理后被系统清零）
    bool   bRunning       = false;         // 跑步

    // --- 运行时状态（MovementSystem 驱动，只读）---
    float3 velocity  = float3(0.0f);   // 当前速度（米/秒）
    bool   bOnGround = false;          // 是否站在地面上
};

} // namespace he
