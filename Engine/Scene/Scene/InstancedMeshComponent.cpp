// ============================================================
// InstancedMeshComponent.cpp — 实例化网格（内置单位立方体 + 变换管理）
// ============================================================

#include "Scene/InstancedMeshComponent.h"

namespace he {

void InstancedMeshComponent::OnCreate() {
    MeshComponent::OnCreate();

    // 单位立方体（halfExtent=0.5 约定，与 CubeComponent 几何一致，便于实例缩放矩阵）
    TArray<StaticVertex> verts;
    TArray<u32>          indices;
    float h = 0.5f;
    struct Face { float3 n, t, b; };
    Face faces[6] = {
        {{ 0, 0, 1}, {1,0,0}, {0,1,0}},
        {{ 0, 0,-1}, {-1,0,0},{0,1,0}},
        {{ 1, 0, 0}, {0,0,-1},{0,1,0}},
        {{-1, 0, 0}, {0,0,1}, {0,1,0}},
        {{ 0, 1, 0}, {1,0,0}, {0,0,-1}},
        {{ 0,-1, 0}, {1,0,0}, {0,0,1}},
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

void InstancedMeshComponent::SetInstanceTransforms(std::vector<float4x4> transforms) {
    instanceTransforms = std::move(transforms);
    bTransformsDirty = true;   // 渲染管线下一帧重建 GPU 实例缓冲
}

} // namespace he
