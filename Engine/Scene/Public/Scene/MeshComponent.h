#pragma once

#include "Scene/Component.h"
#include "RHI/Buffer.h"
#include "Math/Geometry.h"
#include "Containers/Array.h"

#include <memory>

// ============================================================
// MeshComponent — 网格渲染组件
//
// 存储顶点/索引缓冲引用 + 包围盒
// ============================================================

namespace he {

/// 静态网格顶点（Phase 1: 仅位置）
struct StaticVertex {
    float3 position;
    float3 normal;
    float2 uv;
};

class MeshComponent : public Component {
    HE_COMPONENT()
public:
    /// 设置网格数据（由 Asset 加载器调用）
    void SetMeshData(
        const TArray<StaticVertex>& vertices,
        const TArray<u32>& indices
    );

    /// 获取顶点/索引缓冲
    std::unique_ptr<rhi::IRHIBuffer>& GetVertexBuffer() { return m_VertexBuffer; }
    std::unique_ptr<rhi::IRHIBuffer>& GetIndexBuffer()  { return m_IndexBuffer; }

    u32 GetIndexCount()  const { return m_IndexCount; }
    u32 GetVertexCount() const { return m_VertexCount; }

    /// 包围盒
    AABB GetBounds() const { return m_Bounds; }

    // 材质
    float4 baseColorFactor = float4(1.0f);
    String baseColorTexture; // 纹理路径（后续用 Asset 系统加载）

private:
    std::unique_ptr<rhi::IRHIBuffer> m_VertexBuffer;
    std::unique_ptr<rhi::IRHIBuffer> m_IndexBuffer;
    u32 m_VertexCount = 0;
    u32 m_IndexCount  = 0;
    AABB m_Bounds;
};

} // namespace he
