// ============================================================
// PhysicalSkyComponent.cpp — 物理天空组件实现
// ============================================================

#include "Scene/PhysicalSkyComponent.h"

namespace he {

void PhysicalSkyComponent::OnCreate() {
    sunDirection = glm::normalize(sunDirection);
}

} // namespace he
