#pragma once

#include "Core/Types.h"
#include "Math/Math.h"

// ============================================================
// Material.h — glTF 2.0 PBR 材质 + Push Constant 布局
// ============================================================

namespace he::render {

// --- Alpha 混合模式（glTF 2.0 alphaMode）---
enum class AlphaMode : u8 {
    Opaque = 0,  // 不透明（默认）
    Mask   = 1,  // Alpha 截断测试
    Blend  = 2,  // 半透明混合（暂未实现）
};

// --- 材质标志位（GPU push constant）---
enum MaterialFlags : u32 {
    MF_None          = 0,
    MF_DoubleSided   = 1 << 0,  // 双面渲染（禁用背面剔除）
    MF_AlphaMask     = 1 << 1,  // 启用 Alpha 截断测试
    MF_Unlit         = 1 << 2,  // 无光照（KHR_materials_unlit）
};

// --- Push Constant 数据（CPU → GPU 每物体传递）---
// 总大小: 240 字节（≤ 256 字节 Vulkan 最小保证值）
struct alignas(16) PushConstantData {
    // --- 顶点着色器用 ---
    float4x4 modelMatrix;           // [0..64]   模型矩阵
    float4x4 viewProjMatrix;        // [64..128] 视图投影矩阵

    // --- 片元着色器用 ---
    float4   baseColorFactor;       // [128..144] 基础色（RGBA，线性空间）
    float    metallicFactor;        // [144]      金属度 [0,1]
    float    roughnessFactor;       // [148]      粗糙度 [0.04,1]
    float    aoFactor;              // [152]      环境光遮蔽 [0,1]
    float    alphaCutoff;           // [156]      Alpha 截断阈值

    float4   emissiveFactor;        // [160..176] 自发光色（RGB）+ 未使用（A）
    float4   cameraPosition;        // [176..192] 相机世界坐标（xyz）
    float4   lightDirection;        // [192..208] 光线方向（xyz）+ 强度（w）
    float4   lightColor;            // [208..224] 光照颜色（rgb）+ 未使用（w）

    u32      materialFlags;         // [224]      材质标志位（MaterialFlags 位掩码）
    u32      _pad[3];               // [228..240] 对齐填充
};

static_assert(sizeof(PushConstantData) == 240,
    "PushConstantData must be 240 bytes");

// --- glTF 2.0 PBR 材质（CPU 端完整资产数据）---
struct PBRMaterial {
    // PBR 因子
    float4   baseColorFactor       = float4(1.0f);     // 基础色 RGBA
    float3   emissiveFactor        = float3(0.0f);     // 自发光 RGB
    float    metallicFactor        = 1.0f;             // 金属度
    float    roughnessFactor       = 1.0f;             // 粗糙度
    float    aoFactor              = 1.0f;             // 环境光遮蔽强度
    float    alphaCutoff           = 0.5f;             // Alpha 截断阈值

    // Alpha 模式
    AlphaMode alphaMode            = AlphaMode::Opaque;
    bool      doubleSided          = false;            // 双面渲染
    bool      unlit                = false;            // 无光照模式

    // 纹理路径（Phase 2 中期通过 Bindless 采样）
    String    baseColorTexture;            // 基础色纹理
    String    normalTexture;               // 法线贴图
    String    metallicRoughnessTexture;    // 金属度+粗糙度（RG: metal, B: roughness）
    String    occlusionTexture;            // 环境光遮蔽纹理
    String    emissiveTexture;             // 自发光纹理

    // 纹理变换（glTF KHR_texture_transform）
    float2    texCoordOffset        = float2(0.0f);
    float2    texCoordScale         = float2(1.0f);
    float     texCoordRotation      = 0.0f;
};

// --- 默认材质（灰色 PBR，glTF 规范默认值）---
inline PBRMaterial GetDefaultMaterial() {
    PBRMaterial mat;
    mat.baseColorFactor = float4(0.8f, 0.8f, 0.8f, 1.0f);
    mat.metallicFactor  = 0.0f;
    mat.roughnessFactor = 0.8f;
    mat.aoFactor        = 1.0f;
    mat.alphaCutoff     = 0.5f;
    return mat;
}

// 填充 PushConstantData 的材质+光照部分
inline void FillMaterialPushConstants(PushConstantData& pc, const PBRMaterial& mat) {
    pc.baseColorFactor = mat.baseColorFactor;
    pc.metallicFactor  = mat.metallicFactor;
    pc.roughnessFactor = mat.roughnessFactor;
    pc.aoFactor        = mat.aoFactor;
    pc.alphaCutoff     = mat.alphaCutoff;
    pc.emissiveFactor  = float4(mat.emissiveFactor, 0.0f);

    u32 flags = MF_None;
    if (mat.doubleSided)  flags |= MF_DoubleSided;
    if (mat.alphaMode == AlphaMode::Mask) flags |= MF_AlphaMask;
    if (mat.unlit)        flags |= MF_Unlit;
    pc.materialFlags = flags;
}

} // namespace he::render
