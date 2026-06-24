#pragma once

#include "Reflect/TypeInfo.h"
#include "Core/Types.h"

// ============================================================
// Component — 组件基类
//
// 所有引擎组件继承自此，通过 HE_COMPONENT 宏注册反射。
// 生命周期: Create → Start → Update (每帧) → Destroy
// ============================================================

namespace he {

/// 组件生命周期标志
enum class ComponentState : u8 {
    Created,   // 已创建，未启动
    Active,    // 活跃中
    Destroyed, // 已销毁
};

/// 组件基类
class Component {
    HE_COMPONENT()
public:
    Component() = default;
    virtual ~Component() = default;

    // 生命周期
    virtual void OnCreate()  {}
    virtual void OnStart()   {}
    virtual void OnUpdate(f32 deltaTime) {}
    virtual void OnDestroy() {}

    ComponentState GetState() const { return m_State; }
    bool IsActive()      const { return m_State == ComponentState::Active; }

protected:
    ComponentState m_State = ComponentState::Created;
    friend class World;  // World 管理状态切换
};

} // namespace he
