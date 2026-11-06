#ifndef HUGENGINE_GPU_PUSHCONST_RT
#define HUGENGINE_GPU_PUSHCONST_RT
// ============================================================
// ShaderTypes/PushConstants/RT.slang — 光追 pass 的 push constant
//
// 内容：RTShadowPushConstant（108B，alignas 后 112B）· RTRayEffectPushConstant（112B）
// 后者被反射 / AO / GI 三个效果 pass 复用，未使用字段由各 pass 置 0。
// ============================================================

#include "ShaderTypes/GPUDefines.slang"

// --- RTShadowPushConstant: RT 阴影 Pass Push Constant（108B，C++/Slang 单一数据源）---
GPU_STRUCT RTShadowPushConstant {
    float4x4 invViewProj;           // [0..64]   逆 ViewProj（深度 → 世界坐标）
    float4   cameraPos;             // [64..80]  xyz=相机世界位置, w=近平面
    GPU_UINT dispatchDimX;          // [80]      DispatchRaysDimensions.x
    GPU_UINT dispatchDimY;          // [84]      DispatchRaysDimensions.y
    GPU_UINT frameIdx;              // [88]      帧索引（时域抖动用）
    float    maxShadowDist;         // [92]      最大阴影距离
    GPU_UINT shadowFlags;           // [96]      bit0=硬阴影, bit1=半分辨率, bit2=软阴影
    GPU_UINT lightCount;            // [100]     阴影光源数
    GPU_UINT softSPP;               // [104]     软阴影每光源采样数
};                                  // 共 108B（alignas(16) 后 112B）

// --- RTRayEffectPushConstant: RT 反射/AO/GI 效果共享 Push Constant（112B，alignas 后 112B）---
// 被 Reflection / AO / GI 三个效果 Pass 复用；未使用字段由各 Pass 置 0。
GPU_STRUCT RTRayEffectPushConstant {
    float4x4 invViewProj;           // [0..64]   逆 ViewProj（深度 → 世界坐标）
    float4   cameraPos;             // [64..80]  xyz=相机世界位置, w=近平面
    GPU_UINT dispatchDimX;          // [80]      DispatchRaysDimensions.x
    GPU_UINT dispatchDimY;          // [84]      DispatchRaysDimensions.y
    GPU_UINT frameIndex;            // [88]      帧索引（时域抖动 / 随机种子）
    float    maxDistance;           // [92]      最大追踪距离（AO=2m, GI=30m, Refl=500m）
    GPU_UINT sampleCount;           // [96]      AO/GI 每像素采样数（Reflection=1）
    float    maxRoughness;          // [100]     反射最大粗糙度（超过回退 IBL prefilter）
    GPU_UINT flags;                 // [104]     bit0=半分辨率, bit1=DDGI 是漫反射层栈的源（GI miss 不得回退 DDGI）
    GPU_UINT lightCount;            // [108]     直接光源数（ClosestHit 光照用）
};                                  // 共 112B（alignas(16) 后 112B）

#ifdef __cplusplus
// C++ 分支自清理：允许 C++ 头直接 include 本切片而不泄漏宏；Slang 分支由总入口收尾
#undef GPU_UINT
#undef GPU_INT
#undef GPU_STRUCT
#undef GPU_CONST
#undef GPU_CONST_BEGIN
#undef GPU_CONST_END
#endif

#endif // HUGENGINE_GPU_PUSHCONST_RT
