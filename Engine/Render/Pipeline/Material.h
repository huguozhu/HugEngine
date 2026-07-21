#pragma once

#include "Core/Types.h"
#include "Math/Math.h"
#include "RHI/Types.h"   // kMaxFramesInFlight

// ============================================================
// Material.h — glTF 2.0 PBR 材质 + GPU 共享结构体引用
//
// GPU 结构体定义统一在 ShaderTypes.slang（C++/Slang 共用），
// 此文件仅保留 CPU 端材质类型和常量。
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
// GPU 常量
// ============================================================
static constexpr u32 MAX_LIGHTS          = 8;
static constexpr u32 MAX_SHADOWS         = 4;
static constexpr u32 MAX_FRAMES_IN_FLIGHT = rhi::kMaxFramesInFlight;
static constexpr u32 MAX_OBJECTS          = 1024;
static constexpr u32 CASCADE_COUNT        = 3;

// ============================================================
// GPU 结构体 — 来自 ShaderTypes.slang（C++/Slang 共享定义）
// 修改布局时请编辑 ShaderTypes.slang，两边自动同步
// ============================================================
#include "ShaderTypes.slang"

// 尺寸验证（保持与 ShaderTypes.slang 一致）
static_assert(sizeof(GPUShadowData)   == 256, "GPUShadowData must be 256 bytes");
static_assert(sizeof(GPULight)        == 64,  "GPULight must be 64 bytes");
static_assert(sizeof(GPUObjectData)   == 128, "GPUObjectData must be 128 bytes");
static_assert(sizeof(PushConstantData) == 128, "PushConstantData must be 128 bytes");
static_assert(sizeof(ShadowPushConstant) == 80, "ShadowPushConstant must be 80 bytes");

// ============================================================
// glTF 2.0 PBR 材质（CPU 端资产数据）
// ============================================================
struct PBRMaterial {
    float4   baseColorFactor       = float4(1.0f);
    float3   emissiveFactor        = float3(0.0f);
    float    metallicFactor        = 1.0f;
    float    roughnessFactor       = 1.0f;
    float    aoFactor              = 1.0f;
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

// 填充 GPUObjectData（每帧上传到 Storage Buffer）
inline void FillObjectData(GPUObjectData& obj, const PBRMaterial& mat) {
    obj.baseColorFactor = mat.baseColorFactor;
    obj.emissiveFactor  = float4(mat.emissiveFactor, 0.0f);
    obj.metallicFactor  = mat.metallicFactor;
    obj.roughnessFactor = mat.roughnessFactor;
    obj.aoFactor        = mat.aoFactor;
    obj.alphaCutoff     = mat.alphaCutoff;
    u32 flags = MF_None;
    if (mat.doubleSided)  flags |= MF_DoubleSided;
    if (mat.alphaMode == AlphaMode::Mask) flags |= MF_AlphaMask;
    if (mat.unlit)        flags |= MF_Unlit;
    obj.materialFlags = flags;
}

} // namespace he::render
