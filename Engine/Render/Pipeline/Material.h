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
static constexpr u32 MAX_LIGHTS          = 8;
static constexpr u32 MAX_SHADOWS         = 4;   // 最多 4 个投射阴影的光源
static constexpr u32 MAX_FRAMES_IN_FLIGHT = 3;  // 三缓冲帧环

// CSM 级联数
static constexpr u32 CASCADE_COUNT = 3;

// --- GPU 阴影数据（Storage Buffer，每个阴影投射光源一条）---
// CSM: 3 级联 lightViewProj + 4 分割深度
struct alignas(16) GPUShadowData {
    float4x4 lightViewProj[CASCADE_COUNT];  // [0..192] 方向光 CSM 级联矩阵
    float4   shadowParams;    // [192..208] x=bias, y=normalBias, z=strength, w=类型
    float4   splitDistances;  // [208..224] CSM 分割点
    float4   cameraForward;   // [224..240] 相机前向(级联选择)
    float4   pointLightData;  // [240..256] xyz=点光位置, w=范围
};
static_assert(sizeof(GPUShadowData) == 256, "GPUShadowData must be 256 bytes");

struct alignas(16) GPULight {
    float4 colorIntensity;    // xyz=颜色, w=强度
    float4 directionType;     // xyz=方向, w=类型 (0=Dir,1=Point,2=Spot)
    float4 positionRange;     // xyz=位置, w=范围
    float2 coneAngles;        // x=内锥角, y=外锥角 (Spot 专用)
    i32    shadowIndex;       // -1=无阴影, >=0 指向 u_ShadowData 数组
    i32    _pad;
};

static_assert(sizeof(GPULight) == 64, "GPULight must be 64 bytes");

// ============================================================
// GPU 对象数据（Storage Buffer，每物体 128 字节）
// Shader 通过 objectIndex 索引，替代 PushConstant 传递
// ============================================================
static constexpr u32 MAX_OBJECTS = 1024;

struct alignas(16) GPUObjectData {
    float4x4 worldMatrix;           // [0..64]   世界矩阵
    float4   baseColorFactor;       // [64..80]  基础色
    float4   emissiveFactor;        // [80..96]  自发光
    float    metallicFactor;        // [96]
    float    roughnessFactor;       // [100]
    float    aoFactor;              // [104]
    float    alphaCutoff;           // [108]
    u32      materialFlags;         // [112]
    u32      materialID;            // [116]     bindless 纹理索引
    u32      _pad[2];               // [120..128] 对齐
};
static_assert(sizeof(GPUObjectData) == 128, "GPUObjectData must be 128 bytes");

// ============================================================
// Push Constant 数据（帧级数据，约 96 字节）
// 光照数据已移至 Storage Buffer（Descriptor Set）
// ============================================================
struct alignas(16) PushConstantData {
    float4x4 viewProjMatrix;        // [0..64]
    float4   cameraPosition;        // [64..80]
    u32      lightCount;            // [80]
    u32      objectIndex;           // [84]
    float    iblIntensity;          // [88] IBL 强度倍率（0=关闭）
    u32      _pad;                  // [92..96]
};

static_assert(sizeof(PushConstantData) == 96,
    "PushConstantData must be 96 bytes");

// CSM 级联选择用的 cameraForward 移至 GPUShadowData.shadowParams.w

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
