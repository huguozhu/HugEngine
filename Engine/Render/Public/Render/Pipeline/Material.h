#pragma once

#include "Core/Types.h"
#include "Math/Math.h"

// ============================================================
// Material.h — PBR 材质数据 + Push Constant 布局
//
// PushConstantData 必须与 GLSL shader 中的 PushConstants
// 布局精确匹配（16 字节对齐约束）。
// ============================================================

namespace he::render {

// --- Push Constant 数据（CPU → GPU 每物体传递）---
// 总大小: 208 字节（需 ≤ 256 字节 Vulkan 最小保证值）
struct alignas(16) PushConstantData {
    // --- 顶点着色器用 ---
    float4x4 modelMatrix;           // [0..64]   世界→裁剪空间变换
    float4x4 viewProjMatrix;        // [64..128] 视图→裁剪空间变换

    // --- 片元着色器用 ---
    float4   baseColorFactor;       // [128..144] 基础色（线性空间 RGB + Alpha）
    float    metallicFactor;        // [144]      金属度 [0,1]
    float    roughnessFactor;       // [148]      粗糙度 [0.04,1]
    float    aoFactor;              // [152]      环境光遮蔽 [0,1]
    float    alphaCutoff;           // [156]      Alpha 截断阈值

    float4   cameraPosition;        // [160..176] 相机世界坐标（xyz）+ 未使用（w）
    float4   lightDirection;        // [176..192] 光线方向（xyz）+ 强度（w）
    float4   lightColor;            // [192..208] 光照颜色（rgb）+ 未使用（w）
};

// 编译期验证大小
static_assert(sizeof(PushConstantData) == 208,
    "PushConstantData must be 208 bytes, matching GLSL PushConstants block");

// --- PBR 材质（CPU 端资产数据）---
struct PBRMaterial {
    float4  baseColorFactor    = float4(1.0f);        // 基础色
    float3  emissiveFactor     = float3(0.0f);        // 自发光色
    float   metallicFactor     = 1.0f;                // 金属度
    float   roughnessFactor    = 1.0f;                // 粗糙度
    float   aoFactor           = 1.0f;                // 环境光遮蔽
    float   alphaCutoff        = 0.5f;                // Alpha 截断阈值
    bool    doubleSided        = false;               // 双面渲染
    bool    alphaMask          = false;               // 启用 Alpha Mask

    // 纹理文件路径（Phase 2 中期启用纹理采样）
    String   baseColorTexture;         // 基础色纹理路径
    String   normalTexture;            // 法线纹理路径
    String   metallicRoughnessTexture; // 金属度+粗糙度纹理路径
    String   aoTexture;                // AO 纹理路径
    String   emissiveTexture;          // 自发光纹理路径
};

// --- 默认材质（灰色 PBR）---
inline PBRMaterial GetDefaultMaterial() {
    PBRMaterial mat;
    mat.baseColorFactor = float4(0.8f, 0.8f, 0.8f, 1.0f);
    mat.metallicFactor  = 0.0f;
    mat.roughnessFactor = 0.8f;
    mat.aoFactor        = 1.0f;
    return mat;
}

} // namespace he::render
