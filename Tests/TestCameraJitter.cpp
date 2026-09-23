// ============================================================
// Tests/TestCameraJitter.cpp — TAA 子像素抖动接进投影矩阵的符号/量级单元测试
//                               （2026-09 画质阶段 0 第②项）
//
// 覆盖范围（**纯 CPU、无 RHI**）：
//   1. 抖动 δ（NDC）经 `CameraData::jitterNdc` 后，同一点的 NDC 恰好平移 **+δ**；
//   2. 该平移**与深度无关**（近处/远处两点的 NDC 增量相同）—— 这是"把 δ 加在投影矩阵的
//      m[2][0]/m[2][1]（w 系数）而不是 m[0][0]（x 缩放）"的关键，加错位置会变成透视缩放；
//   3. `jitterNdc = 0` 时投影矩阵与直接调 glm 参考实现**逐元素相同**（零抖动=零行为变化，
//      这是"TAA 关闭档与修复前逐位一致"这条验收口径的代码级依据）。
//
// 为什么能脱离 RHI：`Pipeline/Camera.h` 只依赖 Core/Types.h 与 Math/（同 `GI/RSMFrustum.h`
// 的理由），故本文件只需把 `Engine/Render` 加进 include 路径，不必链接 HugEngineRender。
// ============================================================

#include "doctest.h"

#include "Pipeline/Camera.h"

using namespace he;
using namespace he::render;

namespace {

/// 视图空间一点在给定相机下的 NDC（裁剪空间除以 w）
float2 NdcOf(const CameraData& cam, const float3& viewPos) {
    const float4 clip = cam.GetProjMatrix() * float4(viewPos, 1.0f);
    return float2(clip.x / clip.w, clip.y / clip.w);
}

/// 一个确定性的标准相机（RH 视图空间：可见点 z < 0）
CameraData MakeCam() {
    CameraData cam;
    cam.position    = float3(0.0f, 0.0f, 0.0f);
    cam.forward     = float3(0.0f, 0.0f, -1.0f);
    cam.up          = float3(0.0f, 1.0f, 0.0f);
    cam.fov         = 60.0f;
    cam.nearPlane   = 0.1f;
    cam.farPlane    = 2000.0f;
    cam.aspectRatio = 16.0f / 9.0f;
    return cam;
}

} // namespace

TEST_CASE("CameraData::jitterNdc 恰好把 NDC 平移 +jitter，且与深度无关") {
    const CameraData cam = MakeCam();

    // 取 Halton 表里真实会出现的两个值（0.25 px / -0.375 px 对应的 NDC 量级）
    const float2 jitter(0.25f, -0.375f);
    CameraData jittered = cam;
    jittered.jitterNdc = jitter;

    // 近处与远处两点：NDC 的**增量**必须都等于 jitter（屏幕空间常量偏移，不是透视变形）
    const float3 nearV( 0.30f,  0.20f,  -1.5f);
    const float3 farV (-1.20f,  0.60f, -80.0f);
    for (const float3& v : { nearV, farV }) {
        const float2 n0 = NdcOf(cam, v);
        const float2 n1 = NdcOf(jittered, v);
        CHECK(n1.x - n0.x == doctest::Approx(jitter.x).epsilon(1e-5));
        CHECK(n1.y - n0.y == doctest::Approx(jitter.y).epsilon(1e-5));
    }
}

TEST_CASE("CameraData::jitterNdc 不改变视图矩阵与近/远平面（只有 x/y 的 NDC 被平移）") {
    const CameraData cam = MakeCam();
    CameraData jittered = cam;
    jittered.jitterNdc = float2(0.25f, -0.375f);

    // 视图矩阵逐元素相同（抖动只动投影）
    const float4x4 v0 = cam.GetViewMatrix();
    const float4x4 v1 = jittered.GetViewMatrix();
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            CHECK(v0[c][r] == v1[c][r]);

    // 近/远平面（planes[4] / planes[5]）逐元素不变：抖动加在投影矩阵的 x/y 行上，
    // 不参与深度映射。左右/上下平面**会**随抖动倾斜 —— 这正是"视锥随采样相位一致平移"
    // 的效果，故不作为不变量（剔除因此仍与渲染同相位）。
    const Frustum f0 = cam.GetFrustum();
    const Frustum f1 = jittered.GetFrustum();
    for (int p : {4, 5}) {
        for (int k = 0; k < 4; ++k)
            CHECK(f0.planes[p][k] == doctest::Approx(f1.planes[p][k]).epsilon(1e-6));
    }
}

TEST_CASE("CameraData::jitterNdc = 0 时投影矩阵与 glm 参考实现逐元素相同") {
    const CameraData cam = MakeCam();
    const float4x4 p0 = cam.GetProjMatrix();

    CameraData same = cam;
    same.jitterNdc = float2(0.0f, 0.0f);
    const float4x4 p1 = same.GetProjMatrix();

    const float4x4 ref = glm::perspectiveRH_ZO(
        glm::radians(cam.fov), cam.aspectRatio, cam.nearPlane, cam.farPlane);

    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            CHECK(p0[c][r] == p1[c][r]);
            CHECK(p0[c][r] == ref[c][r]);
        }
    }
}
