// ============================================================
// SphereComponent.cpp — UV 球体几何生成
// ============================================================

#include "Scene/SphereComponent.h"
#include "Math/Math.h"

namespace he {

void SphereComponent::OnCreate() {
    MeshComponent::OnCreate();

    TArray<StaticVertex> verts;
    TArray<u32>          indices;
    u32 segs = segmentCount;
    u32 rings = ringCount;

    // 顶点：逐环 + 逐段生成
    for (u32 ring = 0; ring <= rings; ++ring) {
        float phi = HE_PI * static_cast<float>(ring) / static_cast<float>(rings);  // [0, π]
        float y   = cos(phi) * radius;
        float r   = sin(phi) * radius;

        for (u32 seg = 0; seg <= segs; ++seg) {
            float theta = 2.0f * HE_PI * static_cast<float>(seg) / static_cast<float>(segs);  // [0, 2π]
            float3 pos(r * cos(theta), y, r * sin(theta));
            float3 normal = glm::normalize(pos);
            float2 uv(static_cast<float>(seg) / segs, static_cast<float>(ring) / rings);
            verts.push_back({ pos, normal, uv });
        }
    }

    // 索引：每环两行三角形
    for (u32 ring = 0; ring < rings; ++ring) {
        for (u32 seg = 0; seg < segs; ++seg) {
            u32 a = ring * (segs + 1) + seg;
            u32 b = a + segs + 1;
            u32 c = a + 1;
            u32 d = b + 1;

            indices.push_back(a); indices.push_back(b); indices.push_back(c);
            indices.push_back(c); indices.push_back(b); indices.push_back(d);
        }
    }

    SetMeshData(verts, indices);
}

} // namespace he
