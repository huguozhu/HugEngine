#ifndef HUGENGINE_GPU_PUSHCONST_DEFERRED
#define HUGENGINE_GPU_PUSHCONST_DEFERRED
// ============================================================
// ShaderTypes/PushConstants/Deferred.slang — 延迟光照 pass 的 push constant
//
// 内容：DeferredLightingPushConstant（80B）。世界坐标直接从 GBuffer MRT4 读取，
// 不再需要 Camera invViewProj 做深度重建。
// ============================================================

#include "ShaderTypes/GPUDefines.slang"

// --- DeferredLightingPushConstant: 延迟光照 Push Constant（80 字节）---
// 注意：worldPos 直接从 GBuffer MRT4 读取，不再需要 Camera invViewProj 做深度重建
GPU_STRUCT DeferredLightingPushConstant {
    float4   cameraPosition;        // [0..16]   相机世界位置（仅用于 V 方向计算）
    GPU_UINT lightCount;            // [16]
    float    iblIntensity;          // [20]      IBL 环境光强度
    GPU_UINT clusterTilesX;         // [24]      Clustered shading X tile 数
    GPU_UINT clusterTilesY;         // [28]      Clustered shading Y tile 数
    float    clusterNear;           // [32]      近裁剪面
    float    clusterFar;            // [36]      远裁剪面
    float    clusterLogFactor;      // [40]      log(far/near)
    GPU_UINT useClustered;          // [44]      1=启用 Clustered Shading
    GPU_UINT rtShadowSource;        // [48]      0=CSM, 1=RT 阴影（RT 遮罩）
    // 注：rtAOSource / rtSpecularSource / rtDiffuseSource / useScreenGI / ddgiScale
    // 已随 Wave 1「按源 id 分派」移除——各源的参与与模式完全由层栈
    // （GIBlendParams UBO 的源数组）表达，不再需要 shader 侧的源选择开关。
    float    giIntensity;           // [52]      间接漫反射 GI 总强度（层栈合成总倍率）
    float    aoIntensity;           // [56]      AO 强度（0=无 AO，1=完整 AO）
    GPU_UINT _pad0;                 // [60]      对齐到 16 字节边界
    float4   atmosphere;            // [64..80] sunDir.xyz（指向太阳）+ turbidity（空中透视）
};

#ifdef __cplusplus
// C++ 分支自清理：允许 C++ 头直接 include 本切片而不泄漏宏；Slang 分支由总入口收尾
#undef GPU_UINT
#undef GPU_INT
#undef GPU_STRUCT
#undef GPU_CONST
#undef GPU_CONST_BEGIN
#undef GPU_CONST_END
#endif

#endif // HUGENGINE_GPU_PUSHCONST_DEFERRED
