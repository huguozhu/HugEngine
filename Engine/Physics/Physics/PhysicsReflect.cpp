// ============================================================
// Physics/PhysicsReflect.cpp — 物理组件反射注册（仿 AgentReflect）
//
// 放在 Physics 模块内，避免 Scene → Physics 反向依赖（Scene 不依赖 Jolt）。
// 反射注册后：编辑器 Details 面板、WorldModel 快照、AI 动作（SetProperty）自动支持。
// ============================================================

#include "Physics/RigidBodyComponent.h"

#include "Scene/Component.h"

namespace he {

HE_BEGIN_REGISTER(he::RigidBodyComponent)
    HE_REGISTER_PROPERTY(he::RigidBodyComponent, u8, shape)
        HE_ATTR_CATEGORY("Physics") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("形状（0=球 1=盒 2=胶囊）")
    HE_END_PROPERTY()
    HE_REGISTER_PROPERTY(he::RigidBodyComponent, float, radius)
        HE_ATTR_CATEGORY("Physics") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("球/胶囊半径（米）")
    HE_END_PROPERTY()
    HE_REGISTER_PROPERTY(he::RigidBodyComponent, float, halfExtent)
        HE_ATTR_CATEGORY("Physics") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("盒半边长（米）")
    HE_END_PROPERTY()
    HE_REGISTER_PROPERTY(he::RigidBodyComponent, float, height)
        HE_ATTR_CATEGORY("Physics") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("胶囊高度（米）")
    HE_END_PROPERTY()
    HE_REGISTER_PROPERTY(he::RigidBodyComponent, bool, isDynamic)
        HE_ATTR_CATEGORY("Physics") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("是否动态刚体（false=静态）")
    HE_END_PROPERTY()
    HE_REGISTER_PROPERTY(he::RigidBodyComponent, float, mass)
        HE_ATTR_CATEGORY("Physics") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("质量（kg）")
    HE_END_PROPERTY()
    HE_REGISTER_PROPERTY(he::RigidBodyComponent, float, friction)
        HE_ATTR_CATEGORY("Physics") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("摩擦系数")
    HE_END_PROPERTY()
    HE_REGISTER_PROPERTY(he::RigidBodyComponent, float, restitution)
        HE_ATTR_CATEGORY("Physics") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("弹性系数")
    HE_END_PROPERTY()
    HE_REGISTER_PROPERTY(he::RigidBodyComponent, float, linearDamping)
        HE_ATTR_CATEGORY("Physics") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("线性阻尼")
    HE_END_PROPERTY()
    HE_REGISTER_PROPERTY(he::RigidBodyComponent, float, angularDamping)
        HE_ATTR_CATEGORY("Physics") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("角阻尼")
    HE_END_PROPERTY()
    HE_REGISTER_PROPERTY(he::RigidBodyComponent, bool, enabled)
        HE_ATTR_CATEGORY("Physics") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("是否启用刚体")
    HE_END_PROPERTY()
HE_END_REGISTER()

} // namespace he
