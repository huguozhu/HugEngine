#ifndef HUGENGINE_GPU_PUSHCONST_FORWARD
#define HUGENGINE_GPU_PUSHCONST_FORWARD
// ============================================================
// ShaderTypes/PushConstants/Forward.slang — 正向（Forward / Forward+）push constant
//
// 内容：PushConstantData（160B）· ShadowPushConstant（80B）· GBufferPushConstant（192B）
// C++ 侧尺寸断言在 Pipeline/Material.h。
// ============================================================

#include "ShaderTypes/GPUDefines.slang"

// ============================================================
// Push Constant 结构体
// ============================================================

// --- ForwardPushConstant: 前向管线每帧 Push Constant（160 字节）---
// C++ 端别名: PushConstantData
// Forward+: cluster 参数由 m_UseForwardPlus 控制，运行时切换传统 / Forward+ 路径
GPU_STRUCT PushConstantData {
    float4x4 viewProjMatrix;        // [0..64]   相机 VP 矩阵
    float4   cameraPosition;        // [64..80]  相机世界位置
    GPU_UINT lightCount;            // [80]      光源总数
    GPU_UINT objectIndex;           // [84]      GPU 对象索引
    GPU_UINT useInstanceID;         // [88]      1=GPU Driven (SV_InstanceID) 2=实例化网格（SV_InstanceID → 实例变换 SSBO）
    float    iblIntensity;          // [92]      IBL 环境光强度
    GPU_UINT clusterTilesX;         // [96]      Cluster X 方向 tile 数（0=传统模式）
    GPU_UINT clusterTilesY;         // [100]     Cluster Y 方向 tile 数
    float    clusterNear;           // [104]     近裁剪面
    float    clusterFar;            // [108]     远裁剪面
    float    clusterLogFactor;      // [112]     log(far/near)
    GPU_UINT useClustered;          // [116]     1=Forward+ / 0=传统 Forward
    GPU_UINT useBindlessMaterial;   // [120]     1=从 u_Materials[] 读材质参数（bindless），0=从 GPUObjectData 内联读
    float4   atmosphere;            // [128]     sunDir.xyz（指向太阳）+ turbidity（空中透视）
    GPU_UINT instanceSSBOHandle;    // [144]     实例化网格的实例变换 SSBO（bindless u_SSBO[] 句柄；useInstanceID=2 时有效）
    GPU_UINT instanceVisibleHandle; // [148]     逐实例剔除后的**可见实例索引列表**句柄（任务 25；0 = 未剔除，直接用 SV_InstanceID）
    GPU_UINT instancePad[2];        // [152..160] 对齐填充
};

// --- ShadowPushConstant: 阴影通道 Push Constant（80 字节）---
GPU_STRUCT ShadowPushConstant {
    float4x4 lightViewProj;         // 光源 VP 矩阵（非 Camera VP）
    GPU_UINT objectIndex;           // GPU 对象索引
    GPU_UINT _pad[3];               // 对齐到 80 字节
};

// --- GBufferPushConstant: GBuffer 渲染 Push Constant（192 字节）---
GPU_STRUCT GBufferPushConstant {
    float4x4 viewProjMatrix;        // [0..64]   当前帧 VP（含 TAA jitter）
    float4x4 prevViewProjMatrix;    // [64..128] 上一帧 VP（含**上帧** TAA jitter；velocity 因此自带抖动差）
    GPU_UINT objectIndex;           // [128]
    GPU_UINT useInstanceID;         // [132]     1=GPU Driven (SV_InstanceID)；2=实例化网格（任务 25）
    GPU_UINT instanceSSBOHandle;    // [136]     实例化网格的实例变换 SSBO 句柄（bindless u_SSBO[]）
    GPU_UINT instanceVisibleHandle; // [140]     逐实例剔除后的可见实例索引列表句柄（0=未剔除）
    GPU_UINT _pad[12];              // [144..192] 填充到 192B
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

#endif // HUGENGINE_GPU_PUSHCONST_FORWARD
