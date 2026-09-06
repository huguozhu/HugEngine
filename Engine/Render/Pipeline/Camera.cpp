#include "Pipeline/Camera.h"
#include "Scene/CameraComponent.h"
#include "Scene/Transform.h"
#include "Scene/World.h"
#include "Core/Log.h"

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

CameraData ResolveFrameCamera(he::World& world, const CameraData& fallback) {
    // 主相机实体优先：isMain 的 CameraComponent + 其 Transform 组装渲染相机
    he::CameraComponent* cam = world.GetPrimaryCamera();
    if (cam) {
        auto* xform = world.GetComponent<he::TransformComponent>(cam->GetEntity());
        if (xform) return MakeCameraData(*cam, *xform);
        // 相机实体缺 Transform（数据异常）：降级回退，保证渲染不中断
        HE_CORE_WARN("[Camera] 主相机实体缺少 TransformComponent，回退自由相机");
    }
    // 无主相机实体：回退自由相机（CameraController 等外部控制器）
    return fallback;
}

} // namespace he::render
