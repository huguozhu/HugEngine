#pragma once

#include "Scene/Component.h"
#include "Scene/Entity.h"
#include "Math/Math.h"

// ============================================================
// SpringArmComponent — 弹簧臂（对应 UE5 USpringArmComponent）
//
// 第三人称相机跟随：每帧由 SpringArmSystem 把
//   目标 Transform + 局部偏移 - 目标前向 × 臂长
// 合成相机位置，写入 cameraEntity 的 TransformComponent
// （该相机实体通常 isMain=true，配合 S0.4 主相机驱动渲染）。
//
// 旋转滞后：rotationLagSpeed > 0 时相机位置/朝向按指数平滑跟随，
// 产生 UE 式"延迟 + 回弹"手感；= 0 时硬跟随（无延迟）。
// 碰撞缩短臂长依赖 Phase B5 Collision，当前 MVP 无碰撞。
// ============================================================

namespace he {

class SpringArmComponent : public Component {
    HE_COMPONENT()
public:
    EntityID targetEntity = kInvalidEntity;   // 跟随目标实体（取其 Transform 作锚点）
    EntityID cameraEntity = kInvalidEntity;   // 相机实体（写入其 TransformComponent）

    float3 targetOffset     = float3(0.0f, 1.5f, 0.0f);  // 锚点偏移（目标局部空间）
    float  armLength        = 5.0f;                      // 弹簧臂长度（米）
    float  rotationLagSpeed = 8.0f;                      // 旋转滞后速度（越大跟随越快；0=硬跟随）
    bool   bUsePawnControlRotation = true;               // true=相机朝向跟随目标（UE 第三人称默认）
};

} // namespace he
