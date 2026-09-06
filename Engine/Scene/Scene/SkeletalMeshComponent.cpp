// ============================================================
// SkeletalMeshComponent.cpp — 骨骼网格（蒙皮顶点上传 + 剪辑播放）
// ============================================================

#include "Scene/SkeletalMeshComponent.h"
#include "RHI/RHI.h"
#include "Core/Log.h"

namespace he {

void SkeletalMeshComponent::SetSkeleton(std::shared_ptr<asset::SkeletonAsset> skel) {
    skeleton = std::move(skel);
    jointWorldMatrices.assign(skeleton ? skeleton->joints.size() : 0, float4x4(1.0f));
    currentClip = -1;
    clipTime    = 0.0f;

    // 蒙皮顶点布局与 StaticVertex 不同（附加 JOINTS/WEIGHTS），
    // 直接以原始字节上传 GPU 顶点缓冲（布局见 SkinnedVertex 与 PBR.vert 的 location 3/4）
    if (!skeleton || skeleton->vertices.empty()) {
        m_VertexCount = 0;
        m_IndexCount  = 0;
        return;
    }
    m_VertexCount = static_cast<u32>(skeleton->vertices.size());
    m_IndexCount  = static_cast<u32>(skeleton->indices.size());

    // 包围盒（绑定姿势，纯 CPU）
    m_Bounds = AABB();
    for (auto& v : skeleton->vertices) m_Bounds.Expand(v.position);

    if (auto* device = rhi::GetDevice()) {
        rhi::BufferDesc vbDesc;
        vbDesc.size        = skeleton->vertices.size() * sizeof(asset::SkinnedVertex);
        vbDesc.usage       = rhi::BufferUsage::Vertex | rhi::BufferUsage::AccelerationStruct;
        vbDesc.initialData = skeleton->vertices.data();
        vbDesc.stride      = sizeof(asset::SkinnedVertex);
        m_VertexBuffer = device->CreateBuffer(vbDesc);

        if (!skeleton->indices.empty()) {
            rhi::BufferDesc ibDesc;
            ibDesc.size        = skeleton->indices.size() * sizeof(u32);
            ibDesc.usage       = rhi::BufferUsage::Index | rhi::BufferUsage::AccelerationStruct;
            ibDesc.initialData = skeleton->indices.data();
            ibDesc.stride      = sizeof(u32);
            m_IndexBuffer = device->CreateBuffer(ibDesc);
        }
        HE_CORE_INFO("[SkeletalMesh] 上传蒙皮网格: {} 顶点, {} 索引（{} 关节）",
            m_VertexCount, m_IndexCount, skeleton->joints.size());
    }
}

void SkeletalMeshComponent::PlayClip(i32 clipIndex, bool loop) {
    if (!skeleton || clipIndex < 0 || clipIndex >= static_cast<i32>(skeleton->clips.size())) {
        currentClip = -1;   // 越界/无剪辑 → 绑定姿势
        return;
    }
    currentClip = clipIndex;
    clipTime    = 0.0f;
    looping     = loop;
    playing     = true;
}

} // namespace he
