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

namespace he {

// --- Component 基类注册 ---
HE_BEGIN_REGISTER(he::Component)
HE_END_REGISTER()

// --- TransformComponent 注册 ---
HE_BEGIN_REGISTER(he::TransformComponent)
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
HE_END_REGISTER()

HE_BEGIN_REGISTER(he::PointLight)
HE_END_REGISTER()

HE_BEGIN_REGISTER(he::SpotLight)
HE_END_REGISTER()

} // namespace he
