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

HE_BEGIN_REGISTER(he::SpotLight)
HE_END_REGISTER()

HE_BEGIN_REGISTER(he::SkyboxComponent)
HE_END_REGISTER()

HE_BEGIN_REGISTER(he::PhysicalSkyComponent)
HE_END_REGISTER()

HE_BEGIN_REGISTER(he::CameraComponent)
HE_END_REGISTER()

HE_BEGIN_REGISTER(he::AnimationComponent)
HE_END_REGISTER()

HE_BEGIN_REGISTER(he::LevelComponent)
HE_END_REGISTER()

HE_BEGIN_REGISTER(he::ParticleComponent)
HE_END_REGISTER()

} // namespace he
