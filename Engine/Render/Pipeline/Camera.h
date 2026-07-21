#pragma once

#include "Core/Types.h"
#include "Math/Math.h"
#include "Math/Geometry.h"  // Frustum

// ============================================================
// Camera.h — 渲染相机
//
// CameraData: 渲染管线使用的相机数据（视图/投影矩阵 + 视锥体）
// CameraComponent 通过 MakeCameraData() 转换为 CameraData
// ============================================================

namespace he { class CameraComponent; class TransformComponent; }

namespace he::render {

struct CameraData {
    float3  position    = float3(0.0f, 5.0f, 10.0f);  // 相机世界坐标
    float3  forward     = float3(0.0f, 0.0f, -1.0f);  // 视线方向（单位向量）
    float3  up          = float3(0.0f, 1.0f, 0.0f);   // 上方向

    float   fov         = kDefaultFOV;          // 垂直视场角（度）
    float   nearPlane   = kDefaultNearPlane;    // 近裁剪面
    float   farPlane    = kDefaultFarPlane;     // 远裁剪面
    float   aspectRatio = 16.0f / 9.0f;         // 宽高比

    // 视图矩阵（世界空间 → 相机空间）
    float4x4 GetViewMatrix() const {
        float3 f = glm::normalize(forward);
        float3 s = glm::normalize(glm::cross(f, up));
        float3 u = glm::cross(s, f);

        // Vulkan 使用 reverse-Z：近平面=1, 远平面=0
        return glm::lookAtRH(position, position + f, up);
    }

    // 投影矩阵（相机空间 → 裁剪空间，Vulkan [0,1] 深度约定）
    float4x4 GetProjMatrix() const {
        return glm::perspectiveRH_ZO(
            glm::radians(fov),
            aspectRatio,
            nearPlane,
            farPlane
        );
    }

    // 视图-投影合成矩阵
    float4x4 GetViewProjMatrix() const {
        return GetProjMatrix() * GetViewMatrix();
    }

    // 视锥体（6 个平面，用于剔除）
    Frustum GetFrustum() const {
        return Frustum::FromViewProj(GetViewProjMatrix());
    }

    // 设置宽高比基于窗口尺寸
    void SetAspectRatio(float width, float height) {
        aspectRatio = width / height;
    }
};

/// 从 CameraComponent + TransformComponent 构造渲染用 CameraData
CameraData MakeCameraData(const he::CameraComponent& camComp,
                          const he::TransformComponent& transform);

} // namespace he::render
