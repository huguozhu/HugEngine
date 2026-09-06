// ============================================================
// DecalComponent.cpp — 贴花四边形生成
// ============================================================

#include "Scene/DecalComponent.h"

#include <algorithm>
#include <cmath>

namespace he {

void DecalComponent::OnCreate() {
    MeshComponent::OnCreate();

    // 单位四边形按 size 缩放、绕法线（+Z）旋转 rotation（弧度）
    const float hw = size.x * 0.5f;
    const float hh = size.y * 0.5f;
    const float c  = std::cos(rotation);
    const float s  = std::sin(rotation);
    // 四角（逆时针旋转 rotation）
    auto rot = [&](float x, float y) { return float2(x * c - y * s, x * s + y * c); };

    float2 p0 = rot(-hw, -hh), p1 = rot(hw, -hh), p2 = rot(hw, hh), p3 = rot(-hw, hh);

    TArray<StaticVertex> verts;
    TArray<u32>          indices;
    verts.push_back({ {p0.x, p0.y, 0.0f}, {0, 0, 1}, {0, 0} });
    verts.push_back({ {p1.x, p1.y, 0.0f}, {0, 0, 1}, {1, 0} });
    verts.push_back({ {p2.x, p2.y, 0.0f}, {0, 0, 1}, {1, 1} });
    verts.push_back({ {p3.x, p3.y, 0.0f}, {0, 0, 1}, {0, 1} });
    indices = { 0, 1, 2, 0, 2, 3 };
    SetMeshData(verts, indices);

    // 贴花默认渲染方式：无光照 + 半透明混合 + 双面 + 不投影
    unlit       = true;
    alphaMode   = blendMode;
    doubleSided = true;
    castShadow  = false;
    // 不透明度 → 材质 alpha
    baseColorFactor = float4(baseColorFactor.x, baseColorFactor.y, baseColorFactor.z,
                             std::clamp(opacity, 0.0f, 1.0f));
    // 纹理路径 → baseColor 槽（textureMask bit0）
    baseColorTexture = decalTexture;
}

} // namespace he
