#include "Pipeline/PhysicalCamera.h"
#include <glm/gtc/constants.hpp>
#include <cmath>

namespace he::render {

// ============================================================
// 焦距 → FOV 转换
// fov = 2 × atan(sensorHeight / (2 × focalLength))
// ============================================================
float FocalLengthToFOV(float focalLength, float sensorHeight) {
    if (focalLength <= 0.0f || sensorHeight <= 0.0f) return 60.0f;  // 回退默认
    float halfSensor = sensorHeight * 0.5f;
    float halfFovRad = std::atan(halfSensor / focalLength);
    return glm::degrees(halfFovRad * 2.0f);
}

// ============================================================
// 光圈孔径直径
// diameter = focalLength / fStop
// ============================================================
float ApertureDiameter(float focalLength, float fStop) {
    if (fStop <= 0.0f) return 0.0f;
    return focalLength / fStop;
}

// ============================================================
// 曝光偏置（EV 值偏移）
// EV = log₂(N² / t × 100 / ISO)
// 基准: f/2.8, 1/60s, ISO100 → N²/t=470.4, ×100/ISO=1.0 → EV≈log₂(470.4)≈8.88
// 偏置 = 当前EV - 基准EV
// ============================================================
float ExposureBias(float aperture, float shutterSpeed, float iso) {
    if (shutterSpeed <= 0.0f || iso <= 0.0f) return 0.0f;

    // 计算当前曝光值
    float N_sq = aperture * aperture;           // N² (f-stop²)
    float luxSensitivity = 100.0f / iso;         // 感光度因子
    float ev = std::log2(N_sq / shutterSpeed * luxSensitivity);

    // 基准 EV: f/2.8, 1/60s, ISO100
    constexpr float kRefN = 2.8f * 2.8f;        // ≈7.84
    constexpr float kRefT = 1.0f / 60.0f;       // 0.0167
    constexpr float kRefISO = 100.0f;
    float refEV = std::log2(kRefN / kRefT * (100.0f / kRefISO));

    // 偏置 = 基准 - 当前（正值=需要更多曝光补偿, 负值=需要减少曝光）
    return refEV - ev;
}

// ============================================================
// 运动模糊强度
// 强度 = shutterSpeed / frameTime
// 1/60s快门@60fps → 1.0 (一帧曝光, 正常运动模糊)
// 1/30s快门@60fps → 2.0 (两帧曝光, 更强烈的运动模糊)
// 1/125s快门@60fps → 0.48 (半帧曝光, 弱运动模糊)
// ============================================================
float MotionBlurIntensity(float shutterSpeed, float frameTime) {
    if (frameTime <= 0.0f) return 0.0f;
    return shutterSpeed / frameTime;
}

// ============================================================
// 最大弥散圆直径（屏幕空间比例）
// 简化薄透镜模型:
//   CoC_world = apertureDiameter × |focusDist - depth| / max(depth, 0.001f)
// 归一化到屏幕空间: CoC_screen = CoC_world / sensorHeight
// 钳制到合理范围 [0.005, 0.05]
// ============================================================
float MaxCoC(float apertureDiameter, float sensorHeight) {
    if (sensorHeight <= 0.0f) return 0.03f;  // 回退默认
    // 标准场景假设: 对焦距离 ~5m, 最大偏离 ~95m
    float refDepth = 5.0f;
    float maxDepth = 100.0f;
    float cocWorld = apertureDiameter * std::abs(refDepth - maxDepth) / std::max(maxDepth, 0.001f);
    float cocScreen = cocWorld / sensorHeight;
    return glm::clamp(cocScreen, 0.005f, 0.05f);
}

// ============================================================
// 从物理参数推导所有渲染参数
// ============================================================
PhysicalCameraDerived DerivePhysicalCamera(
    const PhysicalCameraParams& params,
    float aspectRatio,
    float frameTime) {

    PhysicalCameraDerived d;

    // FOV: 焦距 + 传感器尺寸 → 视场角
    d.fov = FocalLengthToFOV(params.focalLength, params.sensor.height);

    // 光圈孔径直径
    d.apertureDiameter = ApertureDiameter(params.focalLength, params.fStop);

    // 曝光偏置
    d.exposureBias = ExposureBias(params.fStop, params.shutterSpeed, params.iso);

    // 运动模糊强度
    d.motionBlurIntensity = MotionBlurIntensity(params.shutterSpeed, frameTime);

    // 对焦距离（直接映射）
    d.focusDistance = params.focusDistance;

    // 最大弥散圆（基于光圈孔径 + 传感器尺寸）
    d.maxCoC = MaxCoC(d.apertureDiameter, params.sensor.height);

    return d;
}

} // namespace he::render
