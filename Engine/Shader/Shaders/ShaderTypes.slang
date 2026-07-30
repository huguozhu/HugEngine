// ============================================================
// ShaderTypes.slang — C++ 与 Slang 共用的 GPU 结构体定义
//
// C++ 使用方式:  #include "ShaderTypes.slang" (需要 Math.h / cstdint 先包含)
// Slang 使用方式: #include "ShaderTypes.slang" (-I 指向 Shaders 目录)
//
// 修改此文件时，必须确保 C++ 端 static_assert 尺寸验证全部通过。
// ============================================================

#ifndef HUGENGINE_SHADER_TYPES
#define HUGENGINE_SHADER_TYPES

// ---- C++ / Slang 类型兼容 ----
#ifdef __cplusplus
    // C++: Types.h 已提供 uint32_t / int32_t（通过 <cstdint>）
    #define GPU_UINT  uint32_t    // Slang 'uint' → C++ uint32_t
    #define GPU_INT   int32_t     // Slang 'int'  → C++ int32_t
    #define GPU_STRUCT struct alignas(16)  // Slang 自动对齐，C++ 需显式声明

    // GPU 常量（C++: static constexpr 内部链接，Slang: static const）
    #define GPU_CONST_BEGIN
    #define GPU_CONST_END
    #define GPU_CONST(decl) static constexpr decl
#else
    // Slang/HLSL: 原生 GPU 类型
    #define GPU_UINT  uint
    #define GPU_INT   int
    #define GPU_STRUCT struct       // Slang 不需要 alignas

    #define GPU_CONST_BEGIN
    #define GPU_CONST_END
    #define GPU_CONST(decl) static const decl
#endif

// ============================================================
// GPU 常量定义（C++ 与 Slang 共享，单一数据源）
// 修改此文件时确保 C++ 端 static_assert 和编译同时通过
// ============================================================
GPU_CONST_BEGIN

// --- Descriptor Set 索引 ---
GPU_CONST(GPU_UINT kGPUDescSet_PerFrame = 0);   // set=0: 逐帧数据
GPU_CONST(GPU_UINT kGPUDescSet_Material = 1);   // set=1: 逐材质数据
GPU_CONST(GPU_UINT kGPUDescSet_Bindless = 2);   // set=2: 无绑定资源（TLAS 等）

// --- 共享 Binding 索引（set=0，Forward / Deferred / GBuffer 共用）---
GPU_CONST(GPU_UINT kGPUBinding_ObjectData       = 2);   // Object SSBO
GPU_CONST(GPU_UINT kGPUBinding_BindlessTextures = 5);   // Bindless 纹理数组
GPU_CONST(GPU_UINT kGPUBinding_BindlessSamplers = 6);   // Bindless 采样器数组
GPU_CONST(GPU_UINT kGPUBinding_LightGrid        = 7);   // 光源网格 SSBO
GPU_CONST(GPU_UINT kGPUBinding_LightIndexList   = 8);   // 光源索引列表 SSBO

// --- Forward 管线 Binding 索引 ---
GPU_CONST(GPU_UINT kGPUBinding_Lights          = 1);   // GPULight[] SSBO
GPU_CONST(GPU_UINT kGPUBinding_ShadowData      = 3);   // GPUShadowData[] SSBO
GPU_CONST(GPU_UINT kGPUBinding_ShadowMap0      = 4);   // CSM cascade 0
GPU_CONST(GPU_UINT kGPUBinding_PointShadow     = 9);   // 点光源阴影 Cubemap
GPU_CONST(GPU_UINT kGPUBinding_ShadowMap1      = 10);  // CSM cascade 1
GPU_CONST(GPU_UINT kGPUBinding_ShadowMap2      = 11);  // CSM cascade 2
GPU_CONST(GPU_UINT kGPUBinding_IrradianceMap   = 12);  // IBL 辐照度图
GPU_CONST(GPU_UINT kGPUBinding_PrefilterMap    = 13);  // IBL 预滤波环境图
GPU_CONST(GPU_UINT kGPUBinding_BRDF_LUT        = 14);  // BRDF 积分 LUT
GPU_CONST(GPU_UINT kGPUBinding_RSMPosition     = 15);  // RSM 位置图
GPU_CONST(GPU_UINT kGPUBinding_RSMFlux         = 16);  // RSM 通量图

// --- Deferred 管线 Binding 索引 ---
GPU_CONST(GPU_UINT kGPUBinding_GBufferA      = 0);   // GBuffer albedo+metallic
GPU_CONST(GPU_UINT kGPUBinding_GBufferB      = 1);   // GBuffer normal+roughness
GPU_CONST(GPU_UINT kGPUBinding_GBufferC      = 2);   // GBuffer emissive+ao
GPU_CONST(GPU_UINT kGPUBinding_Depth         = 3);   // 深度缓冲
GPU_CONST(GPU_UINT kGPUBinding_GBufferE      = 23);  // GBuffer worldPos（MRT4）
GPU_CONST(GPU_UINT kGPUBinding_SpotShadow    = 9);   // 聚光灯阴影
GPU_CONST(GPU_UINT kGPUBinding_Lights_DL     = 17);  // GPULight[] SSBO
GPU_CONST(GPU_UINT kGPUBinding_ShadowData_DL = 18);  // GPUShadowData[] SSBO
GPU_CONST(GPU_UINT kGPUBinding_SSGI          = 19);  // SSGI 间接光照
GPU_CONST(GPU_UINT kGPUBinding_SSAO_DL       = 20);  // SSAO
GPU_CONST(GPU_UINT kGPUBinding_SSR           = 21);  // SSR
GPU_CONST(GPU_UINT kGPUBinding_DDGIProbes    = 22);  // DDGI 探针

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

GPU_CONST_END

// ---- 清理宏（GPU struct 定义后统一清理）----

// ============================================================
// Storage Buffer 结构体（StructuredBuffer<T>）
// ============================================================

// --- GPULight: 光源数据（64 字节，std430 布局）---
GPU_STRUCT GPULight {
    float4 colorIntensity;    // xyz=颜色, w=强度
    float4 directionType;     // xyz=方向, w=类型 (0=Dir,1=Point,2=Spot)
    float4 positionRange;     // xyz=位置, w=范围
    float2 coneAngles;        // x=内锥角, y=外锥角 (Spot 专用)
    GPU_INT shadowIndex;      // -1=无阴影, >=0 指向 u_ShadowData 数组
    GPU_INT _padLight;
};

// --- GPUShadowData: 阴影数据（256 字节，std430 布局）---
GPU_STRUCT GPUShadowData {
    float4x4 lightViewProj[3];  // [0..192] CSM 级联矩阵（光源 VP，非 Camera VP）
    float4   shadowParams;      // [192..208] x=bias, y=normalBias, z=strength, w=类型(0=Dir,1=Point,2=Spot)
    float4   splitDistances;    // [208..224] CSM 分割点 (Spot: xyz=方向, w=外锥角)
    float4   cameraForward;     // [224..240] 相机前向（CSM 级联选择用）
    float4   pointLightData;    // [240..256] xyz=光源世界位置, w=范围
};

// --- GPUObjectData: 物体数据（128 字节，std430 布局）---
GPU_STRUCT GPUObjectData {
    float4x4 worldMatrix;           // [0..64]   世界矩阵
    float4   baseColorFactor;       // [64..80]  基础色因子
    float4   emissiveFactor;        // [80..96]  自发光因子
    float    metallicFactor;        // [96]
    float    roughnessFactor;       // [100]
    float    aoFactor;              // [104]
    float    alphaCutoff;           // [108]
    GPU_UINT materialFlags;         // [112]
    GPU_UINT materialID;            // [116]     bindless 纹理基索引
    GPU_UINT _pad[2];               // [120..128] 对齐
};

// LightGridCell 参见 ClusteredShading.h（暂不纳入共享定义，避免重定义冲突）

// ============================================================
// Push Constant 结构体
// ============================================================

// --- ForwardPushConstant: 前向管线每帧 Push Constant（128 字节）---
// C++ 端别名: PushConstantData
// Forward+: cluster 参数由 m_UseForwardPlus 控制，运行时切换传统 / Forward+ 路径
GPU_STRUCT PushConstantData {
    float4x4 viewProjMatrix;        // [0..64]   相机 VP 矩阵
    float4   cameraPosition;        // [64..80]  相机世界位置
    GPU_UINT lightCount;            // [80]      光源总数
    GPU_UINT objectIndex;           // [84]      GPU 对象索引
    GPU_UINT useInstanceID;         // [88]      1=GPU Driven (SV_InstanceID)
    float    iblIntensity;          // [92]      IBL 环境光强度
    GPU_UINT clusterTilesX;         // [96]      Cluster X 方向 tile 数（0=传统模式）
    GPU_UINT clusterTilesY;         // [100]     Cluster Y 方向 tile 数
    float    clusterNear;           // [104]     近裁剪面
    float    clusterFar;            // [108]     远裁剪面
    float    clusterLogFactor;      // [112]     log(far/near)
    GPU_UINT useClustered;          // [116]     1=Forward+ / 0=传统 Forward
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
    float4x4 prevViewProjMatrix;    // [64..128] 上一帧 VP（无 jitter，velocity 用）
    GPU_UINT objectIndex;           // [128]
    GPU_UINT useInstanceID;         // [132]     1=GPU Driven (SV_InstanceID)
    GPU_UINT _pad[14];              // [136..192] 填充到 192B
};

// --- DeferredLightingPushConstant: 延迟光照 Push Constant（64 字节）---
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
    GPU_UINT _pad[4];               // [48..64]  对齐到 64B
};

// ---- 清理宏 ----
#undef GPU_UINT
#undef GPU_INT
#undef GPU_STRUCT
#undef GPU_CONST
#undef GPU_CONST_BEGIN
#undef GPU_CONST_END

#endif // HUGENGINE_SHADER_TYPES
