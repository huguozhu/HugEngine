#pragma once

#include "Scene/Component.h"
#include "Math/Math.h"

// ============================================================
// CameraComponent — 相机组件
//
// 存储投影参数（FOV/裁剪面/宽高比）。位置和朝向从
// TransformComponent 获取，通过 render::MakeCameraData()
// 转换为渲染管线使用的 CameraData。
//
// 用法：
//   Entity cam = world.CreateEntity("MainCamera");
//   world.AddComponent<TransformComponent>(cam);
//   auto* cc = world.AddComponent<CameraComponent>(cam);
//   cc->fov = 60.0f; cc->isMain = true;
//   // 渲染时：
//   auto* t = world.GetComponent<TransformComponent>(cam);
//   auto cd = render::MakeCameraData(*cc, *t);
// ============================================================

namespace he {

class CameraComponent : public Component {
    HE_COMPONENT()
public:
    // --- 投影参数 ---
    float fov         = 60.0f;              // 垂直视场角（度）
    float nearPlane   = 0.1f;               // 近裁剪面
    float farPlane    = 2000.0f;            // 远裁剪面
    float aspectRatio = 16.0f / 9.0f;       // 宽高比

    // --- 主相机标记（场景中多个相机时标识激活相机）---
    bool isMain = true;

    // --- 便捷方法 ---
    void SetAspectFromWindow(u32 width, u32 height) {
        aspectRatio = static_cast<float>(width) / static_cast<float>(height);
    }
};

} // namespace he
