#ifndef HUGENGINE_GPU_PUSHCONST_PT
#define HUGENGINE_GPU_PUSHCONST_PT
// ============================================================
// ShaderTypes/PushConstants/PT.slang — 全路径追踪 / ReSTIR 的 push constant 与蓄水池
//
// 内容：PTPushConstant（176B）· PTReservoir（32B）· ReSTIRPushConstant（128B）
// ============================================================

#include "ShaderTypes/GPUDefines.slang"

// --- PTPushConstant: 全路径追踪 RayGen Push Constant（176B）---
// 被 PT_Full.rgen.slang（RayGen）与 PT_Full.rchit.slang（ClosestHit）共享。
// 注意：ptDepth 存线性视图深度（-primaryHitT），故 RayGen 不需要当前帧 VP，
// 只需 invViewProj（相机光线）+ prevViewProj（velocity 输出）。
GPU_STRUCT PTPushConstant {
    float4x4 invViewProj;           // [0..64]    逆 ViewProj（相机光线反投影）
    float4x4 prevViewProj;          // [64..128]  上一帧 VP（velocity 重投影）
    float4   cameraPos;             // [128..144] xyz=相机世界位置
    GPU_UINT maxBounces;            // [144]      最大弹射次数（1-8）
    GPU_UINT sampleCount;           // [148]      每像素 SPP（1-8，时域累积）
    GPU_UINT frameIndex;            // [152]      帧索引（抖动/随机种子）
    GPU_UINT lightCount;            // [156]      直接光源数
    float    skyIntensity;          // [160]      天空强度
    GPU_UINT dispatchDimX;          // [164]      DispatchRaysDimensions.x
    GPU_UINT dispatchDimY;          // [168]      DispatchRaysDimensions.y
    GPU_UINT flags;                 // [172]      bit0=useReSTIR, bit1=MIS, bit2=俄罗斯轮盘赌, bit3=NEE
};                                  // 共 176B（alignas(16) 后 176B）

// --- PTReservoir: ReSTIR DI 蓄水池（32B，std430）---
// Initial/Temporal/Final 三份 SSBO 均为此布局；M==0 表示空蓄水池（首帧/失效）。
// 注意：lightPos 必须用 float4 —— C++ 端 glm::vec3 在
// GLM_FORCE_DEFAULT_ALIGNED_GENTYPES 下为 16B（带填充），与 GPU 12B 布局不匹配。
GPU_STRUCT PTReservoir {
    GPU_UINT lightIndex;            // 选中的光源索引（无效时 = 0xFFFFFFFF）
    float    weightSum;             // 目标函数权重和（Σp̂）
    GPU_UINT M;                     // 累计候选样本数
    float    W;                     // 选中样本的目标函数值（时空复用后已对当前像素重评估）
    float4   lightPos;              // xyz=采样光源位置（方向光 = 任意远处点），w 未用
};

// --- ReSTIRPushConstant: ReSTIR compute Pass 共享 Push Constant（128B）---
// 被 ReSTIR_Init / ReSTIR_Temporal / ReSTIR_Spatial 三个 compute 着色器复用。
GPU_STRUCT ReSTIRPushConstant {
    float4x4 invViewProj;           // [0..64]    逆 ViewProj（深度重建世界坐标）
    float4   cameraPos;             // [64..80]   xyz=相机世界位置
    GPU_UINT dispatchDimX;          // [80]       DispatchRaysDimensions.x
    GPU_UINT dispatchDimY;          // [84]       DispatchRaysDimensions.y
    GPU_UINT frameIndex;            // [88]       帧索引（随机种子）
    GPU_UINT lightCount;            // [92]       直接光源数
    GPU_UINT candidateCount;        // [96]       初始采样候选数 M（WRS 候选数）
    GPU_UINT spatialRadius;         // [100]      空间复用采样半径（像素）
    GPU_UINT spatialSamples;        // [104]      空间复用采样数
    float    maxDistance;           // [108]      去遮挡验证容差（深度差，米）
    GPU_UINT flags;                 // [112]      bit0=temporal, bit1=spatial, bit2=historyValid
};                                  // 共 128B（alignas(16) 后 128B）

#ifdef __cplusplus
// C++ 分支自清理：允许 C++ 头直接 include 本切片而不泄漏宏；Slang 分支由总入口收尾
#undef GPU_UINT
#undef GPU_INT
#undef GPU_STRUCT
#undef GPU_CONST
#undef GPU_CONST_BEGIN
#undef GPU_CONST_END
#endif

#endif // HUGENGINE_GPU_PUSHCONST_PT
