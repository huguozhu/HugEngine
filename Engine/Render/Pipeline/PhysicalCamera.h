#pragma once

#include "Core/Types.h"
#include "Math/Math.h"

// ============================================================
// PhysicalCamera.h — 基于物理的相机模型
//
// PhysicalCameraParameters: 真实世界相机参数（焦距/光圈/快门/ISO/传感器尺寸）
// 自动推导出渲染所需的 FOV、景深参数、运动模糊强度、曝光偏置
//
// 物理模型:
//   FOV = 2 × atan(sensorHeight / (2 × focalLength))     — 焦距+传感器→视场角
//   EV = log₂(aperture² / shutterSpeed × 100 / ISO)       — 曝光三角形
//   CoC = f² × abs(focusDist - depth) / (depth × N × (focusDist - f))  — 薄透镜弥散圆
//   blurIntensity = shutterSpeed / frameTime               — 快门速度→运动模糊
//
// 预设传感器尺寸 (宽×高, mm):
//   全画幅 36×24, APS-C 23.6×15.6, M4/3 17.3×13.0
// ============================================================

namespace he::render {

// ── 传感器尺寸预设 ──
struct SensorSize {
    float width;   // mm
    float height;  // mm
    const char* name;

    static constexpr SensorSize FullFrame() { return {36.0f, 24.0f, "全画幅"}; }
    static constexpr SensorSize APSC()      { return {23.6f, 15.6f, "APS-C"}; }
    static constexpr SensorSize Micro43()   { return {17.3f, 13.0f, "M4/3"}; }
};

// ── 物理相机参数 ──
struct PhysicalCameraParams {
    // 镜头
    float focalLength    = 50.0f;   // 焦距, mm（默认 50mm 标准镜）
    float fStop          = 2.8f;    // 光圈 f-stop（默认 f/2.8）

    // 快门与感光度
    float shutterSpeed   = 1.0f / 60.0f;  // 快门速度, 秒（默认 1/60s）
    float iso            = 100.0f;         // ISO 感光度（默认 ISO 100）

    // 传感器
    SensorSize sensor    = SensorSize::FullFrame();  // 传感器尺寸

    // 对焦
    float focusDistance  = 5.0f;    // 对焦距离, 世界单位（默认 5m）
    float nearPlane      = 0.1f;    // 近裁剪面
    float farPlane       = 2000.0f; // 远裁剪面
};

// ── 物理相机推导出的渲染参数 ──
struct PhysicalCameraDerived {
    float fov;                    // 垂直视场角（度）
    float apertureDiameter;       // 光圈孔径直径, mm
    float exposureBias;           // 曝光偏置（EV 偏移，叠加到 AutoExposure）
    float motionBlurIntensity;    // 运动模糊强度（0=无模糊, 1=1帧快门）
    float focusDistance;          // 对焦距离（世界单位）
    float maxCoC;                 // 最大弥散圆直径（屏幕空间比例）
};

// ── 物理相机 API ──

// 从物理参数推导所有渲染参数
// aspectRatio: 渲染目标宽高比
// frameTime: 帧时间, 秒（用于运动模糊强度计算）
PhysicalCameraDerived DerivePhysicalCamera(
    const PhysicalCameraParams& params,
    float aspectRatio,
    float frameTime = 1.0f / 60.0f);

// 将物理相机参数转换为渲染用 FOV（垂直）
// fov = 2 × atan(sensorHeight / (2 × focalLength))
float FocalLengthToFOV(float focalLength, float sensorHeight);

// 计算光圈孔径直径, mm
float ApertureDiameter(float focalLength, float fStop);

// 计算曝光偏置（EV 值）
// 参照标准 EV 公式: EV = log₂(N² / t × 100 / ISO)
// 基准：f/2.8, 1/60s, ISO100 → EV≈0
float ExposureBias(float aperture, float shutterSpeed, float iso);

// 计算运动模糊强度
// 强度 = shutterSpeed / frameTime (≥1.0 时多帧曝光)
float MotionBlurIntensity(float shutterSpeed, float frameTime);

// 计算最大弥散圆直径（屏幕空间比例）
// 简化薄透镜模型: apertureDiameter × |focusDist - depth| / depth
// 归一化到屏幕空间比例
float MaxCoC(float apertureDiameter, float sensorHeight);

} // namespace he::render
