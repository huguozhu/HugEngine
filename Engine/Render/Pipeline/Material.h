#pragma once

#include "Core/Types.h"
#include "Math/Math.h"
#include "RHI/Types.h"   // kMaxFramesInFlight

// ============================================================
// Material.h — glTF 2.0 PBR 材质 + GPU 共享结构体引用
//
// GPU 常量定义在 ShaderTypes.slang 的 ::he::gpu 命名空间中。
// GPU 结构体在 include 时进入 he::render 命名空间。
// ============================================================

namespace he::render {

// --- Alpha 混合模式（glTF 2.0 alphaMode）---
enum class AlphaMode : u8 {
    Opaque = 0,  // 不透明
    Mask   = 1,  // Alpha 截断
    Blend  = 2,  // 半透明混合
};

// --- 材质标志位 ---
enum MaterialFlags : u32 {
    MF_None          = 0,
    MF_DoubleSided   = 1 << 0,
    MF_AlphaMask     = 1 << 1,
    MF_Unlit         = 1 << 2,
};

// ============================================================
// GPU 常量 + 结构体 — ShaderTypes.slang（C++/Slang 共享）
// he::gpu::* 常量（namespace ::he::gpu 绝对路径不受 includer 影响）
// GPU 结构体（进入 he::render 命名空间）
// ============================================================
#include "ShaderTypes.slang"

// ============================================================
// 本地常量别名（引用 ShaderTypes.slang kGPU* 统一定义）
// ============================================================
static constexpr u32 MAX_LIGHTS           = kGPUMaxLights;
static constexpr u32 MAX_SHADOWS          = kGPUMaxShadows;
static constexpr u32 MAX_FRAMES_IN_FLIGHT = rhi::kMaxFramesInFlight;
static constexpr u32 MAX_OBJECTS          = kGPUMaxObjects;
static constexpr u32 kMaxGPUObjects       = kGPUMaxGPUObjects;
static constexpr u32 CASCADE_COUNT        = kGPUCascadeCount;

// 尺寸验证（保持与 ShaderTypes.slang 一致）
static_assert(sizeof(GPUShadowData)   == 256, "GPUShadowData must be 256 bytes");
static_assert(sizeof(GPULight)        == 64,  "GPULight must be 64 bytes");
static_assert(sizeof(GPUObjectData)   == 176, "GPUObjectData must be 176 bytes");
static_assert(sizeof(GPUMaterialData) == 112, "GPUMaterialData must be 112 bytes");
static_assert(sizeof(PushConstantData) == 144, "PushConstantData must be 144 bytes");
static_assert(sizeof(ShadowPushConstant) == 80, "ShadowPushConstant must be 80 bytes");
static_assert(sizeof(RTShadowPushConstant) == 112, "RTShadowPushConstant must be 112 bytes");
static_assert(sizeof(RTRayEffectPushConstant) == 112, "RTRayEffectPushConstant must be 112 bytes");
static_assert(sizeof(PTPushConstant) == 176, "PTPushConstant must be 176 bytes");
static_assert(sizeof(PTReservoir) == 32, "PTReservoir must be 32 bytes");
static_assert(sizeof(ReSTIRPushConstant) == 128, "ReSTIRPushConstant must be 128 bytes");

// ============================================================
// glTF 2.0 PBR 材质（CPU 端资产数据）
// ============================================================
struct PBRMaterial {
    float4   baseColorFactor       = float4(1.0f);
    float3   emissiveFactor        = float3(0.0f);
    float    metallicFactor        = 1.0f;
    float    roughnessFactor       = 1.0f;
    float    aoFactor              = 1.0f;
    float    ior                  = 1.5f;   // 电介质折射率（F0 = (ior-1)^2/(ior+1)^2）
    // ── Disney principled BSDF 扩展参数（默认值还原 glTF metallic/roughness）──
    float    anisotropic       = 0.0f;   // 各向异性强度（0=各向同性）
    float    subsurface        = 0.0f;   // 次表面散射混合（0=纯 Lambert）
    float    specular          = 0.5f;   // 镜面强度（F0 = 0.16 * specular²，0.5→0.04）
    float3   specularTint      = float3(1.0f);  // 镜面色调
    float    sheen             = 0.0f;   // 光泽（天鹅绒边缘）
    float    clearcoat         = 0.0f;   // 清漆层强度
    float    clearcoatGloss    = 1.0f;   // 清漆粗糙度
    float    alphaCutoff           = 0.5f;

    AlphaMode alphaMode            = AlphaMode::Opaque;
    bool      doubleSided          = false;
    bool      unlit                = false;

    String    baseColorTexture;
    String    normalTexture;
    String    metallicRoughnessTexture;
    String    occlusionTexture;
    String    emissiveTexture;

    float2    texCoordOffset        = float2(0.0f);
    float2    texCoordScale         = float2(1.0f);
    float     texCoordRotation      = 0.0f;
};

inline PBRMaterial GetDefaultMaterial() {
    PBRMaterial mat;
    mat.baseColorFactor = float4(0.8f, 0.8f, 0.8f, 1.0f);
    mat.metallicFactor  = 0.0f;
    mat.roughnessFactor = 0.8f;
    mat.aoFactor        = 1.0f;
    return mat;
}

// 根据材质纹理路径计算纹理存在位掩码（bit n = 槽 n 有真实纹理）
inline u32 ComputeMaterialTextureMask(const PBRMaterial& mat) {
    u32 mask = 0;
    if (!mat.baseColorTexture.empty())          mask |= (1u << 0);  // BaseColor 槽
    if (!mat.normalTexture.empty())             mask |= (1u << 1);  // Normal 槽
    if (!mat.metallicRoughnessTexture.empty())  mask |= (1u << 2);  // MetallicRough 槽
    if (!mat.occlusionTexture.empty())          mask |= (1u << 3);  // Occlusion 槽
    return mask;
}

// 填充 GPUObjectData（每帧上传到 Storage Buffer）
inline void FillObjectData(GPUObjectData& obj, const PBRMaterial& mat) {
    obj.baseColorFactor = mat.baseColorFactor;
    obj.emissiveFactor  = float4(mat.emissiveFactor, 0.0f);
    obj.metallicFactor  = mat.metallicFactor;
    obj.roughnessFactor = mat.roughnessFactor;
    obj.aoFactor        = mat.aoFactor;
    // 电介质 F0（标量）：由 IOR 预计算，替代硬编码 0.04
    float ior = mat.ior;
    obj.dielectricF0 = (ior - 1.0f) * (ior - 1.0f) / ((ior + 1.0f) * (ior + 1.0f));
    obj.alphaCutoff     = mat.alphaCutoff;
    u32 flags = MF_None;
    if (mat.doubleSided)  flags |= MF_DoubleSided;
    if (mat.alphaMode == AlphaMode::Mask) flags |= MF_AlphaMask;
    if (mat.unlit)        flags |= MF_Unlit;
    obj.materialFlags = flags;
    obj.textureMask = ComputeMaterialTextureMask(mat);   // 纹理存在位掩码（无纹理槽 shader 不采样）
    // Disney 参数打包（与 ShaderTypes.slang GPUObjectData 布局一致）
    obj.disneyA = float4(mat.anisotropic, mat.subsurface, mat.specular, mat.sheen);
    obj.disneyB = float4(mat.clearcoat, mat.clearcoatGloss, mat.specularTint.x, mat.specularTint.y);
    obj.disneyC = mat.specularTint.z;
}

// 填充 GPUMaterialData（bindless 材质 SSBO 元素，去重后的 per-material 数据）
inline void FillMaterialData(GPUMaterialData& m, const PBRMaterial& mat) {
    m.baseColorFactor = mat.baseColorFactor;
    m.emissiveFactor  = float4(mat.emissiveFactor, 0.0f);
    m.metallicFactor  = mat.metallicFactor;
    m.roughnessFactor = mat.roughnessFactor;
    m.aoFactor        = mat.aoFactor;
    m.alphaCutoff     = mat.alphaCutoff;
    float ior = mat.ior;
    m.dielectricF0 = (ior - 1.0f) * (ior - 1.0f) / ((ior + 1.0f) * (ior + 1.0f));
    u32 flags = MF_None;
    if (mat.doubleSided)  flags |= MF_DoubleSided;
    if (mat.alphaMode == AlphaMode::Mask) flags |= MF_AlphaMask;
    if (mat.unlit)        flags |= MF_Unlit;
    m.materialFlags = flags;
    m.textureMask = ComputeMaterialTextureMask(mat);   // 纹理存在位掩码（无纹理槽 shader 不采样）
    m.disneyA = float4(mat.anisotropic, mat.subsurface, mat.specular, mat.sheen);
    m.disneyB = float4(mat.clearcoat, mat.clearcoatGloss, mat.specularTint.x, mat.specularTint.y);
    m.disneyC = mat.specularTint.z;
    m._pad = 0;
}

} // namespace he::render
