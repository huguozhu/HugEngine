#pragma once

// ============================================================
// Physics/RigidBodyComponent.h — 刚体组件（纯数据，不包含任何 Jolt 类型）
//
// 与 CollisionComponent.shape 枚举对齐：0=Sphere 1=Box 2=Capsule。
// 由 PhysicsSystem（T4）负责在物理世界创建/同步对应 Jolt Body。
// 放在 Physics 模块（与 AI 模块的 Agent 组件同模式），保证 Scene 层不依赖 Jolt 头。
// ============================================================

#include "Scene/Component.h"

namespace he {

class RigidBodyComponent : public he::Component {
    HE_COMPONENT()
public:
    u8    shape          = 0;              // 0=Sphere 1=Box 2=Capsule（与 CollisionComponent.shape 对齐）
    float radius         = 0.5f;           // Sphere/Capsule 半径
    float halfExtent     = 0.5f;           // Box 半边长
    float height         = 1.0f;           // Capsule 高度（含端盖）

    bool  isDynamic      = true;           // true=动态刚体；false=静态（如地面/障碍）
    float mass           = 1.0f;           // 质量（kg）
    float friction       = 0.6f;           // 摩擦系数
    float restitution    = 0.1f;           // 弹性系数
    float linearDamping  = 0.05f;          // 线性阻尼
    float angularDamping = 0.05f;          // 角阻尼

    bool  enabled        = true;           // false=移除/冻结对应 body（由 PhysicsSystem 处理）
};

} // namespace he
