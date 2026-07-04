#include "Pipeline/Camera.h"
#include "Scene/CameraComponent.h"
#include "Scene/Transform.h"

// ============================================================
// CameraData 工具函数实现
// ============================================================

namespace he::render {

CameraData MakeCameraData(const he::CameraComponent& camComp,
                          const he::TransformComponent& transform) {
    CameraData cd;
    cd.position    = transform.position;
    cd.forward     = transform.GetForward();
    cd.up          = transform.GetUp();
    cd.fov         = camComp.fov;
    cd.nearPlane   = camComp.nearPlane;
    cd.farPlane    = camComp.farPlane;
    cd.aspectRatio = camComp.aspectRatio;
    return cd;
}

} // namespace he::render
