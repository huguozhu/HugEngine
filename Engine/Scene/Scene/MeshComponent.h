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
    bool   castShadow        = true;             // 是否投射阴影（false 的网格不进入阴影贴图，如光源可视化球）
    u8     alphaMode         = 0;                // AlphaMode: 0=Opaque, 1=Mask, 2=Blend
    u32    materialID        = 0;                // Bindless 纹理数组基索引

    // --- Disney principled BSDF / 折射扩展参数 ---
    // 默认值还原 glTF metallic-roughness 的行为（等价于「没有扩展」），
    // 由 glTF 的 KHR_materials_* 扩展填充；光栅化侧打包成 GPUObjectData 的
    // disneyA/disneyB/disneyC，路径追踪侧随 PathPayload 传给 ClosestHit。
    float  ior                = 1.5f;            // 电介质折射率（F0 = (ior-1)²/(ior+1)²）
    float  anisotropic        = 0.0f;            // 各向异性强度（0=各向同性）
    float  subsurface         = 0.0f;            // 次表面散射混合（0=纯 Lambert）
    float  specular           = 0.5f;            // 镜面强度（0.5 → F0=0.04）
    float3 specularTint       = float3(1.0f);    // 镜面色调
    float  sheen              = 0.0f;            // 光泽强度（天鹅绒边缘）
    float  clearcoat          = 0.0f;            // 清漆层强度
    float  clearcoatGloss     = 1.0f;            // 清漆光泽度（1=光滑，由粗糙度取反而来）
    float  transmission       = 0.0f;            // 透射（0=不透明；>0 走折射，见 PT 任务 4）
    // glTF KHR_materials_volume（参与介质）：Beer-Lambert 吸收
    float3 attenuationColor    = float3(1.0f);   // 体积吸收色（1=不吸收）
    float  attenuationDistance = 0.0f;           // 吸收特征距离（0 = 不衰减，对应 glTF 的 +inf）

    // --- 纹理路径 ---
    String baseColorTexture;            // 基础色纹理
    String normalTexture;               // 法线贴图
    String metallicRoughnessTexture;    // 金属度+粗糙度纹理
    String occlusionTexture;            // 环境光遮蔽纹理
    String emissiveTexture;             // 自发光纹理

protected:
    // 派生组件（SkeletalMeshComponent 等）直接管理自有顶点布局的缓冲
    std::unique_ptr<rhi::IRHIBuffer> m_VertexBuffer;
    std::unique_ptr<rhi::IRHIBuffer> m_IndexBuffer;
    u32 m_VertexCount = 0;
    u32 m_IndexCount  = 0;
    AABB m_Bounds;
};

} // namespace he
