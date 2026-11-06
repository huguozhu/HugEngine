#ifndef HUGENGINE_GPU_SCENE_TYPES
#define HUGENGINE_GPU_SCENE_TYPES
// ============================================================
// ShaderTypes/GPUSceneTypes.slang — 场景侧 GPU 结构（C++/Slang 共享）
//
// 内容：光源 / 阴影 / 物体 / 材质（std430 SSBO 元素）+ 间接绘制命令 / 场景物体 /
//       LightGrid / 簇引用 / RT 命中点光源 UBO 等小型 GPU 结构。
// C++ 侧的 sizeof 断言钉在消费处（Material.h、GPUScene.h、NaniteTypes.h 等）。
// ============================================================

#include "ShaderTypes/GPUDefines.slang"

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
    float4   shadowParams;      // [192..208] x=bias, y=normalBias, z=strength, w=类型(0=Dir,1=Point,2=Spot,3=Rect)
    float4   splitDistances;    // [208..224] CSM 分割点 (Spot: xyz=方向, w=外锥角)
    float4   cameraForward;     // [224..240] 相机前向（CSM 级联选择用）
    float4   pointLightData;    // [240..256] xyz=光源世界位置, w=范围
};

// --- GPUObjectData: 物体数据（208 字节，std430 布局）---
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
    GPU_UINT textureMask;           // [124]     纹理存在位掩码（bit=槽有真实纹理）
    float4   disneyA;               // [128]     x=anisotropic, y=subsurface, z=specular, w=sheen
    float4   disneyB;               // [144]     x=clearcoat, y=clearcoatGloss, z=specularTint.r, w=specularTint.g
    float    disneyC;               // [160]     specularTint.b
    // 物体**世界空间** AABB（原任务 31，该任务已取消；字段保留）：光照图的程序化展开需要它。
    // 【为什么放在这里而不是顶点里】展开要的是"这个物体有多大"，是**每物体**的量：放进
    // 对象缓冲后，GBuffer 就能直接用（世界位置 + 世界法线 + 本物体 AABB）算出光照图键
    // （页 = objectIndex、tile = 主导轴、tile 内 = 归一化投影），不需要给网格加 UV1 通道，
    // 也不需要额外的逐顶点数据。烘培端用同一套公式，保证写入与查找同源。
    float4   boundsMin;             // [168]     xyz = 世界 AABB 最小角
    float4   boundsMax;             // [184]     xyz = 世界 AABB 最大角
};

// --- GPUMaterialData: per-material 材质数据（bindless SSBO 元素，std430）---
// 从 GPUObjectData 抽出材质参数部分（去掉 worldMatrix / materialID / object 专属字段），
// 同一材质多物体共享一份，按 materialID>>2 索引（每材质占 4 个纹理槽）。
GPU_STRUCT GPUMaterialData {
    float4   baseColorFactor;       // 基础色因子
    float4   emissiveFactor;        // 自发光因子
    float    metallicFactor;        // 金属度
    float    roughnessFactor;       // 粗糙度
    float    aoFactor;              // 环境光遮蔽
    float    alphaCutoff;           // alpha 截断阈值
    GPU_UINT materialFlags;         // 材质标志位（双面/AlphaMask/Unlit）
    GPU_UINT textureMask;           // 纹理存在位掩码（bit=槽有真实纹理）
    float    dielectricF0;          // 电介质 F0（由 IOR 预计算）
    float4   disneyA;               // x=anisotropic, y=subsurface, z=specular, w=sheen
    float4   disneyB;               // x=clearcoat, y=clearcoatGloss, z=specularTint.r, w=specularTint.g
    float    disneyC;               // specularTint.b
    GPU_UINT _pad;                  // 对齐填充
};

// ============================================================
// 小型 GPU 结构（间接绘制 / 场景物体 / LightGrid / 簇引用）
//
// 这些结构原先在各 pass 里各写一份，2026-10-10 收敛到共享契约；现随契约一起搬进本切片。
// 注：IndirectCmd / LightUB / LightGridCell / ClusterRef 的大小**不是** 16 的倍数，
// 故用普通 struct（见 GPUDefines.slang 的「何时用 GPU_STRUCT」）。
// ============================================================

// --- 间接绘制命令（20 B）---
// 必须与 `VkDrawIndexedIndirectCommand` **二进制兼容**（5×4 = 20 B，字段顺序
// indexCount / instanceCount / firstIndex / vertexOffset / firstInstance）；C++ 侧对应
// `IndirectDrawCommand`（`Culling/InstancedCull` 一带）与
// `NaniteIndirectCommand`（`Engine/Render/Nanite/NaniteTypes.h`，带逐字段 offsetof 断言），
// 以及各处直接用 `VkDrawIndexedIndirectCommand` 的路径。此前 6 个 Slang 文件各写一份。
struct IndirectCmd {
    GPU_UINT indexCount;      // 偏移 0
    GPU_UINT instanceCount;   // 偏移 4
    GPU_UINT firstIndex;      // 偏移 8
    GPU_INT  vertexOffset;    // 偏移 12（**有符号**，与 Vulkan 一致）
    GPU_UINT firstInstance;   // 偏移 16
};

// --- GPU 场景物体（128 B，std430）---
// C++ 侧原在 `Engine/Render/Pipeline/GPUScene.h` 重复定义一份（并在那里 static_assert 128）。
// 现由本切片提供唯一定义；GPUScene.h 在 `he::render` 内直接使用它（无需别名）。
GPU_STRUCT GPUSceneObject {
    float4x4 localToWorld;      // [0..64)
    float4   boundsMin;         // [64..80)
    float4   boundsMax;         // [80..96)
    GPU_UINT meshIndex;         // [96]
    GPU_UINT materialIndex;     // [100]
    GPU_UINT objectID;          // [104]
    GPU_UINT visibilityFlags;   // [108]
    GPU_UINT indexCount;        // [112] IndirectDraw 参数
    GPU_UINT firstIndex;        // [116] IndirectDraw 参数
    GPU_INT  vertexOffset;      // [120] IndirectDraw 参数
    GPU_UINT _pad;              // [124..128)（C++ 侧曾是 `_pad[1]`，布局相同，统一为标量）
};

// --- RT 命中点光源 UBO（32 B = 2×float4）---
// binding 1 的命中点光源 UBO（顶点拉取 / Sponza 路径）。
// 与 `RTPass::UpdateLightBuffer` 的写入端逐字段一致（那边按 `i * 32` 步长 memcpy 两个 float4）。
// ⚠ 注意与 `RT_HitCommon.slang` 的 `RTHitLight`（48 B = 3×float4）**不是**同一个结构体：
//   后者服务 RTEffectPass 的 GI/反射 ClosestHit 路径，前者服务 Sponza/顶点拉取路径。
struct LightUB {
    float4 ci;   // rgb=颜色, w=强度
    float4 dt;   // xyz=方向, w=类型（0=Dir,1=Point,2=Spot）
};

// --- RT 命中点光源（ClosestHit 用，48 B = 3×float4）---
// 由 C++ 每帧从 `GPULight[]` SSBO **显式抽取**成 UBO（已知 slangc/driver 在 ClosestHit 里
// 访问 StructuredBuffer 会 GPU fault），因此字段顺序**不是** GPULight 的顺序：
// colorIntensity → positionRange → directionType。
// 此前 C++ `RTEffectPass.h` 的 `RTHitLightGPU` 与 Slang `RT_HitCommon.slang` 的 `RTHitLight`
// 各写一份（只靠一个 sizeof 断言对钉），现统一到本切片，两端共用同一类型。
GPU_STRUCT RTHitLight {
    float4 colorIntensity;   // rgb=颜色, w=强度
    float4 positionRange;    // xyz=位置, w=范围
    float4 directionType;    // xyz=方向, w=类型 (0=Dir,1=Point,2=Spot)
};

// --- RT 阴影光源（RT_Shadow.rgen 用，64 B）---
// 同样由 C++ 每帧从 `GPULight[]` 显式抽取（raygen 不访问 SSBO）。
// 此前 C++ `RTShadowPass.cpp` 的 `ShadowLightGPU` 与 Slang `RT_Shadow.rgen.slang` 的
// `ShadowLight` 各写一份，现统一到本切片。
GPU_STRUCT ShadowLight {
    float4 pos_type;       // xyz=位置/方向, w=类型(0=方向光, 1=点光源, 2=聚光灯)
    float4 color_radius;   // rgb=颜色, w=强度（用于剔除无效光源）
    float4 spotDir_angle;  // xyz=聚光方向, w=cos(内锥角)（聚光灯专用）
    float4 radius;         // x=光源半径（软阴影）
};

// --- Bindless 调用数据（48 B）---
// `RT_Bindless.rcall.slang` 与 `RT_Sponza.rchit.slang` 之间传递，纯 Slang 内部使用。
GPU_STRUCT BindlessCallData {
    GPU_UINT materialID;
    float2   uv;
    float3   albedo;
    float3   normal;
    float    metallic;
    float    roughness;
    float    ao;
};

// --- LightGrid（8 B）---
// Clustered Shading / Forward+ 的 LightGrid：每 cluster 只迭代影响它的光源
// （配 LightIndexList 使用）。C++ 侧原在 `ClusteredShading.h` 作为嵌套结构体重复定义，
// 现改为别名引用本定义。
struct LightGridCell {
    GPU_UINT offset;   // 在 LightIndexList 中的起始偏移
    GPU_UINT count;    // 影响该 cluster 的光源数量
};

// --- Nanite 簇引用（8 B）---
// `Nanite_ClusterBVH.comp.slang` ↔ `Nanite_SoftRasterCommon.slang` 共用；纯 Slang。
// 与 C++ 侧 `NaniteVisibleClusterRef` 逐字段一致（8 B）。
struct ClusterRef {
    GPU_UINT instance;
    GPU_UINT cluster;
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

#endif // HUGENGINE_GPU_SCENE_TYPES
