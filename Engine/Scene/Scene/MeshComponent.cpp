// ============================================================
// MeshComponent.cpp — 网格组件实现
// ============================================================

#include "Scene/MeshComponent.h"
#include "RHI/RHI.h"
#include "Core/Log.h"

namespace he {

void MeshComponent::SetMeshData(
    const TArray<StaticVertex>& vertices,
    const TArray<u32>& indices)
{
    m_VertexCount = static_cast<u32>(vertices.size());
    m_IndexCount  = static_cast<u32>(indices.size());

    // 通过 RHI 创建顶点缓冲
    auto* device = rhi::GetDevice();
    if (!device) {
        HE_CORE_ERROR("MeshComponent::SetMeshData: RHI device not available");
        return;
    }

    // 顶点缓冲（含 AccelerationStruct 标志，支持 BLAS 构建）
    rhi::BufferDesc vbDesc;
    vbDesc.size        = vertices.size() * sizeof(StaticVertex);
    vbDesc.usage       = rhi::BufferUsage::Vertex | rhi::BufferUsage::AccelerationStruct;
    vbDesc.initialData = vertices.data();
    vbDesc.stride      = sizeof(StaticVertex);
    m_VertexBuffer = device->CreateBuffer(vbDesc);

    // 索引缓冲（含 AccelerationStruct 标志，支持 BLAS 构建）
    rhi::BufferDesc ibDesc;
    ibDesc.size        = indices.size() * sizeof(u32);
    ibDesc.usage       = rhi::BufferUsage::Index | rhi::BufferUsage::AccelerationStruct;
    ibDesc.initialData = indices.data();
    ibDesc.stride      = sizeof(u32);
    m_IndexBuffer = device->CreateBuffer(ibDesc);

    // 计算包围盒
    m_Bounds = AABB();
    for (auto& v : vertices) {
        m_Bounds.Expand(v.position);
    }

    HE_CORE_INFO("MeshComponent: {} vertices, {} indices, bounds: [{},{},{}]→[{},{},{}]",
        m_VertexCount, m_IndexCount,
        m_Bounds.min.x, m_Bounds.min.y, m_Bounds.min.z,
        m_Bounds.max.x, m_Bounds.max.y, m_Bounds.max.z);
}

} // namespace he
