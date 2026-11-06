#ifndef HUGENGINE_GPU_CONSTANTS
#define HUGENGINE_GPU_CONSTANTS
// ============================================================
// ShaderTypes/GPUConstants.slang — 引擎上限 / 默认值 / 算法常量（C++/Slang 共享）
//
// 只含常量、不含类型。C++ 侧 Material.h 把 kGPUMax* 别名成 MAX_LIGHTS 等，并断言两侧一致。
// ============================================================

#include "ShaderTypes/GPUDefines.slang"

GPU_CONST_BEGIN

// --- 渲染管线限制 ---
GPU_CONST(GPU_UINT kGPUMaxFramesInFlight       = 3);     // 最大飞行帧数
GPU_CONST(GPU_UINT kGPUMaxColorAttachments      = 8);    // 最大 MRT 附件数
GPU_CONST(GPU_UINT kGPUMaxPushConstantSize      = 256);  // Push Constant 最大字节数
GPU_CONST(GPU_UINT kGPUDefaultPushConstantSize  = 128);  // Push Constant 默认范围
GPU_CONST(GPU_UINT kGPUCubemapFaceCount         = 6);    // 立方体贴图面数
GPU_CONST(GPU_UINT kGPURTMaxRecursionDepth      = 2);    // RT 最大递归深度
GPU_CONST(GPU_UINT kGPURTMaxPayloadSize         = 16);   // RT 最大 Payload 字节数
GPU_CONST(GPU_UINT kGPURTMaxHitAttributeSize    = 8);    // RT 最大 Hit Attribute
GPU_CONST(GPU_UINT kGPUMaxProfilerPasses        = 20);   // Profiler 最大 Pass 数
GPU_CONST(GPU_UINT kGPURTShaderUnused           = ~0u);  // RT 着色器"未绑定"哨兵

// --- 引擎限制（与 Material.h 一致）---
GPU_CONST(GPU_UINT kGPUMaxLights   = 8);     // 最大光源数
GPU_CONST(GPU_UINT kGPUMaxShadows  = 4);     // 最大阴影数
GPU_CONST(GPU_UINT kGPUMaxObjects  = 1024);  // CPU 端 Object 缓冲上限
GPU_CONST(GPU_UINT kGPUMaxGPUObjects = 2048); // GPU Culling 最大物体数
GPU_CONST(GPU_UINT kGPUCascadeCount = 3);     // CSM 级联数

// --- 默认值 ---
GPU_CONST(float kGPUDefaultFOV       = 60.0f);    // 默认垂直视场角
GPU_CONST(float kGPUDefaultNearPlane = 0.1f);     // 默认近裁剪面
GPU_CONST(float kGPUDefaultFarPlane  = 2000.0f);  // 默认远裁剪面
GPU_CONST(GPU_UINT kGPUDefaultShadowMapSize = 2048); // 默认阴影贴图分辨率

// --- 渲染算法常量（C++ / Slang 共享）---
GPU_CONST(GPU_UINT kGPUClusterTileSize    = 64);  // Clustered Shading 屏幕 tile 像素数
GPU_CONST(GPU_UINT kGPUClusterSlices      = 12);  // Clustered Shading Z 切片数
GPU_CONST(float   kGPUPrefilterMips       = 5.0); // IBL 预滤波 mip 层数

// --- 材质纹理槽偏移（每材质占 4 个纹理槽，materialID 为基索引）---
GPU_CONST(GPU_UINT kGPUMaterialTexSlot_BaseColor      = 0);  // 基础色 BaseColor
GPU_CONST(GPU_UINT kGPUMaterialTexSlot_Normal         = 1);  // 法线 Normal
GPU_CONST(GPU_UINT kGPUMaterialTexSlot_MetallicRough  = 2);  // 金属度/粗糙度 MetallicRoughness
GPU_CONST(GPU_UINT kGPUMaterialTexSlot_Occlusion      = 3);  // 环境光遮蔽 Occlusion
GPU_CONST(GPU_UINT kGPUMaterialTexSlot_Count          = 4);  // 每材质纹理槽数（materialID >> 2 的依据）

// --- 纹理存在位掩码（textureMask：bit n = 槽 n 绑定真实纹理）---
// 无纹理的槽不做采样（占位纹理内容不可信，如白色会污染法线），用中性默认值。
GPU_CONST(GPU_UINT kGPUMaterialTexMask_BaseColor     = 1u << 0);  // 有 BaseColor 纹理
GPU_CONST(GPU_UINT kGPUMaterialTexMask_Normal        = 1u << 1);  // 有法线贴图
GPU_CONST(GPU_UINT kGPUMaterialTexMask_MetallicRough = 1u << 2);  // 有金属度/粗糙度纹理
GPU_CONST(GPU_UINT kGPUMaterialTexMask_Occlusion     = 1u << 3);  // 有 AO 纹理

GPU_CONST_END

#ifdef __cplusplus
// C++ 分支自清理：允许 C++ 头直接 include 本切片而不泄漏宏；Slang 分支由总入口收尾
#undef GPU_UINT
#undef GPU_INT
#undef GPU_STRUCT
#undef GPU_CONST
#undef GPU_CONST_BEGIN
#undef GPU_CONST_END
#endif

#endif // HUGENGINE_GPU_CONSTANTS
