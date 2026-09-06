// ============================================================
// BillboardComponent.cpp — 广告牌四边形生成 + billboard 矩阵
// ============================================================

#include "Scene/BillboardComponent.h"

namespace he {

void BillboardComponent::OnCreate() {
    MeshComponent::OnCreate();

    // 单位四边形（XY 平面，法线 +Z，尺寸由 MakeBillboardMatrix 的缩放列决定）
    TArray<StaticVertex> verts;
    TArray<u32>          indices;
    verts.push_back({ {-0.5f, -0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f} });
    verts.push_back({ { 0.5f, -0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f} });
    verts.push_back({ { 0.5f,  0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f} });
    verts.push_back({ {-0.5f,  0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f} });
    indices = { 0, 1, 2, 0, 2, 3 };
    SetMeshData(verts, indices);

    // 广告牌默认渲染方式：无光照 + 半透明混合 + 双面 + 不投影（贴片类对象惯例）
    unlit       = true;
    alphaMode   = 2;   // AlphaMode::Blend
    doubleSided = true;
    castShadow  = false;
}

float4x4 BillboardComponent::MakeBillboardMatrix(const float3& position,
                                                 const float3& cameraForward,
                                                 const float3& cameraUp,
                                                 const float2& size) {
    // 相机右轴（右手系：right = cross(forward, up)），与 CameraData::GetViewMatrix 一致
    float3 right = glm::normalize(glm::cross(cameraForward, cameraUp));
    float3 up    = glm::normalize(cameraUp);
    // 法线朝向相机（+Z = −forward，使四边形正面朝向相机）
    float3 normal = -glm::normalize(cameraForward);

    float4x4 m(1.0f);
    m[0] = float4(right * size.x, 0.0f);
    m[1] = float4(up * size.y, 0.0f);
    m[2] = float4(normal, 0.0f);
    m[3] = float4(position, 1.0f);
    return m;
}

} // namespace he
