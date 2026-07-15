#pragma once

#include "Scene/Component.h"
#include "RHI/Buffer.h"
#include "Math/Geometry.h"
#include "Containers/Array.h"

#include <memory>

// ============================================================
// MeshComponent — 网格渲染组件
//
// 存储顶点/索引缓冲引用 + 包围盒 + glTF 2.0 PBR 材质
// ============================================================

namespace he {

/// 静态网格顶点
struct StaticVertex {
    float3 position;
    float3 normal;
    float2 uv;
};

class MeshComponent : public Component {
    HE_COMPONENT()
public:
    /// 设置网格数据（由 Asset 加载器或 Shape 组件调用）
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

    // --- glTF 2.0 PBR 材质参数 ---
    float4 baseColorFactor   = float4(1.0f);     // 基础色 RGBA
    float3 emissiveFactor    = float3(0.0f);     // 自发光 RGB
    float  metallicFactor    = 0.0f;             // 金属度 [0,1]
    float  roughnessFactor   = 0.8f;             // 粗糙度 [0.04,1]
    float  aoFactor          = 1.0f;             // 环境光遮蔽强度
    float  alphaCutoff       = 0.5f;             // Alpha 截断阈值
    bool   doubleSided       = false;            // 双面渲染
    bool   unlit             = false;            // 无光照模式
    u8     alphaMode         = 0;                // AlphaMode: 0=Opaque, 1=Mask, 2=Blend
    u32    materialID        = 0;                // Bindless 纹理数组基索引

    // --- 纹理路径 ---
    String baseColorTexture;            // 基础色纹理
    String normalTexture;               // 法线贴图
    String metallicRoughnessTexture;    // 金属度+粗糙度纹理
    String occlusionTexture;            // 环境光遮蔽纹理
    String emissiveTexture;             // 自发光纹理

private:
    std::unique_ptr<rhi::IRHIBuffer> m_VertexBuffer;
    std::unique_ptr<rhi::IRHIBuffer> m_IndexBuffer;
    u32 m_VertexCount = 0;
    u32 m_IndexCount  = 0;
    AABB m_Bounds;
};

} // namespace he
