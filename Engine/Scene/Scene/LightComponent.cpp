// ============================================================
// LightComponent.cpp — 光源组件实现
// ============================================================

#include "Scene/LightComponent.h"

namespace he {

void DirectionalLight::OnCreate() {
    LightComponent::OnCreate();
    type = LightType::Directional;
    direction = glm::normalize(direction);
}

void PointLight::OnCreate() {
    LightComponent::OnCreate();
    type = LightType::Point;
}

void SpotLight::OnCreate() {
    LightComponent::OnCreate();
    type = LightType::Spot;
    direction = glm::normalize(direction);
}

} // namespace he
