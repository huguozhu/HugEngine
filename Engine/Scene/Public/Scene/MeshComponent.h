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

    // --- 纹理设置（外部加载后注入，指针由调用方管理生命周期）---
    void SetBaseColorTexture(rhi::IRHITexture* tex, rhi::IRHISampler* sampler) {
        m_BaseColorGPUTex     = tex;
        m_BaseColorGPUSampler = sampler;
    }
    void SetNormalTexture(rhi::IRHITexture* tex, rhi::IRHISampler* sampler) {
        m_NormalGPUTex     = tex;
        m_NormalGPUSampler = sampler;
    }
    void SetMetallicRoughnessTexture(rhi::IRHITexture* tex, rhi::IRHISampler* sampler) {
        m_MetallicRoughnessGPUTex     = tex;
        m_MetallicRoughnessGPUSampler = sampler;
    }
    void SetOcclusionTexture(rhi::IRHITexture* tex, rhi::IRHISampler* sampler) {
        m_OcclusionGPUTex     = tex;
        m_OcclusionGPUSampler = sampler;
    }

    rhi::IRHITexture* GetBaseColorGPUTexture() const        { return m_BaseColorGPUTex; }
    rhi::IRHISampler* GetBaseColorGPUSampler() const        { return m_BaseColorGPUSampler; }
    rhi::IRHITexture* GetNormalGPUTexture() const           { return m_NormalGPUTex; }
    rhi::IRHISampler* GetNormalGPUSampler() const           { return m_NormalGPUSampler; }
    rhi::IRHITexture* GetMetallicRoughnessGPUTexture() const { return m_MetallicRoughnessGPUTex; }
    rhi::IRHISampler* GetMetallicRoughnessGPUSampler() const { return m_MetallicRoughnessGPUSampler; }
    rhi::IRHITexture* GetOcclusionGPUTexture() const        { return m_OcclusionGPUTex; }
    rhi::IRHISampler* GetOcclusionGPUSampler() const        { return m_OcclusionGPUSampler; }

    // 描述符集（包含该 primitive 的纹理绑定，渲染时直接 bind 无需 update）
    void SetDescriptorSet(rhi::DescriptorSetHandle h) { m_DescSetHandle = h; }
    rhi::DescriptorSetHandle GetDescriptorSet() const { return m_DescSetHandle; }

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

    // --- 纹理路径（Phase 2 中期通过 Bindless 采样）---
    String baseColorTexture;            // 基础色纹理
    String normalTexture;               // 法线贴图
    String metallicRoughnessTexture;    // 金属度+粗糙度纹理
    String occlusionTexture;            // 环境光遮蔽纹理
    String emissiveTexture;             // 自发光纹理

private:
    std::unique_ptr<rhi::IRHIBuffer> m_VertexBuffer;
    std::unique_ptr<rhi::IRHIBuffer> m_IndexBuffer;
    rhi::IRHITexture* m_BaseColorGPUTex             = nullptr;
    rhi::IRHISampler* m_BaseColorGPUSampler          = nullptr;
    rhi::IRHITexture* m_NormalGPUTex                = nullptr;
    rhi::IRHISampler* m_NormalGPUSampler             = nullptr;
    rhi::IRHITexture* m_MetallicRoughnessGPUTex      = nullptr;
    rhi::IRHISampler* m_MetallicRoughnessGPUSampler  = nullptr;
    rhi::IRHITexture* m_OcclusionGPUTex             = nullptr;
    rhi::IRHISampler* m_OcclusionGPUSampler          = nullptr;
    rhi::DescriptorSetHandle m_DescSetHandle = rhi::kInvalidSet;
    u32 m_VertexCount = 0;
    u32 m_IndexCount  = 0;
    AABB m_Bounds;
};

} // namespace he
