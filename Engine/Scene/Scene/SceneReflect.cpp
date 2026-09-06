// ============================================================
// SceneReflect.cpp — Scene 组件反射注册
//
// 为 HE_COMPONENT() 标记的类提供 StaticClass() 定义
// ============================================================

#include "Scene/Component.h"
#include "Scene/Transform.h"
#include "Scene/MeshComponent.h"
#include "Scene/CubeComponent.h"
#include "Scene/SphereComponent.h"
#include "Scene/LightComponent.h"
#include "Scene/SkyboxComponent.h"
#include "Scene/PhysicalSkyComponent.h"
#include "Scene/CameraComponent.h"
#include "Scene/AnimationComponent.h"
#include "Scene/LevelComponent.h"
#include "Scene/ParticleComponent.h"

namespace he {

// --- Component 基类注册 ---
HE_BEGIN_REGISTER(he::Component)
HE_END_REGISTER()

// --- TransformComponent 注册 ---
// 示范 AI 注解：AI_VISIBLE(进世界模型快照) + AI_WRITABLE(允许 AI 写) + AI_DESCRIPTION(LLM 说明)
HE_BEGIN_REGISTER(he::TransformComponent)
    HE_REGISTER_PROPERTY(he::TransformComponent, float3, position)
        HE_ATTR_CATEGORY("Transform") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("世界空间位置，单位米")
    HE_END_PROPERTY()
    HE_REGISTER_PROPERTY(he::TransformComponent, quat, rotation)
        HE_ATTR_CATEGORY("Transform") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("旋转四元数")
    HE_END_PROPERTY()
    HE_REGISTER_PROPERTY(he::TransformComponent, float3, scale)
        HE_ATTR_CATEGORY("Transform") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("三轴缩放")
    HE_END_PROPERTY()
HE_END_REGISTER()

// --- MeshComponent 注册 ---
HE_BEGIN_REGISTER(he::MeshComponent)
HE_END_REGISTER()

// --- CubeComponent 注册 ---
HE_BEGIN_REGISTER(he::CubeComponent)
HE_END_REGISTER()

// --- SphereComponent 注册 ---
HE_BEGIN_REGISTER(he::SphereComponent)
HE_END_REGISTER()

// --- LightComponent + 子类注册 ---
HE_BEGIN_REGISTER(he::LightComponent)
HE_END_REGISTER()

HE_BEGIN_REGISTER(he::DirectionalLight)
    HE_REGISTER_PROPERTY(he::DirectionalLight, float3, direction)
        HE_ATTR_CATEGORY("Light") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("光线方向（世界空间）")
    HE_END_PROPERTY()
    HE_REGISTER_PROPERTY(he::DirectionalLight, float3, color)
        HE_ATTR_CATEGORY("Light") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("光照颜色 [r,g,b] 0~1")
    HE_END_PROPERTY()
    HE_REGISTER_PROPERTY(he::DirectionalLight, float, intensity)
        HE_ATTR_CATEGORY("Light") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("光照强度（非物理模式乘数）")
    HE_END_PROPERTY()
    HE_REGISTER_PROPERTY(he::DirectionalLight, bool, castShadow)
        HE_ATTR_CATEGORY("Light") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("是否投射阴影")
    HE_END_PROPERTY()
HE_END_REGISTER()

HE_BEGIN_REGISTER(he::PointLight)
    HE_REGISTER_PROPERTY(he::PointLight, float3, color)
        HE_ATTR_CATEGORY("Light") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("光照颜色 [r,g,b] 0~1")
    HE_END_PROPERTY()
    HE_REGISTER_PROPERTY(he::PointLight, float, intensity)
        HE_ATTR_CATEGORY("Light") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("光照强度")
    HE_END_PROPERTY()
    HE_REGISTER_PROPERTY(he::PointLight, float, range)
        HE_ATTR_CATEGORY("Light") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("影响范围，单位米")
    HE_END_PROPERTY()
HE_END_REGISTER()

// --- SpotLight 注册（S0.1：补齐反射 + AI 注解，编辑器 Details 与 LLM 世界模型立即可用）---
// color/intensity/castShadow 为 LightComponent 基类成员，offsetof 对继承公有成员同样有效
HE_BEGIN_REGISTER(he::SpotLight)
    HE_REGISTER_PROPERTY(he::SpotLight, float3, direction)
        HE_ATTR_CATEGORY("Light") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("聚光锥轴方向（世界空间）")
    HE_END_PROPERTY()
    HE_REGISTER_PROPERTY(he::SpotLight, float3, color)
        HE_ATTR_CATEGORY("Light") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("光照颜色 [r,g,b] 0~1")
    HE_END_PROPERTY()
    HE_REGISTER_PROPERTY(he::SpotLight, float, intensity)
        HE_ATTR_CATEGORY("Light") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("光照强度")
    HE_END_PROPERTY()
    HE_REGISTER_PROPERTY(he::SpotLight, float, range)
        HE_ATTR_CATEGORY("Light") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("影响范围（米）")
    HE_END_PROPERTY()
    HE_REGISTER_PROPERTY(he::SpotLight, float, innerConeAngle)
        HE_ATTR_CATEGORY("Light") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("内锥角（弧度，全亮区）")
    HE_END_PROPERTY()
    HE_REGISTER_PROPERTY(he::SpotLight, float, outerConeAngle)
        HE_ATTR_CATEGORY("Light") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("外锥角（弧度，衰减边缘）")
    HE_END_PROPERTY()
    HE_REGISTER_PROPERTY(he::SpotLight, bool, castShadow)
        HE_ATTR_CATEGORY("Light") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("是否投射阴影")
    HE_END_PROPERTY()
HE_END_REGISTER()

// --- RectLight 注册（S0.3 词表对齐：补 color/intensity/castShadow/softness，全部 AI 一等公民）---
HE_BEGIN_REGISTER(he::RectLight)
    HE_REGISTER_PROPERTY(he::RectLight, float, width)
        HE_ATTR_CATEGORY("Light") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("矩形宽度（米）")
    HE_END_PROPERTY()
    HE_REGISTER_PROPERTY(he::RectLight, float, height)
        HE_ATTR_CATEGORY("Light") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("矩形高度（米）")
    HE_END_PROPERTY()
    HE_REGISTER_PROPERTY(he::RectLight, float3, normal)
        HE_ATTR_CATEGORY("Light") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("发光面朝向法线")
    HE_END_PROPERTY()
    HE_REGISTER_PROPERTY(he::RectLight, float3, color)
        HE_ATTR_CATEGORY("Light") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("光照颜色 [r,g,b] 0~1")
    HE_END_PROPERTY()
    HE_REGISTER_PROPERTY(he::RectLight, float, intensity)
        HE_ATTR_CATEGORY("Light") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("光照强度")
    HE_END_PROPERTY()
    HE_REGISTER_PROPERTY(he::RectLight, float, range)
        HE_ATTR_CATEGORY("Light") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("影响范围（米）")
    HE_END_PROPERTY()
    HE_REGISTER_PROPERTY(he::RectLight, float, softness)
        HE_ATTR_CATEGORY("Light") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("软阴影系数 [0,1]（PCF 半径缩放）")
    HE_END_PROPERTY()
    HE_REGISTER_PROPERTY(he::RectLight, bool, castShadow)
        HE_ATTR_CATEGORY("Light") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("是否投射阴影")
    HE_END_PROPERTY()
HE_END_REGISTER()

HE_BEGIN_REGISTER(he::SkyboxComponent)
HE_END_REGISTER()

HE_BEGIN_REGISTER(he::PhysicalSkyComponent)
HE_END_REGISTER()

// --- CameraComponent 注册（S0.2：补齐反射 + AI 注解）---
// isMain 仅 AI_VISIBLE：AI 可读主相机标记但不可改（主相机归属场景作者决定）
HE_BEGIN_REGISTER(he::CameraComponent)
    HE_REGISTER_PROPERTY(he::CameraComponent, float, fov)
        HE_ATTR_CATEGORY("Camera") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("垂直视场角（度，默认 60）")
    HE_END_PROPERTY()
    HE_REGISTER_PROPERTY(he::CameraComponent, float, nearPlane)
        HE_ATTR_CATEGORY("Camera") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("近裁剪面距离")
    HE_END_PROPERTY()
    HE_REGISTER_PROPERTY(he::CameraComponent, float, farPlane)
        HE_ATTR_CATEGORY("Camera") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("远裁剪面距离")
    HE_END_PROPERTY()
    HE_REGISTER_PROPERTY(he::CameraComponent, bool, isMain)
        HE_ATTR_CATEGORY("Camera") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_DESCRIPTION("是否为主相机（场景中仅一个为 true）")
    HE_END_PROPERTY()
HE_END_REGISTER()

HE_BEGIN_REGISTER(he::AnimationComponent)
HE_END_REGISTER()

HE_BEGIN_REGISTER(he::LevelComponent)
HE_END_REGISTER()

HE_BEGIN_REGISTER(he::ParticleComponent)
HE_END_REGISTER()

} // namespace he
