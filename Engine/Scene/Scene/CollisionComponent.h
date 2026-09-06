#pragma once

#include "Scene/Component.h"
#include "Math/Math.h"

// ============================================================
// CollisionComponent — 基础碰撞体积（对应 UE5 UCapsuleComponent/UBoxComponent）
//
// 纯数据组件：AABB / Sphere / Capsule（垂直，沿 Transform 上方向）三种形状，
// 世界空间位置/朝向取自实体 TransformComponent。
// 检测统一走 CollisionSystem（Overlap / Contains / Raycast）。
// 渲染调试线框预留（后续接 Billboard/线框 mesh）。
//
// 形状尺寸语义：
//   AABB   : halfExtents 三轴半尺寸（局部空间）
//   Sphere : radius 半径
//   Capsule: radius 半径 + height 总高（含两端半球，段半长 = max(0, height/2 - radius)）
// MVP 约定：AABB 旋转取世界矩阵重轴（保守），半径统一乘 Transform 最大缩放分量。
// ============================================================

namespace he {

/// 碰撞形状类型
enum class CollisionShape : u8 {
    AABB    = 0,   // 轴对齐盒（局部空间）
    Sphere  = 1,   // 球体
    Capsule = 2,   // 垂直胶囊（沿 Transform 上方向）
};

class CollisionComponent : public Component {
    HE_COMPONENT()
public:
    CollisionShape shape       = CollisionShape::AABB;   // 形状类型
    float3 halfExtents = float3(0.5f);                   // AABB 三轴半尺寸（米）
    float  radius       = 0.5f;                          // Sphere/Capsule 半径（米）
    float  height       = 1.0f;                          // Capsule 总高（米，含两端半球）
    bool   bEnabled     = true;                          // 是否参与碰撞检测
};

} // namespace he
