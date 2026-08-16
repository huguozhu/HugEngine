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
GPU_CONST(GPU_UINT kGPUBinding_BindlessSSBO     = 30);  // Bindless SSBO 数组（最高 binding，VARIABLE_COUNT）
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
GPU_CONST(GPU_UINT kGPUBinding_GBufferF      = 28);  // GBuffer disneyA（anisotropic/subsurface/specular/sheen，MRT5）
GPU_CONST(GPU_UINT kGPUBinding_GBufferG      = 29);  // GBuffer disneyB（clearcoat/clearcoatGloss/specularTint.rg，MRT6）
GPU_CONST(GPU_UINT kGPUBinding_SpotShadow    = 9);   // 聚光灯阴影
GPU_CONST(GPU_UINT kGPUBinding_Lights_DL     = 17);  // GPULight[] SSBO
GPU_CONST(GPU_UINT kGPUBinding_ShadowData_DL = 18);  // GPUShadowData[] SSBO
GPU_CONST(GPU_UINT kGPUBinding_SSGI          = 19);  // SSGI 间接光照
GPU_CONST(GPU_UINT kGPUBinding_SSAO_DL       = 20);  // SSAO
GPU_CONST(GPU_UINT kGPUBinding_SSR           = 21);  // SSR
GPU_CONST(GPU_UINT kGPUBinding_DDGIProbes    = 22);  // DDGI 探针

// --- Hybrid RT 管线 Binding 索引（Lighting 采样 RT 效果输出纹理）---
GPU_CONST(GPU_UINT kGPUBinding_RT_ShadowMask = 24);  // RT 阴影遮罩（R16_FLOAT）
GPU_CONST(GPU_UINT kGPUBinding_RT_Reflection = 25);  // RT 反射（RGBA16_FLOAT）
GPU_CONST(GPU_UINT kGPUBinding_RT_AO         = 26);  // RT 环境光遮蔽（R8_UNORM）
GPU_CONST(GPU_UINT kGPUBinding_RT_GI         = 27);  // RT 间接漫反射（RGBA16_FLOAT）

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
    float   shadowRadius;     // 光源半径（软阴影用，0=硬阴影）
};

// --- GPUShadowData: 阴影数据（256 字节，std430 布局）---
GPU_STRUCT GPUShadowData {
    float4x4 lightViewProj[3];  // [0..192] CSM 级联矩阵（光源 VP，非 Camera VP）
    float4   shadowParams;      // [192..208] x=bias, y=normalBias, z=strength, w=类型(0=Dir,1=Point,2=Spot)
    float4   splitDistances;    // [208..224] CSM 分割点 (Spot: xyz=方向, w=外锥角)
    float4   cameraForward;     // [224..240] 相机前向（CSM 级联选择用）
    float4   pointLightData;    // [240..256] xyz=光源世界位置, w=范围
};

// --- GPUObjectData: 物体数据（176 字节，std430 布局）---
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
    float    dielectricF0;          // [120]     电介质 F0（由 IOR 预计算）
    float4   disneyA;               // [128]     x=anisotropic, y=subsurface, z=specular, w=sheen
    float4   disneyB;               // [144]     x=clearcoat, y=clearcoatGloss, z=specularTint.r, w=specularTint.g
    float    disneyC;               // [160]     specularTint.b
    GPU_UINT _pad;                  // [164..168] 对齐到 168 → 取整 176（std430 float4 对齐）
};

// LightGridCell 参见 ClusteredShading.h（暂不纳入共享定义，避免重定义冲突）

// ============================================================
// Push Constant 结构体
// ============================================================

// --- ForwardPushConstant: 前向管线每帧 Push Constant（144 字节）---
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
    float4   atmosphere;            // sunDir.xyz（指向太阳）+ turbidity（空中透视；C++ 自动 16B 对齐，结构从 128→144B）
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
    GPU_UINT rtAOSource;            // [52]      0=SSAO, 1=RT AO
    GPU_UINT rtSpecularSource;      // [56]      0=SSR, 1=RT 反射
    GPU_UINT rtDiffuseSource;       // [60]      0=SSGI, 1=RT GI
    float4   atmosphere;            // [64..80] sunDir.xyz（指向太阳）+ turbidity（空中透视）
};

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
    GPU_UINT flags;                 // [104]     bit0=半分辨率
    GPU_UINT lightCount;            // [108]     直接光源数（ClosestHit 光照用）
};                                  // 共 112B（alignas(16) 后 112B）

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

// ---- 清理宏 ----
#undef GPU_UINT
#undef GPU_INT
#undef GPU_STRUCT
#undef GPU_CONST
#undef GPU_CONST_BEGIN
#undef GPU_CONST_END

#endif // HUGENGINE_SHADER_TYPES
