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

namespace he { class CameraComponent; class TransformComponent; class World; }

namespace he::render {

struct CameraData {
    float3  position    = float3(0.0f, 5.0f, 10.0f);  // 相机世界坐标
    float3  forward     = float3(0.0f, 0.0f, -1.0f);  // 视线方向（单位向量）
    float3  up          = float3(0.0f, 1.0f, 0.0f);   // 上方向

    float   fov         = kDefaultFOV;          // 垂直视场角（度）
    float   nearPlane   = kDefaultNearPlane;    // 近裁剪面
    float   farPlane    = kDefaultFarPlane;     // 远裁剪面
    float   aspectRatio = 16.0f / 9.0f;         // 宽高比

    // ── 物理相机推导值（可选，由 PhysicalCamera 填充）──
    float   exposureBias          = 0.0f;   // 曝光偏置（EV 偏移，叠加到 AutoExposure）
    float   apertureDiameter      = 0.0f;   // 光圈孔径直径, mm（0=不使用物理 DOF）
    float   focusDistance         = 5.0f;   // 对焦距离, 世界单位
    float   motionBlurIntensity   = 0.5f;   // 运动模糊强度（物理相机可覆盖）
    float   maxCoC                = 0.03f;  // 最大弥散圆直径（屏幕空间比例）

    // ── TAA 子像素抖动（NDC 空间，2026-09 画质阶段 0 第②项）──
    //   由渲染管线每帧注入（`DeferredPipeline::BuildFrameGraph` 帧首：从 `AA_TAA::GetJitterOffset()`
    //   取当前帧的 Halton 偏移）。**0 = 不抖动**（TAA 关闭/未接线时的默认值）。
    //   它只参与投影矩阵，**不参与**视图矩阵与 `GetFrustum()` 之外的任何几何：
    //     · 阴影（CSM/Spot/Point）用未抖动的相机做级联拟合 ⇒ 阴影贴图不随抖动摇摆；
    //     · 其余（GBuffer/速度/光照重建/屏幕空间 GI/天空盒/光追）全部用同一份带抖动的投影
    //       ⇒ 深度、法线、运动矢量与光照重建处于**同一相位**，这正是 TAA 收敛的前提。
    float2  jitterNdc             = float2(0.0f);

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
        float4x4 p = glm::perspectiveRH_ZO(
            glm::radians(fov),
            aspectRatio,
            nearPlane,
            farPlane
        );
        // ── TAA 抖动注入 ──
        // 目标：把裁剪空间 x/y 平移一个**常量 NDC 偏移** δ（不论深度）。对 glm 的列主序矩阵
        //   clip.x = m[0][0]·x + m[2][0]·z ，  clip.w = m[2][3]·z = -z（RH：可见点 z<0）
        // 给 m[2][0] 加上 Δ 会让 clip.x 增加 Δ·z，于是 NDC.x 变化 -Δ；故取 Δ = -δ 即可让
        // NDC 平移 **+δ**。y 同理（m[2][1]）。
        // 这一条与 `Tests/TestCameraJitter.cpp` 的断言一一对应（防符号约定被改错）。
        if (jitterNdc.x != 0.0f || jitterNdc.y != 0.0f) {
            p[2][0] -= jitterNdc.x;
            p[2][1] -= jitterNdc.y;
        }
        return p;
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

/// 帧入口相机解析（S0.4 主相机接入）：
/// 优先取 World 主相机实体（isMain 的 CameraComponent + 其 Transform）组装 CameraData；
/// 无主相机实体（或缺 Transform）时回退 fallback（如 CameraController 的自由相机）。
/// 各渲染管线帧入口用本函数替代直接传 camCtrl.GetCamera()。
CameraData ResolveFrameCamera(he::World& world, const CameraData& fallback);

} // namespace he::render
