#pragma once

#include "Core/Types.h"
#include "Math/Math.h"

// ============================================================
// Material.h — glTF 2.0 PBR 材质 + Light + Push Constant
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
// GPU Light 结构（Storage Buffer 上传，glsl std430 对齐）
// 每灯光 64 字节，最多 MAX_LIGHTS（8）个
// ============================================================
static constexpr u32 MAX_LIGHTS = 8;

struct alignas(16) GPULight {
    float4 colorIntensity;    // xyz=颜色, w=强度
    float4 directionType;     // xyz=方向, w=类型 (0=Dir,1=Point,2=Spot)
    float4 positionRange;     // xyz=位置, w=范围
    float2 coneAngles;        // x=内锥角, y=外锥角 (Spot 专用)
    float2 _pad;
};

static_assert(sizeof(GPULight) == 64, "GPULight must be 64 bytes");

// ============================================================
// Push Constant 数据（每物体，192 字节）
// 光照数据已移至 Storage Buffer（Descriptor Set）
// ============================================================
struct alignas(16) PushConstantData {
    float4x4 modelMatrix;           // [0..64]
    float4x4 viewProjMatrix;        // [64..128]
    float4   baseColorFactor;       // [128..144]
    float    metallicFactor;        // [144]
    float    roughnessFactor;       // [148]
    float    aoFactor;              // [152]
    float    alphaCutoff;           // [156]
    float4   emissiveFactor;        // [160..176]
    float4   cameraPosition;        // [176..192]
    u32      lightCount;            // [192]
    u32      materialFlags;         // [196]
    u32      _pad[14];              // [200..256]
};

static_assert(sizeof(PushConstantData) == 256,
    "PushConstantData must be 256 bytes");

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
