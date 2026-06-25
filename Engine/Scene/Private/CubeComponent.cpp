// ============================================================
// CubeComponent.cpp — 立方体几何生成
// ============================================================

#include "Scene/CubeComponent.h"

namespace he {

void CubeComponent::OnCreate() {
    MeshComponent::OnCreate();

    TArray<StaticVertex> verts;
    TArray<u32>          indices;
    float h = halfExtent;

    // 6 个面：+Z, -Z, +X, -X, +Y, -Y
    struct Face { float3 n, t, b; };
    Face faces[6] = {
        {{ 0, 0, 1}, {1,0,0}, {0,1,0}},   // +Z
        {{ 0, 0,-1}, {-1,0,0},{0,1,0}},   // -Z
        {{ 1, 0, 0}, {0,0,-1},{0,1,0}},   // +X
        {{-1, 0, 0}, {0,0,1}, {0,1,0}},   // -X
        {{ 0, 1, 0}, {1,0,0}, {0,0,-1}},  // +Y
        {{ 0,-1, 0}, {1,0,0}, {0,0,1}},   // -Y
    };

    for (auto& f : faces) {
        u32 base = static_cast<u32>(verts.size());
        float3 center = f.n * h;
        float3 corners[4] = {
            center + (-f.t - f.b) * h,
            center + ( f.t - f.b) * h,
            center + ( f.t + f.b) * h,
            center + (-f.t + f.b) * h,
        };
        for (u32 i = 0; i < 4; ++i)
            verts.push_back({ corners[i], f.n, float2(i % 2 == 1 ? 1.0f : 0.0f, i / 2 == 1 ? 1.0f : 0.0f) });
        u32 idx[6] = { base, base+1, base+2, base, base+2, base+3 };
        for (u32 i = 0; i < 6; ++i) indices.push_back(idx[i]);
    }

    SetMeshData(verts, indices);
}

} // namespace he
